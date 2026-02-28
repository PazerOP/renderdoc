# Process I/O Streams Feature Plan

## Overview
Add the ability to see stdout/stderr from processes launched by RenderDoc, displayed in the LiveCapture panel. Support both local and remote launches. Stdin writing deferred to a follow-up.

## Architecture Decision

**Approach: Pipe-based I/O capture with forwarding through the target control protocol for remote, and direct pipe reading for local.**

The key insight is that stdout/stderr pipes exist at the OS level between the parent process and the child. For local launches, the parent is qrenderdoc itself. For remote launches, the parent is the remote server process (`renderdoccmd`). In both cases, we need to:

1. Create pipes when launching the process (already exists for `LaunchProcess`, needs to be added to `LaunchAndInjectIntoProcess`)
2. Read from pipes asynchronously on a background thread
3. Forward the data to the UI

For **local** launches: A new I/O reader thread in the LiveCapture reads directly from the pipe file descriptors/HANDLEs.

For **remote** launches: The remote server reads from pipes and sends the data as new target control messages (or remote server messages) to the client.

Since the target control protocol runs inside the *injected* process (not the parent), we cannot easily route I/O through it. Instead, for remote, we extend the remote server protocol to stream process I/O alongside the existing connection.

## Implementation Steps

### Step 1: Extend `LaunchAndInjectIntoProcess` to create I/O pipes

**Files:** `renderdoc/os/os_specific.h`, `renderdoc/os/posix/posix_process.cpp`, `renderdoc/os/win32/win32_process.cpp`

- Add a new struct `ProcessIOHandles` to hold platform-specific pipe read handles:
  ```cpp
  struct ProcessIOHandles
  {
  #if ENABLED(RDOC_WIN32)
    void *stdoutRead = nullptr;  // HANDLE
    void *stderrRead = nullptr;  // HANDLE
  #else
    int stdoutRead = -1;
    int stderrRead = -1;
  #endif
    void Close();
  };
  ```

- Add an optional `ProcessIOHandles *ioHandles` parameter to `LaunchAndInjectIntoProcess()`:
  ```cpp
  rdcpair<RDResult, uint32_t> LaunchAndInjectIntoProcess(
      ..., bool waitForExit,
      ProcessIOHandles *ioHandles = NULL);
  ```

- **POSIX implementation:** When `ioHandles != NULL`, create pipes with `pipe()`, pass them to `RunProcess()` (which already accepts `stdoutPipe`/`stderrPipe`), and store the read ends in `ioHandles`. Don't close the read ends.

- **Windows implementation:** When `ioHandles != NULL`, pass the `&hChildStdOutput_Rd` and `&hChildStdError_Rd` to `RunProcess()` (which already accepts these), and store the read HANDLEs in `ioHandles`. Don't close them.

### Step 2: Expose I/O handles through the API layers

**Files:** `renderdoc/api/replay/control_types.h`, `renderdoc/replay/entry_points.cpp`, `renderdoc/api/replay/renderdoc_replay.h`

- Extend `ExecuteResult` to carry an opaque pointer to `ProcessIOHandles`:
  ```cpp
  struct ExecuteResult
  {
    ResultDetails result;
    uint32_t ident;
    // Internal-only, not exposed to Python/SWIG. Holds pipe handles for local launches.
    ProcessIOHandles *ioHandles = nullptr;
  };
  ```
  Note: `ioHandles` is internal-only and not reflected/serialised.

- Modify `RENDERDOC_ExecuteAndInject()` to create a `ProcessIOHandles` on the heap, pass it to `LaunchAndInjectIntoProcess()`, and store it in `ExecuteResult::ioHandles`.

### Step 3: Thread process I/O through ReplayManager

**Files:** `qrenderdoc/Code/ReplayManager.h`, `qrenderdoc/Code/ReplayManager.cpp`

- `ReplayManager::ExecuteAndInject()` already returns `ExecuteResult`. The `ioHandles` pointer will be passed through naturally since it's a field of the struct.

### Step 4: Add I/O reader infrastructure

**Files:** New file `qrenderdoc/Code/ProcessIOReader.h` and `qrenderdoc/Code/ProcessIOReader.cpp`

- Create a `ProcessIOReader` class that:
  - Takes ownership of `ProcessIOHandles`
  - Runs two background threads (or uses `select`/`WaitForMultipleObjects`) to read from stdout and stderr pipes
  - Accumulates output into thread-safe buffers
  - Emits signals (or uses callbacks) when new data is available
  - Handles pipe closure gracefully (process exit)

```cpp
class ProcessIOReader : public QObject
{
  Q_OBJECT
public:
  ProcessIOReader(ProcessIOHandles *handles, QObject *parent = nullptr);
  ~ProcessIOReader();

  void start();
  void stop();

signals:
  void outputReceived(bool isStderr, const QString &text);
  void processFinished();

private:
  void readThread(bool isStderr);
  ProcessIOHandles *m_Handles;
  LambdaThread *m_StdoutThread = nullptr;
  LambdaThread *m_StderrThread = nullptr;
  std::atomic<bool> m_Running{false};
};
```

