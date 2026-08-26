#include "core/logging/logging.hpp"

#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <vector>

namespace ibstart::logging {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool StartsWithNoCase(std::wstring_view value, std::wstring_view prefix) {
  return value.size() >= prefix.size() && _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

bool IsSeparateSecretSwitch(std::wstring_view argument) {
  constexpr std::wstring_view switches[] = {L"/P", L"/Password", L"--password", L"-password", L"--token", L"-token"};
  return std::any_of(std::begin(switches), std::end(switches), [&](auto value) { return EqualNoCase(argument, value); });
}

std::optional<size_t> ExplicitInlineSecretPrefixLength(std::wstring_view argument) {
  constexpr std::wstring_view prefixes[] = {
      L"/Password=", L"--password=", L"-password=", L"--token=", L"-token=",
      L"password=", L"pwd=", L"token=", L"secret=", L"/P="};
  for (const auto prefix : prefixes) if (StartsWithNoCase(argument, prefix)) return prefix.size();
  return std::nullopt;
}

std::optional<size_t> InlineSecretPrefixLength(std::wstring_view argument) {
  if (const auto explicitPrefix = ExplicitInlineSecretPrefixLength(argument)) return explicitPrefix;
  // 1C also accepts /P immediately followed by a password. Prefer hiding an
  // ambiguous /P... argument in diagnostics over exposing credentials.
  if (argument.size() > 2 && StartsWithNoCase(argument, L"/P")) return 2;
  return std::nullopt;
}

std::wstring Stamp(const wchar_t* format) {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_s(&local, &now);
  std::wostringstream output;
  output << std::put_time(&local, format);
  return output.str();
}
}  // namespace

std::wstring MaskSecrets(std::wstring_view arguments) {
  std::wstring result(arguments);
  // Both forms are accepted by 1C: /P secret and /P"secret". Generic token/password forms are also masked.
  const std::wregex paired(LR"mask(((?:/(?:Password|P)|--(?:password|token)|-(?:password|token))\s*(?:=\s*)?)("(?:[^"]*)"|[^\s]+))mask", std::regex_constants::icase);
  result = std::regex_replace(result, paired, L"$1***");
  // Pwd is the password key used inside 1C connection strings, including the
  // fallback /IBConnection form that is written to the launch log.
  const std::wregex assignment(LR"(((?:password|pwd|token|secret)\s*=\s*)(\"(?:[^\"]*)\"|[^\s]+))", std::regex_constants::icase);
  return std::regex_replace(result, assignment, L"$1***");
}

std::wstring RedactedCommandLine(const domain::LaunchCommand& command) {
  std::vector<std::wstring> redacted;
  redacted.reserve(command.arguments.size());
  bool redactNext = false;
  for (const auto& argument : command.arguments) {
    if (redactNext) {
      redacted.emplace_back(L"***");
      redactNext = false;
      continue;
    }
    if (IsSeparateSecretSwitch(argument)) {
      redacted.push_back(argument);
      redactNext = true;
      continue;
    }
    if (EqualNoCase(argument, L"/IBConnection")) {
      redacted.push_back(argument);
      // A fallback connection string may contain Pwd and other server details.
      // Hiding the complete following argument avoids implementing a second,
      // subtly different connection-string parser in the logging layer.
      redactNext = true;
      continue;
    }
    if (const auto prefix = InlineSecretPrefixLength(argument)) {
      redacted.push_back(argument.substr(0, *prefix) + L"***");
      continue;
    }
    redacted.push_back(argument);
  }

  std::wstring result = launcher::QuoteWindowsArgument(command.executable.wstring());
  for (const auto& argument : redacted) {
    result.push_back(L' ');
    result += launcher::QuoteWindowsArgument(argument);
  }
  return result;
}

bool ContainsSecretArguments(const domain::LaunchCommand& command) {
  bool connectionValue = false;
  for (const auto& argument : command.arguments) {
    if (connectionValue) {
      connectionValue = false;
      continue;
    }
    if (EqualNoCase(argument, L"/F") || EqualNoCase(argument, L"/S") ||
        EqualNoCase(argument, L"/WS") || EqualNoCase(argument, L"/IBConnection")) {
      connectionValue = true;
      continue;
    }
    if (IsSeparateSecretSwitch(argument) || ExplicitInlineSecretPrefixLength(argument)) return true;
  }
  return false;
}

Logger::Logger(std::filesystem::path directory) : directory_(std::move(directory)) {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  path_ = directory_ / (L"ibstart_" + Stamp(L"%Y%m%d_%H%M%S") + L".log");
  Prune();
}

void Logger::Prune() {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const auto& item : std::filesystem::directory_iterator(directory_, error)) {
    if (!error && item.is_regular_file() && item.path().extension() == L".log") files.push_back(item.path());
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.filename() > right.filename(); });
  // path_ is created lazily by the first Write.  Keep room for that current
  // file so the documented ten-log limit is not exceeded after startup.
  for (size_t index = 9; index < files.size(); ++index) std::filesystem::remove(files[index], error);
}

void Logger::Write(std::wstring_view level, std::wstring_view message) {
  std::lock_guard lock(mutex_);
  std::ofstream output(path_, std::ios::binary | std::ios::app);
  if (!output) return;
  const auto line = Stamp(L"%Y-%m-%d %H:%M:%S") + L" [" + std::wstring(level) + L"] " + MaskSecrets(message) + L"\r\n";
  output << utf::ToUtf8(line);
}

void Logger::Info(std::wstring_view message) { Write(L"INFO", message); }
void Logger::Error(std::wstring_view message) { Write(L"ERROR", message); }

}  // namespace ibstart::logging
