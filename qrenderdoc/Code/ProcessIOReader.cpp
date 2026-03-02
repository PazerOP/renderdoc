/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "ProcessIOReader.h"
#include <QPointer>
#include "Code/QRDUtils.h"
#include "control_types.h"

#if defined(Q_OS_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

ProcessIOReader::ProcessIOReader(Process::ProcessIOHandles *handles, QObject *parent)
    : QObject(parent), m_Handles(handles)
{
}

ProcessIOReader::~ProcessIOReader()
{
  stop();
}

void ProcessIOReader::start(OutputCallback callback)
{
  m_Callback = callback;

  if(!m_Handles)
    return;

#if defined(Q_OS_WIN32)
  bool hasStdout = (m_Handles->stdoutRead != NULL);
  bool hasStderr = (m_Handles->stderrRead != NULL);
#else
  bool hasStdout = (m_Handles->stdoutRead >= 0);
  bool hasStderr = (m_Handles->stderrRead >= 0);
#endif

  if(hasStdout)
  {
    m_StdoutThread = new LambdaThread([this]() { readLoop(false); });
    m_StdoutThread->setName(lit("ProcessIO_stdout"));
    m_StdoutThread->start();
  }

  if(hasStderr)
  {
    m_StderrThread = new LambdaThread([this]() { readLoop(true); });
    m_StderrThread->setName(lit("ProcessIO_stderr"));
    m_StderrThread->start();
  }
}

void ProcessIOReader::stop()
{
  // Close the handles so the read loops unblock and exit
  if(m_Handles)
    m_Handles->Close();

  if(m_StdoutThread)
  {
    m_StdoutThread->wait();
    m_StdoutThread->deleteLater();
    m_StdoutThread = nullptr;
  }

  if(m_StderrThread)
  {
    m_StderrThread->wait();
    m_StderrThread->deleteLater();
    m_StderrThread = nullptr;
  }

  delete m_Handles;
  m_Handles = nullptr;
}

void ProcessIOReader::readLoop(bool isStderr)
{
  char buf[4096];

  for(;;)
  {
    int bytesRead = 0;

#if defined(Q_OS_WIN32)
    void *handle = isStderr ? m_Handles->stderrRead : m_Handles->stdoutRead;
    if(handle == NULL)
      break;

    DWORD dwRead = 0;
    BOOL ok = ReadFile((HANDLE)handle, buf, sizeof(buf), &dwRead, NULL);
    bytesRead = ok ? (int)dwRead : -1;
#else
    int fd = isStderr ? m_Handles->stderrRead : m_Handles->stdoutRead;
    if(fd < 0)
      break;

    bytesRead = (int)read(fd, buf, sizeof(buf));
#endif

    if(bytesRead <= 0)
      break;

    QString text = QString::fromUtf8(buf, bytesRead);
    QPointer<ProcessIOReader> self(this);
    bool stderr_flag = isStderr;

    GUIInvoke::call(this, [self, stderr_flag, text]() {
      if(self && self->m_Callback)
        self->m_Callback(stderr_flag, text);
    });
  }
}
