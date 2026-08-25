#include "core/launcher/command_builder.hpp"

#include "core/connection/connection_string.hpp"
#include "core/domain/utf.hpp"
#include "core/platform/platform_version.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
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

std::wstring Trim(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

bool VersionMatches(std::wstring_view installed, std::wstring_view requested) {
  if (EqualNoCase(installed, requested)) return true;
  // In ibases.v8i a platform can be specified without its build number, for
  // example 8.3.27.  An installed 8.3.27.1688 is then a valid match, whereas
  // 8.3.270 must not accidentally match 8.3.27.
  return installed.size() > requested.size() &&
      _wcsnicmp(installed.data(), requested.data(), requested.size()) == 0 &&
      installed[requested.size()] == L'.';
}

bool IsThinOnlyPlatform(const domain::PlatformInstallation& platform) {
  return EqualNoCase(platform.executable.filename().wstring(), L"1cv8c.exe");
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
  bool tokenStarted = false;
  size_t slashes = 0;
  const auto flush = [&] { if (tokenStarted) { result.push_back(std::move(current)); current.clear(); tokenStarted = false; } };
  for (const wchar_t character : text) {
    if (character == L'\\') { ++slashes; tokenStarted = true; continue; }
    if (character == L'"') {
      tokenStarted = true;
      current.append(slashes / 2, L'\\');
      if (slashes % 2 == 0) quoted = !quoted;
      else current.push_back(L'"');
      slashes = 0;
      continue;
    }
    current.append(slashes, L'\\');
    slashes = 0;
    if (std::iswspace(character) && !quoted) flush();
    else { current.push_back(character); tokenStarted = true; }
  }
  current.append(slashes, L'\\');
  if (quoted) throw std::invalid_argument("Unclosed quote in additional launch parameters.");
  flush();
  return result;
}

std::optional<domain::ClientArchitecture> ParseAppArchitecture(std::wstring_view value) {
  const auto normalized = Trim(value);
  if (EqualNoCase(normalized, L"x86")) return domain::ClientArchitecture::x86;
  if (EqualNoCase(normalized, L"x86_64")) return domain::ClientArchitecture::x64;
  if (EqualNoCase(normalized, L"x86_prt")) return domain::ClientArchitecture::x86_priority;
  if (EqualNoCase(normalized, L"x86_64_prt")) return domain::ClientArchitecture::x64_priority;
  return std::nullopt;
}

std::optional<domain::ClientArchitecture> AppArchitectureFromParameters(std::wstring_view text) {
  const auto arguments = SplitCommandArguments(text);
  for (size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (EqualNoCase(argument, L"/AppArch") && index + 1 < arguments.size()) return ParseAppArchitecture(arguments[index + 1]);
    constexpr std::wstring_view prefix = L"/AppArch=";
    if (argument.size() > prefix.size() && _wcsnicmp(argument.c_str(), prefix.data(), prefix.size()) == 0) {
      return ParseAppArchitecture(std::wstring_view(argument).substr(prefix.size()));
    }
  }
  return std::nullopt;
}

std::optional<domain::PlatformInstallation> SelectPlatform(
    std::span<const domain::PlatformInstallation> candidates, const domain::LaunchOptions& options) {
  std::vector<domain::PlatformInstallation> filtered;
  const auto architecture = options.architecture;
  for (const auto& candidate : candidates) {
    if (options.version != L"" && options.version != L"Авто" && !VersionMatches(candidate.version, options.version)) continue;
    if (options.bitness != domain::ClientBitness::automatic && candidate.bitness != options.bitness) continue;
    if ((architecture == domain::ClientArchitecture::x86 && candidate.bitness != domain::ClientBitness::x86) ||
        (architecture == domain::ClientArchitecture::x64 && candidate.bitness != domain::ClientBitness::x64)) continue;
    const bool requiresThickClient = options.mode == domain::LaunchMode::designer ||
        options.client_type == domain::ClientType::thick;
    if (requiresThickClient && IsThinOnlyPlatform(candidate)) continue;
    if (options.client_type == domain::ClientType::thin && !candidate.has_thin_client) continue;
    filtered.push_back(candidate);
  }
  std::sort(filtered.begin(), filtered.end(), [architecture](const auto& left, const auto& right) {
    if (left.version != right.version) {
      if (platform::IsNewerVersion(left.version, right.version)) return true;
      if (platform::IsNewerVersion(right.version, left.version)) return false;
    }
    // Automatic selection follows the product default: prefer a 64-bit client
    // when both bitnesses are available.  Only an explicit x86 priority reverses
    // that order; the strict x86/x64 modes were already filtered above.
    const bool prefer64 = architecture != domain::ClientArchitecture::x86_priority;
    const int leftRank = left.bitness == (prefer64 ? domain::ClientBitness::x64 : domain::ClientBitness::x86) ? 0 : 1;
    const int rightRank = right.bitness == (prefer64 ? domain::ClientBitness::x64 : domain::ClientBitness::x86) ? 0 : 1;
    if (leftRank != rightRank) return leftRank < rightRank;
    return left.executable.wstring() < right.executable.wstring();
  });
  if (filtered.empty()) return std::nullopt;
  return filtered.front();
}

domain::LaunchCommand BuildCommand(const domain::Database& database,
    const domain::PlatformInstallation& platform, const domain::LaunchOptions& options) {
  if (database.connect.empty()) throw std::invalid_argument("Database has no Connect field.");
  const bool thinOnlyPlatform = IsThinOnlyPlatform(platform);
  if (thinOnlyPlatform && (options.mode == domain::LaunchMode::designer ||
      options.client_type == domain::ClientType::thick)) {
    throw std::invalid_argument("The standalone thin client cannot run the requested mode.");
  }
  domain::LaunchCommand command;
  command.executable = platform.executable;
  if (options.client_type == domain::ClientType::thin) {
    if (options.mode == domain::LaunchMode::designer) throw std::invalid_argument("Designer cannot be launched with the thin client.");
    const auto thin = platform.executable.parent_path() / L"1cv8c.exe";
    if (!std::filesystem::exists(thin)) throw std::runtime_error("The selected platform has no thin client (1cv8c.exe).");
    command.executable = thin;
  }
  if (options.mode == domain::LaunchMode::designer) command.arguments.push_back(L"DESIGNER");
  else if (options.mode == domain::LaunchMode::enterprise) command.arguments.push_back(L"ENTERPRISE");

  const auto web = connection::WebUrl(database.connect);
  const auto file = connection::ValueOrEmpty(database.connect, L"File");
  const auto server = connection::ValueOrEmpty(database.connect, L"Srvr");
  const auto reference = connection::ValueOrEmpty(database.connect, L"Ref");
  if (web) {
    if (options.mode != domain::LaunchMode::enterprise) {
      throw std::invalid_argument("A web database can only be launched in enterprise mode.");
    }
    if (options.client_type != domain::ClientType::thin) {
      throw std::invalid_argument("A web database requires the thin client.");
    }
    command.arguments.insert(command.arguments.end(), {L"/WS", *web});
  } else if (!file.empty()) {
    command.arguments.insert(command.arguments.end(), {L"/F", file});
  } else if (!server.empty() && !reference.empty()) {
    command.arguments.insert(command.arguments.end(), {L"/S", server + L"\\" + reference});
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
