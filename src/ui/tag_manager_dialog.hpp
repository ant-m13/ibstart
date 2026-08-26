#pragma once

#include "core/storage/storage.hpp"

#include <Windows.h>

#include <optional>

namespace ibstart::ui::dialog {

struct TagManagerResult {
  storage::DatabaseTags tags;
  storage::TagStyles styles;
};

[[nodiscard]] std::optional<TagManagerResult> EditTagManager(
    HWND owner, const storage::DatabaseTags& tags, const storage::TagStyles& styles);

}  // namespace ibstart::ui::dialog
