#include "ui/configuration_catalog_dialog.hpp"

#include "core/domain/utf.hpp"
#include "ui/dialog_support.hpp"

#include <commctrl.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace ibstart::ui::dialog {
namespace {
constexpr wchar_t kClass[] = L"IBStart.ConfigurationCatalog";
constexpr wchar_t kEditorClass[] = L"IBStart.ConfigurationEditor";
enum : int { kSearch = 1800, kList, kName, kTitle, kShort, kNew, kEdit, kDelete };
struct State {
  catalog::CatalogMetadataService* metadata{};
  HWND search{}, list{}, edit{}, remove{}, usage{}, choose{};
  HFONT font{};
  std::vector<std::wstring> visible;
  std::wstring selected;
  std::optional<std::wstring> result;
  bool select{}, done{}, refreshing{};
};
struct EditorState {
  catalog::CatalogMetadataService* metadata{};
  configuration::Definition draft;
  HWND name{}, title{}, short_name{};
  std::optional<std::wstring> result;
  bool done{};
};

std::wstring Text(HWND control) {
  std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
  const int size = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
  text.resize(static_cast<size_t>(size));
  return text;
}

LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* s = reinterpret_cast<EditorState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_CLOSE && s) { s->done = true; return 0; }
  if (message != WM_COMMAND || !s) return DefWindowProcW(window, message, wparam, lparam);
  if (LOWORD(wparam) == IDCANCEL) { s->done = true; return 0; }
  if (LOWORD(wparam) == IDOK) {
    try {
      auto draft = s->draft;
      draft.name = Text(s->name); draft.title = Text(s->title); draft.short_name = Text(s->short_name);
      if (draft.id.empty()) draft.id = configuration::NewId();
      s->metadata->SaveConfiguration(draft);
      s->result = draft.id; s->done = true;
    } catch (const std::exception& error) {
      MessageBoxW(window, utf::FromUtf8(error.what()).c_str(), L"Конфигурация: изменения не сохранены", MB_OK | MB_ICONWARNING);
    }
  }
  return 0;
}

