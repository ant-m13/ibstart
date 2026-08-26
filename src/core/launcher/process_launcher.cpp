#include "core/launcher/process_launcher.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <stdexcept>

namespace ibstart::launcher {

void Launch(const domain::LaunchCommand& command) {
  std::wstring mutableCommandLine = command.CommandLine();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(command.executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
          0, nullptr, command.executable.parent_path().c_str(), &startup, &process)) {
    throw std::runtime_error("Unable to start 1C client: " + utf::ToUtf8(utf::LastErrorMessage()));
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

}  // namespace ibstart::launcher
