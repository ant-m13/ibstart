#include "core/launcher/command_builder.hpp"

#include "core/connection/connection_string.hpp"
#include "core/domain/utf.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/platform/platform_version.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
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

struct CaseInsensitiveLess {
  using is_transparent = void;
  bool operator()(std::wstring_view left, std::wstring_view right) const noexcept {
    if (left.empty() || right.empty()) return left.size() < right.size();
    const size_t common = std::min(left.size(), right.size());
    const int comparison = _wcsnicmp(left.data(), right.data(), common);
    return comparison == 0 ? left.size() < right.size() : comparison < 0;
  }
};

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

std::wstring SwitchName(std::wstring_view argument) {
  if (argument.empty() || (argument.front() != L'/' && argument.front() != L'-')) return {};
  size_t start = 1;
  if (start < argument.size() && argument[start] == L'-') ++start;
  const size_t end = argument.find(L'=', start);
  return std::wstring(argument.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
}

[[noreturn]] void ThrowValidation(std::wstring_view message) {
  throw std::invalid_argument(utf::ToUtf8(message));
}

void AddParameterConflict(std::vector<std::wstring>& errors, std::wstring message) {
  if (std::find(errors.begin(), errors.end(), message) == errors.end()) errors.push_back(std::move(message));
}

void ValidateParameterText(std::wstring_view text, std::map<std::wstring, size_t, CaseInsensitiveLess>& occurrences,
    std::vector<std::wstring>& errors) {
  const auto arguments = SplitCommandArguments(text);
  for (size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (EqualNoCase(argument, L"ENTERPRISE") || EqualNoCase(argument, L"DESIGNER") ||
        EqualNoCase(argument, L"CREATEINFOBASE")) {
      AddParameterConflict(errors, L"Дополнительные параметры не должны задавать режим ENTERPRISE, DESIGNER или CREATEINFOBASE.");
      continue;
    }

    const auto name = SwitchName(argument);
    if (name.empty()) continue;
    constexpr std::wstring_view connection_names[] = {
        L"F", L"S", L"WS", L"IBConnection", L"IBConnectionString", L"URL"};
    const bool connection_parameter = std::any_of(std::begin(connection_names), std::end(connection_names),
        [&](const auto value) { return EqualNoCase(name, value); });
    const bool tracked_parameter = connection_parameter ||
        EqualNoCase(name, L"AppArch") || EqualNoCase(name, L"Proxy") ||
        EqualNoCase(name, L"NoProxy") || EqualNoCase(name, L"Execute") ||
        EqualNoCase(name, L"ExecuteAfter");
    if (!tracked_parameter) continue;

    const bool inserted = occurrences.emplace(name, index).second;
    if (!inserted) {
      AddParameterConflict(errors, L"Параметр /" + name + L" указан более одного раза в параметрах запуска.");
    }
    if (connection_parameter) {
      AddParameterConflict(errors, L"Дополнительные параметры не должны переопределять подключение (/" + name + L").");
    } else if (EqualNoCase(name, L"Proxy") && occurrences.contains(std::wstring_view(L"NoProxy"))) {
      AddParameterConflict(errors, L"Параметры /Proxy и /NoProxy взаимоисключающие.");
    } else if (EqualNoCase(name, L"NoProxy") && occurrences.contains(std::wstring_view(L"Proxy"))) {
      AddParameterConflict(errors, L"Параметры /Proxy и /NoProxy взаимоисключающие.");
    }

    if (EqualNoCase(name, L"AppArch")) {
      std::wstring value;
      constexpr std::wstring_view prefix = L"/AppArch=";
      if (argument.size() >= prefix.size() && _wcsnicmp(argument.c_str(), prefix.data(), prefix.size()) == 0) {
        value = argument.substr(prefix.size());
      } else if (index + 1 < arguments.size() && !SwitchName(arguments[index + 1]).empty()) {
        AddParameterConflict(errors, L"У параметра /AppArch отсутствует значение.");
      } else if (index + 1 < arguments.size()) {
        value = arguments[++index];
      } else {
        AddParameterConflict(errors, L"У параметра /AppArch отсутствует значение.");
      }
      if (value.empty()) {
        AddParameterConflict(errors, L"У параметра /AppArch отсутствует допустимое значение.");
      } else if (!ParseAppArchitecture(value)) {
        AddParameterConflict(errors, L"Недопустимое значение /AppArch: " + value + L".");
      }
    } else if (EqualNoCase(name, L"Execute") &&
        (index + 1 >= arguments.size() || !SwitchName(arguments[index + 1]).empty())) {
      AddParameterConflict(errors, L"У параметра /Execute отсутствует команда.");
    }
  }
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
  std::optional<domain::ClientArchitecture> result;
  for (size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (EqualNoCase(argument, L"/AppArch")) {
      if (index + 1 >= arguments.size() || !SwitchName(arguments[index + 1]).empty()) {
        throw std::invalid_argument("AppArch parameter has no value.");
      }
      if (result) throw std::invalid_argument("AppArch parameter is specified more than once.");
      result = ParseAppArchitecture(arguments[++index]);
      if (!result) throw std::invalid_argument("AppArch parameter has an invalid value.");
      continue;
    }
    constexpr std::wstring_view prefix = L"/AppArch=";
    if (argument.size() >= prefix.size() && _wcsnicmp(argument.c_str(), prefix.data(), prefix.size()) == 0) {
      if (result) throw std::invalid_argument("AppArch parameter is specified more than once.");
      result = ParseAppArchitecture(std::wstring_view(argument).substr(prefix.size()));
      if (!result) throw std::invalid_argument("AppArch parameter has an invalid value.");
    }
  }
  return result;
}

ConnectionSpec ParseConnectionSpec(std::wstring_view connect) {
  const auto parsed = connection::Parse(connect);
  if (!parsed.diagnostics.empty()) ThrowValidation(parsed.diagnostics.front());

  std::optional<std::wstring> file;
  std::optional<std::wstring> server;
  std::optional<std::wstring> reference;
  std::optional<std::wstring> web;
  bool direct_web = false;
  if (!parsed.fragments.empty() && !parsed.fragments.front().has_equals) {
    const auto& first = parsed.fragments.front().value;
    const bool has_http_scheme = (first.size() >= 7 && EqualNoCase(std::wstring_view(first).substr(0, 7), L"http://")) ||
        (first.size() >= 8 && EqualNoCase(std::wstring_view(first).substr(0, 8), L"https://"));
    if (has_http_scheme && !connection::IsValidHttpUrl(first)) {
      ThrowValidation(L"Legacy URL должен содержать корректную схему, authority, хост и порт.");
    }
    if (has_http_scheme) {
      if (parsed.fragments.size() > 1 && !connection::WebUrl(connect)) {
        ThrowValidation(L"Legacy URL с неоднозначным символом ';'; заключите URL в кавычки.");
      }
      direct_web = true;
      web = first;
    }
  }
  for (const auto& fragment : parsed.fragments) {
    if (!fragment.has_equals) continue;
    std::optional<std::wstring>* destination = nullptr;
    if (EqualNoCase(fragment.key, L"File")) destination = &file;
    else if (EqualNoCase(fragment.key, L"Srvr")) destination = &server;
    else if (EqualNoCase(fragment.key, L"Ref")) destination = &reference;
    else if (EqualNoCase(fragment.key, L"WS")) destination = &web;
    if (!destination) continue;
    if (*destination) ThrowValidation(L"Строка подключения содержит повторяющийся ключ: " + fragment.key + L".");
    *destination = fragment.value;
  }

  const bool has_file = file.has_value();
  const bool has_server = server.has_value() || reference.has_value();
  const bool has_web = web.has_value() || direct_web;
  const int variants = static_cast<int>(has_file) + static_cast<int>(has_server) + static_cast<int>(has_web);
  if (variants > 1) ThrowValidation(L"Строка подключения одновременно задаёт File, Srvr/Ref и веб-подключение.");
  if (has_file) {
    if (file->empty()) ThrowValidation(L"Ключ File в строке подключения не может быть пустым.");
    return {ConnectionSpec::Kind::file, *file, {}, {}};
  }
  if (has_server) {
    if (!server || server->empty() || !reference || reference->empty()) {
      ThrowValidation(L"Для серверного подключения нужны непустые ключи Srvr и Ref.");
    }
    return {ConnectionSpec::Kind::server, {}, *server, *reference};
  }
  if (has_web) {
    if (!web || web->empty() || !connection::IsValidHttpUrl(*web)) ThrowValidation(L"Ключ WS должен содержать URL http:// или https://.");
    return {ConnectionSpec::Kind::web, *web, {}, {}};
  }
  return {ConnectionSpec::Kind::fallback, {}, {}, {}};
}

std::vector<std::wstring> ValidateLaunchParameters(const domain::Database& database,
    const domain::LaunchOptions& options) {
  std::vector<std::wstring> errors;
  try {
    if (database.connect.empty()) errors.push_back(L"У базы отсутствует строка подключения Connect.");
    else static_cast<void>(ParseConnectionSpec(database.connect));
  } catch (const std::exception& error) {
    errors.push_back(utf::FromUtf8(error.what()));
  }

  std::map<std::wstring, size_t, CaseInsensitiveLess> occurrences;
  const auto individual_parameters = options.individual_parameters.empty() ? database.additional_parameters :
      options.individual_parameters;
  try {
    ValidateParameterText(options.common_parameters, occurrences, errors);
    ValidateParameterText(individual_parameters, occurrences, errors);
  } catch (const std::exception& error) {
    AddParameterConflict(errors, utf::FromUtf8(error.what()));
  }
  const auto app_architecture = Trim(database.app_arch);
  if (!app_architecture.empty() && !ParseAppArchitecture(app_architecture)) {
    AddParameterConflict(errors, L"Недопустимое значение AppArch: " + app_architecture + L".");
  }
  return errors;
}

std::optional<std::wstring> BrowserFallbackUrl(const domain::Database& database,
    const domain::LaunchOptions& options) {
  const auto connection_spec = ParseConnectionSpec(database.connect);
  if (connection_spec.kind != ConnectionSpec::Kind::web) return std::nullopt;
  const auto validation = ValidateLaunchParameters(database, options);
  if (!validation.empty()) ThrowValidation(validation.front());
  return connection_spec.value;
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
    if (options.client_type == domain::ClientType::thin && !platform::FindThinClient(candidate.executable)) continue;
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
  const auto validation = ValidateLaunchParameters(database, options);
  if (!validation.empty()) ThrowValidation(validation.front());
  const auto connection_spec = ParseConnectionSpec(database.connect);
  const bool thinOnlyPlatform = IsThinOnlyPlatform(platform);
  if (thinOnlyPlatform && (options.mode == domain::LaunchMode::designer ||
      options.client_type == domain::ClientType::thick)) {
    throw std::invalid_argument("The standalone thin client cannot run the requested mode.");
  }
  domain::LaunchCommand command;
  command.executable = platform.executable;
  if (options.client_type == domain::ClientType::thin) {
    if (options.mode == domain::LaunchMode::designer) throw std::invalid_argument("Designer cannot be launched with the thin client.");
    const auto thin = platform::FindThinClient(platform.executable);
    if (!thin) throw std::runtime_error("The selected platform has no thin client (1cv8c.exe).");
    command.executable = *thin;
  }
  if (options.mode == domain::LaunchMode::designer) command.arguments.push_back(L"DESIGNER");
  else if (options.mode == domain::LaunchMode::enterprise) command.arguments.push_back(L"ENTERPRISE");

  if (connection_spec.kind == ConnectionSpec::Kind::web) {
    if (options.mode != domain::LaunchMode::enterprise) {
      throw std::invalid_argument("A web database can only be launched in enterprise mode.");
    }
    if (options.client_type != domain::ClientType::thin) {
      throw std::invalid_argument("A web database requires the thin client.");
    }
    command.arguments.insert(command.arguments.end(), {L"/WS", connection_spec.value});
  } else if (connection_spec.kind == ConnectionSpec::Kind::file) {
    command.arguments.insert(command.arguments.end(), {L"/F", connection_spec.value});
  } else if (connection_spec.kind == ConnectionSpec::Kind::server) {
    command.arguments.insert(command.arguments.end(), {L"/S", connection_spec.server + L"\\" + connection_spec.reference});
  } else {
    command.arguments.insert(command.arguments.end(), {L"/IBConnection", database.connect});
  }
  const auto common = SplitCommandArguments(options.common_parameters);
  const auto individual = SplitCommandArguments(options.individual_parameters.empty() ? database.additional_parameters : options.individual_parameters);
  command.arguments.insert(command.arguments.end(), common.begin(), common.end());
  command.arguments.insert(command.arguments.end(), individual.begin(), individual.end());
  return command;
}

}  // namespace ibstart::launcher
