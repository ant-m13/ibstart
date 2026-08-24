#include "ui/folder_picker.hpp"

#include "ui/dialog_support.hpp"

#include <CommCtrl.h>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"IBStart.FolderPicker";
constexpr int kTreeControl = 1800;

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

struct State {
  HWND tree{};
  HFONT font{};
  HFONT button_font{};
  std::vector<std::wstring> folders;
  std::optional<std::wstring> result;
  bool done{false};
};

HTREEITEM AddItem(State& state, const catalog::TreeItem& source, HTREEITEM parent, std::wstring_view selected) {
  if (source.database) return nullptr;
  const size_t index = state.folders.size();
  state.folders.push_back(source.name);
  TVINSERTSTRUCTW insert{};
  insert.hParent = parent;
  insert.hInsertAfter = TVI_LAST;
  insert.item.mask = TVIF_TEXT | TVIF_PARAM;
  insert.item.pszText = const_cast<wchar_t*>(source.name.c_str());
  insert.item.lParam = static_cast<LPARAM>(index);
  const HTREEITEM node = TreeView_InsertItem(state.tree, &insert);
  for (const auto& child : source.children) AddItem(state, child, node, selected);
  TreeView_Expand(state.tree, node, TVE_EXPAND);
  if (EqualNoCase(source.name, selected)) TreeView_SelectItem(state.tree, node);
  return node;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_COMMAND && state) {
    if (LOWORD(wparam) == IDOK) {
      const HTREEITEM selected = TreeView_GetSelection(state->tree);
      TVITEMW item{};
      item.mask = TVIF_PARAM;
      item.hItem = selected;
      if (selected && TreeView_GetItem(state->tree, &item) && item.lParam >= 0 && static_cast<size_t>(item.lParam) < state->folders.size()) {
        state->result = state->folders[static_cast<size_t>(item.lParam)];
      }
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

std::optional<std::wstring> SelectCatalogFolder(HWND owner, const std::vector<catalog::TreeItem>& items,
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
  const SIZE outer_size = DialogOuterSize(owner, 460, 400, style, extended_style);
  HWND dialog = CreateWindowExW(extended_style, kClassName, L"Переместить в папку", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND caption = CreateWindowW(L"STATIC", L"Выберите папку, в которую нужно переместить базу:", WS_CHILD | WS_VISIBLE,
      px(10), px(10), px(440), px(18), dialog, nullptr, nullptr, nullptr);
  state.tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
      px(10), px(32), px(440), px(315), dialog, reinterpret_cast<HMENU>(kTreeControl), nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"Переместить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      px(224), px(360), px(120), px(28), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(354), px(360), px(96), px(28), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(caption, state.font);
  SetControlFont(state.tree, state.font);
  SetControlFont(accept, state.button_font ? state.button_font : state.font);
  SetControlFont(cancel, state.button_font ? state.button_font : state.font);
  state.folders.push_back(L"");
  std::wstring root_label = L"Корневой уровень";
  TVINSERTSTRUCTW root{};
  root.hParent = TVI_ROOT;
  root.hInsertAfter = TVI_LAST;
  root.item.mask = TVIF_TEXT | TVIF_PARAM;
  root.item.pszText = root_label.data();
  root.item.lParam = 0;
  const HTREEITEM root_item = TreeView_InsertItem(state.tree, &root);
  if (initial.empty()) TreeView_SelectItem(state.tree, root_item);
  for (const auto& item : items) AddItem(state, item, root_item, initial);
  TreeView_Expand(state.tree, root_item, TVE_EXPAND);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.tree);
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
