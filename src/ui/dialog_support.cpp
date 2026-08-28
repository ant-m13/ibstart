#include "ui/dialog_support.hpp"

#include <ShObjIdl.h>

#include <algorithm>
#include <string>

namespace {

template <typename Interface>
class ComPtr final {
 public:
  explicit ComPtr(Interface* value = nullptr) : value_(value) {}
  ~ComPtr() { if (value_) value_->Release(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface* operator->() const noexcept { return value_; }

 private:
  Interface* value_{};
};

std::optional<std::filesystem::path> DialogResultPath(IFileDialog* dialog) {
  IShellItem* raw_item{};
  if (FAILED(dialog->GetResult(&raw_item))) return std::nullopt;
  ComPtr<IShellItem> item(raw_item);

  PWSTR raw_path{};
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || !raw_path) return std::nullopt;
  std::filesystem::path path(raw_path);
  CoTaskMemFree(raw_path);
  return path;
}

std::optional<std::filesystem::path> ShowCatalogFileDialog(HWND owner, bool save) {
  IFileDialog* raw_dialog{};
  const CLSID class_id = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
  if (FAILED(CoCreateInstance(class_id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_dialog)))) {
    return std::nullopt;
  }
  ComPtr<IFileDialog> dialog(raw_dialog);

  const std::wstring title = save ? L"Сохранить список баз" : L"Открыть список баз";
  const COMDLG_FILTERSPEC filters[] = {
      {L"Списки баз 1С (*.v8i)", L"*.v8i"},
      {L"Все файлы (*.*)", L"*.*"},
  };
  if (FAILED(dialog->SetTitle(title.c_str())) || FAILED(dialog->SetFileTypes(2, filters)) ||
      FAILED(dialog->SetDefaultExtension(L"v8i"))) {
    return std::nullopt;
  }
  if (save && FAILED(dialog->SetFileName(L"ibases.v8i"))) return std::nullopt;

  DWORD options{};
  if (FAILED(dialog->GetOptions(&options))) return std::nullopt;
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
  if (FAILED(dialog->SetOptions(options)) || FAILED(dialog->Show(owner))) return std::nullopt;
  return DialogResultPath(dialog.get());
}

}  // namespace

namespace ibstart::ui::dialog {

HFONT CreateUiFont(HWND window, int points, LONG weight) {
  return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int ScaleForDpi(int logical_pixels, UINT dpi) {
  return MulDiv(logical_pixels, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

SIZE DialogOuterSize(HWND owner, int client_width, int client_height, DWORD style, DWORD extended_style) {
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  RECT bounds{0, 0, ScaleForDpi(client_width, dpi), ScaleForDpi(client_height, dpi)};
  if (!AdjustWindowRectExForDpi(&bounds, style, FALSE, extended_style, dpi)) return {bounds.right, bounds.bottom};
  return {bounds.right - bounds.left, bounds.bottom - bounds.top};
}

void SetControlFont(HWND control, HFONT font) {
  if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void PositionDialogNearOwner(HWND dialog, HWND owner) {
  RECT dialog_rect{};
  if (!GetWindowRect(dialog, &dialog_rect)) return;
  const int width = dialog_rect.right - dialog_rect.left;
  const int height = dialog_rect.bottom - dialog_rect.top;
  RECT owner_rect{};
  if (!owner || !GetWindowRect(owner, &owner_rect)) owner_rect = dialog_rect;
  MONITORINFO monitor{sizeof(monitor)};
  const HMONITOR selected = MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(selected, &monitor)) return;
  const RECT work = monitor.rcWork;
  int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
  int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
  const int minimum_x = static_cast<int>(work.left);
  const int minimum_y = static_cast<int>(work.top);
  const int maximum_x = std::max(minimum_x, static_cast<int>(work.right) - width);
  const int maximum_y = std::max(minimum_y, static_cast<int>(work.bottom) - height);
  x = std::clamp(x, minimum_x, maximum_x);
  y = std::clamp(y, minimum_y, maximum_y);
  SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT DialogControlColor(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message != WM_CTLCOLORSTATIC && message != WM_CTLCOLORBTN) return 0;
  const HDC context = reinterpret_cast<HDC>(wparam);
  const HWND control = reinterpret_cast<HWND>(lparam);
  SetBkColor(context, GetSysColor(COLOR_WINDOW));
  SetTextColor(context, IsWindowEnabled(control) ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT));
  return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}

void DisableModalOwner(HWND owner) {
  if (owner && IsWindow(owner)) EnableWindow(owner, FALSE);
}

void CloseModalDialog(HWND dialog, HWND owner) {
  // Re-enabling the owner before its owned popup is destroyed lets Windows
  // restore activation in one transition, without a visible owner redraw.
  if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
  if (dialog && IsWindow(dialog)) DestroyWindow(dialog);
}

void RestoreModalOwner(HWND owner) {
  if (!owner || !IsWindow(owner)) return;
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
}

std::optional<std::filesystem::path> OpenCatalogFile(HWND owner) {
  return ShowCatalogFileDialog(owner, false);
}

std::optional<std::filesystem::path> SaveCatalogFile(HWND owner) {
  return ShowCatalogFileDialog(owner, true);
}

std::optional<std::filesystem::path> PickFolder(HWND owner, std::wstring_view title) {
  IFileOpenDialog* raw_dialog{};
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&raw_dialog)))) {
    return std::nullopt;
  }
  ComPtr<IFileOpenDialog> dialog(raw_dialog);
  const std::wstring dialog_title(title);
  if (FAILED(dialog->SetTitle(dialog_title.c_str()))) return std::nullopt;

  DWORD options{};
  if (FAILED(dialog->GetOptions(&options))) return std::nullopt;
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS;
  if (FAILED(dialog->SetOptions(options)) || FAILED(dialog->Show(owner))) return std::nullopt;
  return DialogResultPath(dialog.get());
}

}  // namespace ibstart::ui::dialog