### Step 5: Extend remote server protocol for I/O forwarding

**Files:** `renderdoc/core/remote_server.cpp`, `renderdoc/core/remote_server.h`

- When the remote server handles `ExecuteAndInject`, create pipes and start a background thread that reads from them.
- Add a new `RemoteServerPacket` type: `ProcessOutput`
- The remote server's I/O reader thread sends `ProcessOutput` packets containing the data and a flag for stdout vs stderr.
- On the client side (`RemoteServer` class), when receiving `ProcessOutput`, buffer it and expose via a new method.
- Add to `IRemoteServer`:
  ```cpp
  virtual bool HasProcessIOData() = 0;
  virtual rdcstr GetProcessIOData(bool &isStderr) = 0;
  ```

  Or, more simply, add a callback mechanism that the LiveCapture can register to receive I/O data from the remote server.

### Step 6: Modify LiveCapture to display process output

**Files:** `qrenderdoc/Windows/Dialogs/LiveCapture.h`, `qrenderdoc/Windows/Dialogs/LiveCapture.cpp`, `qrenderdoc/Windows/Dialogs/LiveCapture.ui`

**UI Changes:**
- Add a collapsible `QTextEdit` (read-only) at the bottom of the LiveCapture panel, below the captures list, for process output
- Add a "Process Output" label and a toggle button to show/hide it
- The text edit uses different colors for stdout (default) vs stderr (red/orange)
- Auto-scroll to bottom as new output arrives
- Add a "Clear" button

**Constructor changes:**
- Accept `ProcessIOHandles*` (for local) or connect to remote I/O stream
- Create and start a `ProcessIOReader` (local) or poll the remote server (remote)
- Connect signals to append text to the QTextEdit

**Connection thread changes (for remote):**
- In the existing `connectionThreadEntry()` loop, also check for process I/O data from the remote server and forward it to the UI

### Step 7: Wire up MainWindow launch flow

**Files:** `qrenderdoc/Windows/MainWindow.cpp`

- In `OnCaptureTrigger()`, pass `ret.ioHandles` to the `LiveCapture` constructor for local launches
- For remote launches, pass a reference to the remote server so LiveCapture can poll for I/O data

### Step 8: Update build files

**Files:** `qrenderdoc/qrenderdoc.pro`, `qrenderdoc/qrenderdoc_local.vcxproj`, CMakeLists where applicable

- Add `ProcessIOReader.h` and `ProcessIOReader.cpp` to the build

## Detailed File Changes

### `renderdoc/os/os_specific.h`
- Add `ProcessIOHandles` struct in `Process` namespace
- Add `ioHandles` parameter to `LaunchAndInjectIntoProcess`

### `renderdoc/os/posix/posix_process.cpp`
- Modify `LaunchAndInjectIntoProcess` to create pipes when `ioHandles != NULL`
- Pass pipe arrays to `RunProcess()`
- Store read-end FDs in `ioHandles`, close write ends in parent

### `renderdoc/os/win32/win32_process.cpp`
- Modify `LaunchAndInjectIntoProcess` to create pipes when `ioHandles != NULL`
- Pass pipe read handles to `RunProcess()`
- Store read HANDLEs in `ioHandles`

### `renderdoc/api/replay/control_types.h`
- Add `ProcessIOHandles *ioHandles` to `ExecuteResult` (internal only, guarded by `#ifndef SWIG`)

### `renderdoc/replay/entry_points.cpp`
- Modify `RENDERDOC_ExecuteAndInject` to allocate and pass `ProcessIOHandles`

### `renderdoc/core/remote_server.cpp`
- Add `ProcessOutput` packet type
- Server side: start I/O reader thread after launching process, send output packets
- Client side: handle `ProcessOutput` packets, buffer data

### `qrenderdoc/Code/ProcessIOReader.h` (NEW)
### `qrenderdoc/Code/ProcessIOReader.cpp` (NEW)
- Platform-agnostic I/O reader for local process pipes

### `qrenderdoc/Windows/Dialogs/LiveCapture.ui`
- Add QTextEdit for process output below the captures area

### `qrenderdoc/Windows/Dialogs/LiveCapture.h`
- Add member variables for ProcessIOReader, QTextEdit
- Add slots for receiving output data

### `qrenderdoc/Windows/Dialogs/LiveCapture.cpp`
- Wire up ProcessIOReader or remote I/O polling
- Append output to QTextEdit with color coding

### `qrenderdoc/Windows/MainWindow.cpp`
- Pass ioHandles through to LiveCapture constructor

## Testing Approach
- Launch a process that writes to stdout/stderr and verify output appears in the LiveCapture panel
- Test with a process that produces large amounts of output (buffer handling)
- Test process exit (pipe closure handling)
- Test remote launch if possible
- Verify no resource leaks (pipe handles properly closed)
