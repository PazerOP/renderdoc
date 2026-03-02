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

#pragma once

#include <QObject>
#include <functional>

namespace Process
{
struct ProcessIOHandles;
}

class LambdaThread;

// Reads stdout/stderr from a launched process's pipe handles on background threads
// and invokes a callback on the GUI thread when data arrives.
class ProcessIOReader : public QObject
{
public:
  typedef std::function<void(bool isStderr, const QString &text)> OutputCallback;

  explicit ProcessIOReader(Process::ProcessIOHandles *handles, QObject *parent = nullptr);
  ~ProcessIOReader();

  void start(OutputCallback callback);
  void stop();

private:
  void readLoop(bool isStderr);

  Process::ProcessIOHandles *m_Handles;
  LambdaThread *m_StdoutThread = nullptr;
  LambdaThread *m_StderrThread = nullptr;
  OutputCallback m_Callback;
};
