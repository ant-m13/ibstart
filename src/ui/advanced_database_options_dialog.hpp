#pragma once

#include "ui/database_editor_dialog.hpp"

#include <Windows.h>

#include <optional>
#include <span>

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<DatabaseEditorData> EditAdvancedDatabaseOptions(
    HWND owner, DatabaseEditorData initial, std::span<const domain::PlatformInstallation> installations = {});

}  // namespace ibstart::ui::dialog
