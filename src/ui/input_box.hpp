#pragma once

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<std::wstring> InputBox(HWND owner, std::wstring_view title, std::wstring_view caption,
    std::wstring_view initial = {});

}  // namespace ibstart::ui::dialog
