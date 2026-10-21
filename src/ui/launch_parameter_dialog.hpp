#pragma once

#include "core/launcher/parameter_catalog.hpp"

#include <Windows.h>

#include <optional>
#include <string>

namespace ibstart::ui::dialog {

enum class ParameterDialogPurpose { return_to_field, standalone };

// Edits only a local copy. nullopt = cancel; an empty string = explicit clear.
[[nodiscard]] std::optional<std::wstring> EditLaunchParameters(HWND owner,
    std::wstring initial, launcher::ParameterEditorContext context = {},
    ParameterDialogPurpose purpose = ParameterDialogPurpose::return_to_field,
    std::span<const domain::PlatformInstallation> installations = {});

}  // namespace ibstart::ui::dialog
