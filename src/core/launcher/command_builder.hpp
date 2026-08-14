#pragma once

#include "core/domain/model.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ibstart::launcher {

[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view argument);
[[nodiscard]] std::vector<std::wstring> SplitCommandArguments(std::wstring_view text);
[[nodiscard]] std::optional<domain::PlatformInstallation> SelectPlatform(
    std::span<const domain::PlatformInstallation> candidates, const domain::LaunchOptions& options);
[[nodiscard]] domain::LaunchCommand BuildCommand(const domain::Database& database,
    const domain::PlatformInstallation& platform, const domain::LaunchOptions& options);
void Launch(const domain::LaunchCommand& command);

}  // namespace ibstart::launcher
