#pragma once

#include "core/domain/model.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ibstart::launcher {

struct ConnectionSpec {
  enum class Kind { file, server, web, fallback };

  Kind kind{Kind::fallback};
  std::wstring value;
  std::wstring server;
  std::wstring reference;
};

[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view argument);
[[nodiscard]] std::vector<std::wstring> SplitCommandArguments(std::wstring_view text);
[[nodiscard]] std::optional<domain::ClientArchitecture> ParseAppArchitecture(std::wstring_view value);
[[nodiscard]] std::optional<domain::ClientArchitecture> AppArchitectureFromParameters(std::wstring_view text);
[[nodiscard]] ConnectionSpec ParseConnectionSpec(std::wstring_view connect);
[[nodiscard]] std::vector<std::wstring> ValidateLaunchParameters(const domain::Database& database,
    const domain::LaunchOptions& options);
// Validates the connection and launch parameters before allowing browser
// fallback. Returns a URL only for a valid web connection; non-web databases
// return std::nullopt and invalid data throws std::invalid_argument.
[[nodiscard]] std::optional<std::wstring> BrowserFallbackUrl(const domain::Database& database,
    const domain::LaunchOptions& options);
[[nodiscard]] std::optional<domain::PlatformInstallation> SelectPlatform(
    std::span<const domain::PlatformInstallation> candidates, const domain::LaunchOptions& options);
[[nodiscard]] domain::LaunchCommand BuildCommand(const domain::Database& database,
    const domain::PlatformInstallation& platform, const domain::LaunchOptions& options);
}  // namespace ibstart::launcher
