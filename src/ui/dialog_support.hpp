#pragma once

#include <Windows.h>

namespace ibstart::ui::dialog {

[[nodiscard]] HFONT CreateUiFont(HWND window, int points, LONG weight);
[[nodiscard]] int ScaleForDpi(int logical_pixels, UINT dpi);
[[nodiscard]] SIZE DialogOuterSize(HWND owner, int client_width, int client_height, DWORD style, DWORD extended_style);
void SetControlFont(HWND control, HFONT font);
void PositionDialogNearOwner(HWND dialog, HWND owner);
[[nodiscard]] LRESULT DialogControlColor(UINT message, WPARAM wparam, LPARAM lparam);
void RestoreModalOwner(HWND owner);

}  // namespace ibstart::ui::dialog
