#pragma once

#include "ui/database_editor_dialog.hpp"

#include <Windows.h>

#include <optional>

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<DatabaseEditorData> EditAdvancedDatabaseOptions(
    HWND owner, DatabaseEditorData initial);

}  // namespace ibstart::ui::dialog
