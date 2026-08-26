#pragma once

#include "core/storage/storage.hpp"

#include <Windows.h>

#include <optional>
#include <string>
#include <vector>

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<std::vector<std::wstring>> EditTagAssignment(HWND owner,
    const std::vector<std::wstring>& assigned, const storage::DatabaseTags& tags,
    const storage::TagStyles& styles);

}  // namespace ibstart::ui::dialog
