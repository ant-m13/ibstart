#include "ui/input_box.hpp"

#include "ui/dialog_support.hpp"

#include <cstddef>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"IBStart.InputBox";

struct State {
  HWND edit{};
  HFONT font{};
  HFONT button_font{};
  std::optional<std::wstring> result;
  bool done{false};
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_COMMAND && state) {
    if (LOWORD(wparam) == IDOK) {
      const int length = GetWindowTextLengthW(state->edit);
      std::wstring text(static_cast<size_t>(length) + 1, L'\0');
      GetWindowTextW(state->edit, text.data(), length + 1);
      text.resize(static_cast<size_t>(length));
      state->result = std::move(text);
      state->done = true;
      DestroyWindow(window);
      return 0;
    }
    if (LOWORD(wparam) == IDCANCEL) {
      state->done = true;
      DestroyWindow(window);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

std::optional<std::wstring> InputBox(HWND owner, std::wstring_view title, std::wstring_view caption,
    std::wstring_view initial) {
  State state;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kClassName;
    klass.lpfnWndProc = WindowProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 470, 125, style, extended_style);
  HWND dialog = CreateWindowExW(extended_style, kClassName, std::wstring(title).c_str(), style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND caption_control = CreateWindowW(L"STATIC", std::wstring(caption).c_str(), WS_CHILD | WS_VISIBLE,
      px(14), px(14), px(430), px(20), dialog, nullptr, nullptr, nullptr);
  state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::wstring(initial).c_str(),
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, px(14), px(38), px(430), px(24), dialog, nullptr, nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"ОК", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      px(258), px(78), px(96), px(25), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(364), px(78), px(96), px(25), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(caption_control, state.font);
  SetControlFont(state.edit, state.font);
  SetControlFont(accept, state.button_font ? state.button_font : state.font);
  SetControlFont(cancel, state.button_font ? state.button_font : state.font);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.edit);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace ibstart::ui::dialog
