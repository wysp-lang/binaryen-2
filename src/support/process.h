/*
 * Copyright 2021 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Process helpers.
//

#ifndef wasm_support_process_h
#define wasm_support_process_h

#include <iostream>
#include <string>

#include "support/timing.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Create a string with last error message
std::string GetLastErrorStdStr() {
  DWORD error = GetLastError();
  if (error) {
    LPVOID lpMsgBuf;
    DWORD bufLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                   FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL,
                                 error,
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 (LPTSTR)&lpMsgBuf,
                                 0,
                                 NULL);
    if (bufLen) {
      LPCSTR lpMsgStr = (LPCSTR)lpMsgBuf;
      std::string result(lpMsgStr, lpMsgStr + bufLen);
      LocalFree(lpMsgBuf);
      return result;
    }
  }
  return std::string();
}
#endif

namespace wasm {

struct ProgramResult {
  size_t timeout;

  std::string command;

  int code;
  std::string output;
  double time;

  ProgramResult(size_t timeout = -1) : timeout(timeout) {}
  ProgramResult(std::string command, size_t timeout = -1) : timeout(timeout) {
    getFromExecution(command);
  }

#ifdef _WIN32
  void getFromExecution(std::string command_) {
    command = command_;

    Timer timer;
    timer.start();
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStd_OUT_Rd;
    HANDLE hChildStd_OUT_Wr;

    if (
      // Create a pipe for the child process's STDOUT.
      !CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0) ||
      // Ensure the read handle to the pipe for STDOUT is not inherited.
      !SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
      Fatal() << "CreatePipe \"" << command
              << "\" failed: " << GetLastErrorStdStr() << ".\n";
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hChildStd_OUT_Wr;
    si.hStdOutput = hChildStd_OUT_Wr;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    // Start the child process.
    if (!CreateProcess(NULL, // No module name (use command line)
                       (LPSTR)command.c_str(), // Command line
                       NULL,                   // Process handle not inheritable
                       NULL,                   // Thread handle not inheritable
                       TRUE,                   // Set handle inheritance to TRUE
                       0,                      // No creation flags
                       NULL,                   // Use parent's environment block
                       NULL, // Use parent's starting directory
                       &si,  // Pointer to STARTUPINFO structure
                       &pi)  // Pointer to PROCESS_INFORMATION structure
    ) {
      Fatal() << "CreateProcess \"" << command
              << "\" failed: " << GetLastErrorStdStr() << ".\n";
    }

    // Wait until child process exits.
    DWORD retVal = WaitForSingleObject(pi.hProcess, timeout * 1000);
    if (retVal == WAIT_TIMEOUT) {
      printf("Command timeout: %s", command.c_str());
      TerminateProcess(pi.hProcess, -1);
    }
    DWORD dwordExitCode;
    if (!GetExitCodeProcess(pi.hProcess, &dwordExitCode)) {
      Fatal() << "GetExitCodeProcess failed: " << GetLastErrorStdStr() << ".\n";
    }
    code = (int)dwordExitCode;

    // Close process and thread handles.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Read output from the child process's pipe for STDOUT
    // Stop when there is no more data.
    {
      const int BUFSIZE = 4096;
      DWORD dwRead, dwTotal, dwTotalRead = 0;
      CHAR chBuf[BUFSIZE];
      BOOL bSuccess = FALSE;

      PeekNamedPipe(hChildStd_OUT_Rd, NULL, 0, NULL, &dwTotal, NULL);
      while (dwTotalRead < dwTotal) {
        bSuccess =
          ReadFile(hChildStd_OUT_Rd, chBuf, BUFSIZE - 1, &dwRead, NULL);
        if (!bSuccess || dwRead == 0)
          break;
        chBuf[dwRead] = 0;
        dwTotalRead += dwRead;
        output.append(chBuf);
      }
    }
    timer.stop();
    time = timer.getTotal();
  }
#else  // POSIX
  // runs the command and notes the output
  // TODO: also stderr, not just stdout?
  void getFromExecution(std::string command_) {
    command = command_;

    Timer timer;
    timer.start();
    // do this using just core stdio.h and stdlib.h, for portability
    // sadly this requires two invokes
    code = system(("timeout " + std::to_string(timeout) + "s " + command +
                   " > /dev/null 2> /dev/null")
                    .c_str());
    const int MAX_BUFFER = 1024;
    char buffer[MAX_BUFFER];
    FILE* stream = popen(
      ("timeout " + std::to_string(timeout) + "s " + command + " 2> /dev/null")
        .c_str(),
      "r");
    while (fgets(buffer, MAX_BUFFER, stream) != NULL) {
      output.append(buffer);
    }
    pclose(stream);
    timer.stop();
    time = timer.getTotal() / 2;
  }
#endif // _WIN32

  bool operator==(ProgramResult& other) {
    return code == other.code && output == other.output;
  }
  bool operator!=(ProgramResult& other) { return !(*this == other); }

  bool failed() { return code != 0; }

  void dump(std::ostream& o) {
    o << "[ProgramResult " << command << "]\ncode: " << code << ", stdout: \n"
      << output << "[====]\nin " << time << " seconds\n[/ProgramResult]\n";
  }
};

} // namespace wasm

namespace std {

inline std::ostream& operator<<(std::ostream& o, wasm::ProgramResult& result) {
  result.dump(o);
  return o;
}

} // namespace std

#endif // wasm_support_process_h
