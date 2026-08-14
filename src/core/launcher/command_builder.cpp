#include "core/launcher/command_builder.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace ibstart::domain {
std::wstring LaunchCommand::CommandLine() const {
  std::wstring command = launcher::QuoteWindowsArgument(executable.wstring());
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command += launcher::QuoteWindowsArgument(argument);
  }
  return command;
}
}  // namespace ibstart::domain

namespace ibstart::launcher {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::wstring Unquote(std::wstring value) {
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') return value.substr(1, value.size() - 2);
  return value;
}

std::wstring ConnectionValue(std::wstring_view connect, std::wstring_view key) {
  const std::wstring search = std::wstring(key) + L"=";
  const size_t start = connect.find(search);
  if (start == std::wstring_view::npos) return {};
  size_t valueStart = start + search.size();
  size_t valueEnd = connect.find(L';', valueStart);
  if (valueEnd == std::wstring_view::npos) valueEnd = connect.size();
  return Unquote(std::wstring(connect.substr(valueStart, valueEnd - valueStart)));
}

}  // namespace

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
  if (argument.empty()) return L"\"\"";
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(argument);
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(character);
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::vector<std::wstring> SplitCommandArguments(std::wstring_view text) {
  std::vector<std::wstring> result;
  std::wstring current;
  bool quoted = false;
  size_t slashes = 0;
  const auto flush = [&] { if (!current.empty()) { result.push_back(std::move(current)); current.clear(); } };
  for (const wchar_t character : text) {
    if (character == L'\\') { ++slashes; continue; }
    if (character == L'"') {
      current.append(slashes / 2, L'\\');
      if (slashes % 2 == 0) quoted = !quoted;
      else current.push_back(L'"');
      slashes = 0;
      continue;
    }
    current.append(slashes, L'\\');
    slashes = 0;
    if (std::iswspace(character) && !quoted) flush();
    else current.push_back(character);
  }
  current.append(slashes, L'\\');
  if (quoted) throw std::invalid_argument("Unclosed quote in additional launch parameters.");
  flush();
  return result;
}

std::optional<domain::PlatformInstallation> SelectPlatform(
    std::span<const domain::PlatformInstallation> candidates, const domain::LaunchOptions& options) {
  std::vector<domain::PlatformInstallation> filtered;
  for (const auto& candidate : candidates) {
    if (options.version != L"" && options.version != L"Авто" && !EqualNoCase(candidate.version, options.version)) continue;
    if (options.bitness != domain::ClientBitness::automatic && candidate.bitness != options.bitness) continue;
    if (options.client_type == domain::ClientType::thin && !candidate.has_thin_client) continue;
    filtered.push_back(candidate);
  }
  std::sort(filtered.begin(), filtered.end(), [](const auto& left, const auto& right) {
    const int leftRank = left.bitness == domain::ClientBitness::x64 ? 0 : 1;
    const int rightRank = right.bitness == domain::ClientBitness::x64 ? 0 : 1;
    if (leftRank != rightRank) return leftRank < rightRank;
    return left.version > right.version;
  });
  if (filtered.empty()) return std::nullopt;
  return filtered.front();
}

domain::LaunchCommand BuildCommand(const domain::Database& database,
    const domain::PlatformInstallation& platform, const domain::LaunchOptions& options) {
  if (database.connect.empty()) throw std::invalid_argument("Database has no Connect field.");
  domain::LaunchCommand command;
  command.executable = platform.executable;
  if (options.client_type == domain::ClientType::thin) {
    const auto thin = platform.executable.parent_path() / L"1cv8c.exe";
    if (!std::filesystem::exists(thin)) throw std::runtime_error("The selected platform has no thin client (1cv8c.exe).");
    command.executable = thin;
  }
  if (options.mode == domain::LaunchMode::designer) command.arguments.push_back(L"DESIGNER");
  else if (options.mode == domain::LaunchMode::enterprise) command.arguments.push_back(L"ENTERPRISE");

  const auto file = ConnectionValue(database.connect, L"File");
  const auto server = ConnectionValue(database.connect, L"Srvr");
  const auto reference = ConnectionValue(database.connect, L"Ref");
  if (!file.empty()) {
    command.arguments.insert(command.arguments.end(), {L"/F", file});
  } else if (!server.empty() && !reference.empty()) {
    command.arguments.insert(command.arguments.end(), {L"/S", server + L"\\" + reference});
  } else if (database.connect.starts_with(L"http://") || database.connect.starts_with(L"https://")) {
    if (options.mode != domain::LaunchMode::web_client) throw std::invalid_argument("A web database can only be opened with the web-client action.");
    command.arguments.insert(command.arguments.end(), {L"/WS", database.connect});
  } else {
    command.arguments.insert(command.arguments.end(), {L"/IBConnection", database.connect});
  }
  const auto common = SplitCommandArguments(options.common_parameters);
  const auto individual = SplitCommandArguments(options.individual_parameters.empty() ? database.additional_parameters : options.individual_parameters);
  command.arguments.insert(command.arguments.end(), common.begin(), common.end());
  command.arguments.insert(command.arguments.end(), individual.begin(), individual.end());
  return command;
}

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
