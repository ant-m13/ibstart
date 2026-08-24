#pragma once

#include "core/catalog/catalog.hpp"

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<std::wstring> SelectCatalogFolder(HWND owner, const std::vector<catalog::TreeItem>& items,
    std::wstring_view initial);

}  // namespace ibstart::ui::dialog
