#pragma once

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string_view>

namespace ibstart::ui::dialog {

[[nodiscard]] HFONT CreateUiFont(HWND window, int points, LONG weight);
[[nodiscard]] int ScaleForDpi(int logical_pixels, UINT dpi);
[[nodiscard]] SIZE DialogOuterSize(HWND owner, int client_width, int client_height, DWORD style, DWORD extended_style);
void SetControlFont(HWND control, HFONT font);
void PositionDialogNearOwner(HWND dialog, HWND owner);
[[nodiscard]] LRESULT DialogControlColor(UINT message, WPARAM wparam, LPARAM lparam);
void DisableModalOwner(HWND owner);
void CloseModalDialog(HWND dialog, HWND owner);
void RestoreModalOwner(HWND owner);

// The legacy common-dialog API stores the selected name in a MAX_PATH-sized
// caller buffer. IFileDialog returns the complete filesystem path instead.
[[nodiscard]] std::optional<std::filesystem::path> OpenCatalogFile(HWND owner);
[[nodiscard]] std::optional<std::filesystem::path> SaveCatalogFile(HWND owner);
[[nodiscard]] std::optional<std::filesystem::path> PickFolder(HWND owner, std::wstring_view title);

}  // namespace ibstart::ui::dialog