std::optional<std::wstring> EditDefinition(HWND owner, catalog::CatalogMetadataService& metadata,
    configuration::Definition draft) {
  EditorState state;
  state.metadata = &metadata; state.draft = std::move(draft);
  static const ATOM atom = [] {
    WNDCLASSW klass{}; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = kEditorClass;
    klass.lpfnWndProc = EditorProc; klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const auto size = DialogOuterSize(owner, 480, 272, style, extended);
  const HWND window = CreateWindowExW(extended, kEditorClass,
      state.draft.id.empty() ? L"Добавить конфигурацию" : L"Изменить конфигурацию", style,
      CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  const HFONT font = CreateUiFont(window, 9, FW_NORMAL);
  const auto px = [window](int n) { return ScaleForDpi(n, GetDpiForWindow(window)); };
  const auto create = [&](const wchar_t* type, const wchar_t* text, DWORD flags, int x, int y, int w, int h, int id) {
    const HWND c = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | flags, px(x), px(y), px(w), px(h),
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(c, font); return c;
  };
  create(L"STATIC", L"Имя (свойство конфигурации 1С):", 0, 16, 16, 448, 20, 0);
  state.name = create(L"EDIT", state.draft.name.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 38, 448, 25, kName);
  create(L"STATIC", L"Наименование (синоним конфигурации 1С):", 0, 16, 78, 448, 20, 0);
  state.title = create(L"EDIT", state.draft.title.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 100, 448, 25, kTitle);
  create(L"STATIC", L"Краткое наименование:", 0, 16, 140, 448, 20, 0);
  state.short_name = create(L"EDIT", state.draft.short_name.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 162, 448, 25, kShort);
  for (const auto c : {state.name, state.title}) SendMessageW(c, EM_SETLIMITTEXT, 256, 0);
  SendMessageW(state.short_name, EM_SETLIMITTEXT, 80, 0);
  create(L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, 232, 228, 110, 28, IDOK);
  create(L"BUTTON", L"Отмена", WS_TABSTOP, 354, 228, 110, 28, IDCANCEL);
  PositionDialogNearOwner(window, owner); DisableModalOwner(owner); ShowWindow(window, SW_SHOW); SetFocus(state.name);
  MSG message{}; int pump = 1;
  while (!state.done && (pump = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  CloseModalDialog(window, owner);
  if (pump == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (font) DeleteObject(font);
  return state.result;
}

bool HasSelection(const State& state) {
  return ListView_GetNextItem(state.list, -1, LVNI_SELECTED) >= 0 &&
      configuration::Find(state.metadata->Read().configurations, state.selected);
}

void ShowSelection(State& state) {
  const auto& data = state.metadata->Read();
  const bool selected = HasSelection(state);
  const auto count = std::count_if(data.database_configurations.begin(), data.database_configurations.end(),
      [&](const auto& a) { return domain::EqualIdentifier(a.configuration_id, state.selected); });
  const auto usage = selected ? L"Связанных баз в профиле: " + std::to_wstring(count) : L"Выберите конфигурацию в таблице.";
  SetWindowTextW(state.usage, usage.c_str());
  EnableWindow(state.edit, selected); EnableWindow(state.remove, selected);
  EnableWindow(state.choose, state.select && selected);
}

void Refresh(State& state) {
  const auto query = Text(state.search);
  const auto& definitions = state.metadata->Read().configurations;
  std::vector<const configuration::Definition*> sorted;
  for (const auto& d : definitions) {
    const auto contains = [&](const auto& text) { return utf::FindNoCaseOrdinal(text, query) != std::wstring_view::npos; };
    if (query.empty() || contains(d.name) || contains(d.title) || contains(d.short_name)) sorted.push_back(&d);
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return configuration::Label(*a) < configuration::Label(*b); });
  state.refreshing = true;
  ListView_DeleteAllItems(state.list);
  state.visible.clear();
  for (const auto* d : sorted) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(state.visible.size());
    item.pszText = const_cast<wchar_t*>(d->name.c_str());
    const int index = ListView_InsertItem(state.list, &item);
    ListView_SetItemText(state.list, index, 1, const_cast<wchar_t*>(d->title.c_str()));
    ListView_SetItemText(state.list, index, 2, const_cast<wchar_t*>(d->short_name.c_str()));
    state.visible.push_back(d->id);
    if (domain::EqualIdentifier(d->id, state.selected))
      ListView_SetItemState(state.list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  const int selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
  if (selected >= 0) ListView_EnsureVisible(state.list, selected, FALSE);
  state.refreshing = false;
  ShowSelection(state);
}

void OpenEditor(HWND window, State& state, bool add) {
  configuration::Definition draft;
  if (!add) {
    if (!HasSelection(state)) return;
    draft = *configuration::Find(state.metadata->Read().configurations, state.selected);
  }
  if (const auto saved = EditDefinition(window, *state.metadata, std::move(draft))) {
    state.selected = *saved;
    SetWindowTextW(state.search, L""); Refresh(state);
  }
  SetFocus(state.list);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_CLOSE && s) { s->done = true; return 0; }
  if ((message != WM_COMMAND && message != WM_NOTIFY) || !s) return DefWindowProcW(window, message, wparam, lparam);
  try {
    if (message == WM_NOTIFY) {
      const auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom != s->list || s->refreshing) return 0;
      if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) &&
            !(change->uOldState & LVIS_SELECTED) && change->iItem >= 0 &&
            static_cast<size_t>(change->iItem) < s->visible.size()) {
          s->selected = s->visible[static_cast<size_t>(change->iItem)];
        }
        ShowSelection(*s);
      } else if (header->code == NM_DBLCLK) {
        const auto* click = reinterpret_cast<NMITEMACTIVATE*>(lparam);
        if (click->iItem >= 0 && static_cast<size_t>(click->iItem) < s->visible.size()) {
          if (s->select) { s->result = s->visible[static_cast<size_t>(click->iItem)]; s->done = true; }
          else OpenEditor(window, *s, false);
        }
      }
      return 0;
    }
    const int command = LOWORD(wparam);
    if (command == kSearch && HIWORD(wparam) == EN_CHANGE) { Refresh(*s); return 0; }
    if (command == kNew || command == kEdit) { OpenEditor(window, *s, command == kNew); return 0; }
    if (command == kDelete && HasSelection(*s)) {
      if (MessageBoxW(window, L"Удалить выбранную конфигурацию из справочника?", L"Конфигурации", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
      s->metadata->DeleteConfiguration(s->selected);
      s->selected.clear(); Refresh(*s); ShowSelection(*s); return 0;
    }
    if (command == IDOK && s->select && HasSelection(*s)) {
      s->result = s->selected; s->done = true; return 0;
    }
    if (command == IDCANCEL) { s->done = true; return 0; }
  } catch (const std::exception& error) {
    MessageBoxW(window, utf::FromUtf8(error.what()).c_str(), L"Конфигурации: изменения не сохранены", MB_OK | MB_ICONWARNING);
  }
  return 0;
}
}

std::optional<std::wstring> EditConfigurations(HWND owner, catalog::CatalogMetadataService& metadata, bool select, std::wstring initial_id) {
  State state;
  state.metadata = &metadata; state.select = select; state.selected = std::move(initial_id);
  static const ATOM atom = [] {
    WNDCLASSW klass{}; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = kClass;
    klass.lpfnWndProc = Proc; klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const auto size = DialogOuterSize(owner, 820, 460, style, extended);
  const HWND window = CreateWindowExW(extended, kClass, L"Конфигурации", style, CW_USEDEFAULT, CW_USEDEFAULT,
      size.cx, size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  const auto px = [window](int n) { return ScaleForDpi(n, GetDpiForWindow(window)); };
  const auto create = [&](const wchar_t* type, const wchar_t* text, DWORD flags, int x, int y, int w, int h, int id) {
    const HWND c = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | flags, px(x), px(y), px(w), px(h),
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(c, state.font); return c;
  };
  create(L"STATIC", L"Поиск по имени, наименованию или сокращению:", 0, 16, 14, 788, 20, 0);
  state.search = create(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 36, 788, 25, kSearch);
  state.list = create(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      16, 74, 788, 300, kList);
  ListView_SetExtendedListViewStyle(state.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  const wchar_t* headings[] = {L"Имя", L"Наименование", L"Краткое наименование"};
  const int widths[] = {250, 360, 160};
  for (int i = 0; i < 3; ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(headings[i]); column.cx = px(widths[i]); column.iSubItem = i;
    ListView_InsertColumn(state.list, i, &column);
  }
  state.usage = create(L"STATIC", L"", 0, 16, 388, 788, 20, 0);
  create(L"BUTTON", L"Добавить…", WS_TABSTOP, 16, 416, 112, 28, kNew);
  state.edit = create(L"BUTTON", L"Изменить…", WS_TABSTOP, 140, 416, 112, 28, kEdit);
  state.remove = create(L"BUTTON", L"Удалить", WS_TABSTOP, 264, 416, 112, 28, kDelete);
  if (select) state.choose = create(L"BUTTON", L"Выбрать", WS_TABSTOP | BS_DEFPUSHBUTTON, 572, 416, 110, 28, IDOK);
  create(L"BUTTON", select ? L"Отмена" : L"Закрыть", WS_TABSTOP, 694, 416, 110, 28, IDCANCEL);
  Refresh(state); ShowSelection(state);
  PositionDialogNearOwner(window, owner); ShowWindow(window, SW_SHOW); DisableModalOwner(owner); SetFocus(state.search);
  MSG message{}; int pump = 1;
  while (!state.done && (pump = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  CloseModalDialog(window, owner);
  if (pump == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (state.font) DeleteObject(state.font);
  return state.result;
}
}  // namespace ibstart::ui::dialog
