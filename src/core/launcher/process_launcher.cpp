#include "core/launcher/process_launcher.hpp"

#include "core/domain/utf.hpp"
#include "core/windows_path.hpp"

#include <Windows.h>

#include <stdexcept>

namespace ibstart::launcher {

void Launch(const domain::LaunchCommand& command) {
  if (!windows_path::IsWithinLimit(command.executable)) {
    throw std::runtime_error("1C executable path is too long: " +
        utf::ToUtf8(windows_path::LengthError(command.executable)));
  }
  if (!command.executable.parent_path().empty() &&
      !windows_path::IsWithinLimit(command.executable.parent_path())) {
    throw std::runtime_error("1C working directory path is too long: " +
        utf::ToUtf8(windows_path::LengthError(command.executable.parent_path())));
  }
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
