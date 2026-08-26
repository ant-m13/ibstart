#include "ui/tag_assignment_dialog.hpp"

#include "ui/dialog_support.hpp"
#include "ui/tree_presentation.hpp"

#include <CommCtrl.h>

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"IBStart.TagAssignment";

enum Control : int {
  kList = 1750,
  kName,
  kAdd
};

struct State {
  HWND list{};
  HWND name{};
  HFONT font{};
  HFONT button_font{};
  const storage::TagStyles* styles{};
  std::optional<std::vector<std::wstring>> result;
  bool done{false};
};

void Message(HWND owner, std::wstring_view text, std::wstring_view title, UINT type) {
  MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type);
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
          static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TrimText(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

std::wstring ReadControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(length));
  return result;
}

std::wstring ListViewText(HWND list, int row, int column) {
  std::wstring text(256, L'\0');
  for (;;) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = text.data();
    item.cchTextMax = static_cast<int>(text.size());
    const int copied = static_cast<int>(SendMessageW(
        list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item)));
    if (copied < static_cast<int>(text.size()) - 1) {
      text.resize(std::max(0, copied));
      return text;
    }
    text.resize(text.size() * 2);
  }
}

int AddItem(State& state, std::wstring_view tag, bool checked) {
  std::wstring value(tag);
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = ListView_GetItemCount(state.list);
  item.pszText = value.data();
  const int row = ListView_InsertItem(state.list, &item);
  if (row >= 0) ListView_SetCheckState(state.list, row, checked);
  return row;
}

bool AddEntry(HWND window, State& state) {
  const std::wstring name = TrimText(ReadControlText(state.name));
  if (name.empty()) {
    Message(window, L"Укажите название нового тега.", L"Теги базы", MB_OK | MB_ICONWARNING);
    return false;
  }
  const int count = ListView_GetItemCount(state.list);
  for (int row = 0; row < count; ++row) {
    if (!EqualNoCase(ListViewText(state.list, row, 0), name)) continue;
    ListView_SetCheckState(state.list, row, TRUE);
    ListView_SetItemState(state.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state.list, row, FALSE);
    SetWindowTextW(state.name, L"");
    return true;
  }
  const int row = AddItem(state, name, true);
  if (row < 0) {
    Message(window, L"Не удалось добавить тег в список.", L"Теги базы", MB_OK | MB_ICONERROR);
    return false;
  }
  ListView_SetItemState(state.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(state.list, row, FALSE);
  SetWindowTextW(state.name, L"");
  return true;
}

std::vector<std::wstring> SelectedTags(const State& state) {
  std::vector<std::wstring> result;
  const int count = ListView_GetItemCount(state.list);
  for (int row = 0; row < count; ++row) {
    if (ListView_GetCheckState(state.list, row)) result.push_back(ListViewText(state.list, row, 0));
  }
  return result;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
    return DialogControlColor(message, wparam, lparam);
  }
  if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header && header->idFrom == kList && header->code == NM_CUSTOMDRAW) {
      auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
      if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const std::wstring tag = ListViewText(state->list, static_cast<int>(draw->nmcd.dwItemSpec), 0);
        const auto* configured = state->styles ? presentation::TagStyleFor(*state->styles, tag) : nullptr;
        const storage::TagStyle style = configured ? *configured : storage::TagStyle{};
        draw->clrTextBk = style.background;
        draw->clrText = style.text;
        return CDRF_DODEFAULT;
      }
    }
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command == kAdd) {
      AddEntry(window, *state);
      return 0;
    }
    if (command == IDOK) {
      state->result = SelectedTags(*state);
      state->done = true;
      DestroyWindow(window);
      return 0;
    }
    if (command == IDCANCEL) {
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

std::optional<std::vector<std::wstring>> EditTagAssignment(HWND owner,
    const std::vector<std::wstring>& assigned, const storage::DatabaseTags& tags,
    const storage::TagStyles& styles) {
  State state;
  state.styles = &styles;
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
  const SIZE outer_size = DialogOuterSize(owner, 570, 358, style, extended_style);
  HWND window = CreateWindowExW(extended_style, kClassName, L"Теги базы", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr,
      GetModuleHandleW(nullptr), &state);
  if (!window) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND caption = CreateWindowW(L"STATIC", L"Отметьте существующие теги или быстро добавьте новый.",
      WS_CHILD | WS_VISIBLE, px(10), px(10), px(550), px(18), window, nullptr, nullptr, nullptr);
  state.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      px(10), px(31), px(550), px(219), window, reinterpret_cast<HMENU>(kList), nullptr, nullptr);
  const HWND new_caption = CreateWindowW(L"STATIC", L"Новый тег", WS_CHILD | WS_VISIBLE,
      px(10), px(261), px(370), px(18), window, nullptr, nullptr, nullptr);
  state.name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, px(10), px(280), px(370), px(25),
      window, reinterpret_cast<HMENU>(kName), nullptr, nullptr);
  const HWND add = CreateWindowW(L"BUTTON", L"Добавить и отметить", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(390), px(279), px(170), px(27), window, reinterpret_cast<HMENU>(kAdd), nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"Готово",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, px(370), px(320), px(90), px(28),
      window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(470), px(320), px(90), px(28), window, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(caption, state.font);
  SetControlFont(new_caption, state.font);
  SetControlFont(state.list, state.font);
  SetControlFont(state.name, state.font);
  const HFONT button_font = state.button_font ? state.button_font : state.font;
  SetControlFont(add, button_font);
  SetControlFont(accept, button_font);
  SetControlFont(cancel, button_font);
  ListView_SetExtendedListViewStyle(state.list,
      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW column{};
  column.mask = LVCF_WIDTH;
  column.cx = px(524);
  ListView_InsertColumn(state.list, 0, &column);
  for (const auto& tag : presentation::KnownTags(tags, styles)) {
    AddItem(state, tag, presentation::ContainsTag(assigned, tag));
  }
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  SetFocus(state.list);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(window)) DestroyWindow(window);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace ibstart::ui::dialog
