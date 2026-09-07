#pragma once

#include "core/domain/model.hpp"
#include "core/credentials/credentials.hpp"

#include <Windows.h>

#include <optional>
#include <vector>
#include <functional>

namespace ibstart::ui::dialog {

// Opens a one-off launch editor. The returned credentials and parameters are
// intentionally kept outside the database model and are valid only for the
// current launch operation.
[[nodiscard]] std::optional<domain::LaunchOptions> EditLaunchOptions(
    HWND owner, const domain::Database& database,
    const std::vector<domain::PlatformInstallation>& platforms,
    domain::LaunchOptions initial, std::vector<credentials::Credential> credentials = {},
    std::function<std::vector<credentials::Credential>(HWND)> manage_credentials = {});

}  // namespace ibstart::ui::dialog
