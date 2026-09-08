#include "ui/credential_manager_dialog.hpp"

#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>

#include "core/catalog/catalog.hpp"
#include "ui/dialog_support.hpp"
#pragma comment(lib, "comctl32.lib")

namespace ibstart::ui::dialog {
namespace {
constexpr wchar_t kManagerClass[] = L"IBStart.CredentialManager2";
constexpr wchar_t kEditorClass[] = L"IBStart.CredentialEditor2";
enum : int { kList = 2100, kAdd, kEdit, kDelete, kSave, kCancel };
enum : int {
  eTitle = 2200,
  eUser,
  ePassword,
  eAll,
  eSelected,
  eTree,
  eLinks,
  eRemove,
  eRebind,
  eSave,
  eCancel
};
struct Editor {
  HWND w{}, title{}, user{}, password{}, all{}, selected{}, tree{}, links{};
  HIMAGELIST images{};
  HFONT font{}, button_font{};
  credentials::Credential draft;
  const std::vector<credentials::Credential>* records{};
  std::filesystem::path path;
  const catalog::Catalog* catalog{};
  std::vector<size_t> indices;
  std::optional<credentials::Credential> result;
  bool done{};
};
struct Manager {
  HWND w{}, list{}, edit{}, remove{};
  HFONT font{}, button_font{};
  std::vector<credentials::Credential> records;
  std::filesystem::path path;
  const catalog::Catalog* catalog{};
  std::optional<std::vector<credentials::Credential>> result;
  bool done{};
};
void Msg(HWND w, std::wstring_view text, std::wstring_view title = L"Учётные записи",
         UINT flags = MB_OK | MB_ICONWARNING) {
  const std::wstring a(text), b(title);
  MessageBoxW(w, a.c_str(), b.c_str(), flags);
}
std::wstring Read(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(static_cast<size_t>(n));
  return s;
}
std::wstring Trim(std::wstring s) {
  size_t a = 0, b = s.size();
  while (a < b && std::iswspace(s[a])) ++a;
  while (b > a && std::iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}
std::wstring NewId() {
  GUID g{};
  if (FAILED(CoCreateGuid(&g))) return {};
  wchar_t b[40]{};
  return StringFromGUID2(g, b, 40) == 0 ? std::wstring{} : std::wstring(b);
}
bool Checked(HWND t, HTREEITEM i) {
  TVITEMW x{};
  x.mask = TVIF_STATE;
  x.hItem = i;
  x.stateMask = TVIS_STATEIMAGEMASK;
  return TreeView_GetItem(t, &x) && ((x.state & TVIS_STATEIMAGEMASK) >> 12) == 2;
}
void Check(HWND t, HTREEITEM i, bool v) {
  TVITEMW x{};
  x.mask = TVIF_STATE;
  x.hItem = i;
  x.stateMask = TVIS_STATEIMAGEMASK;
  x.state = INDEXTOSTATEIMAGEMASK(v ? 2 : 1);
  TreeView_SetItem(t, &x);
}
const domain::Entry* Entry(HWND t, HTREEITEM i, const catalog::Catalog* c) {
  if (!c) return nullptr;
  TVITEMW x{};
  x.mask = TVIF_PARAM;
  x.hItem = i;
  if (!TreeView_GetItem(t, &x)) return nullptr;
  return c->FindBySectionIndex(static_cast<size_t>(x.lParam));
}
bool Present(const credentials::Target& t, const std::filesystem::path& p,
             const catalog::Catalog* c) {
  return c && credentials::SameCatalogPath(t.catalog_path, p) &&
         std::any_of(c->document().sections.begin(), c->document().sections.end(),
                     [&](const auto& s) { return credentials::TargetMatches(t, p, s.entry); });
}
void Scope(Editor& s, credentials::ScopeMode m) {
  s.draft.scope = m;
  SendMessageW(s.all, BM_SETCHECK, m == credentials::ScopeMode::all ? BST_CHECKED : BST_UNCHECKED,
               0);
  SendMessageW(s.selected, BM_SETCHECK,
               m == credentials::ScopeMode::selected ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(s.tree, m == credentials::ScopeMode::selected && s.catalog);
}
void Build(Editor& s, const std::vector<catalog::TreeItem>& items, HTREEITEM parent) {
  for (const auto& x : items) {
    TVINSERTSTRUCTW i{};
    i.hParent = parent;
    i.item.mask = TVIF_TEXT | TVIF_PARAM;
    i.item.pszText = const_cast<wchar_t*>(x.name.c_str());
    i.item.lParam = static_cast<LPARAM>(x.section_index);
    HTREEITEM h = TreeView_InsertItem(s.tree, &i);
    bool on = false;
    if (const auto* e = Entry(s.tree, h, s.catalog))
      on = std::any_of(s.draft.targets.begin(), s.draft.targets.end(),
                       [&](const auto& t) { return credentials::TargetMatches(t, s.path, *e); });
    Check(s.tree, h, on);
    Build(s, x.children, h);
  }
}
std::vector<credentials::Target> Gather(Editor& s) {
  std::vector<credentials::Target> r;
  std::function<void(HTREEITEM)> walk = [&](HTREEITEM i) {
    for (HTREEITEM h = i; h; h = TreeView_GetNextSibling(s.tree, h)) {
      if (Checked(s.tree, h))
        if (const auto* e = Entry(s.tree, h, s.catalog))
          r.push_back(credentials::MakeTarget(s.path, *e));
      walk(TreeView_GetChild(s.tree, h));
    }
  };
  if (s.catalog) walk(TreeView_GetRoot(s.tree));
  for (const auto& t : s.draft.targets)
    if (!Present(t, s.path, s.catalog)) r.push_back(t);
  return r;
}
void Links(Editor& s) {
  SendMessageW(s.links, LB_RESETCONTENT, 0, 0);
  s.indices.clear();
  for (size_t i = 0; i < s.draft.targets.size(); ++i) {
    const auto& t = s.draft.targets[i];
    if (Present(t, s.path, s.catalog)) continue;
    std::wstring v = t.last_known_name.empty() ? L"(без названия)" : t.last_known_name;
    v += L" — " + t.catalog_path.wstring();
    if (credentials::SameCatalogPath(t.catalog_path, s.path)) v += L" [элемент не найден]";
    SendMessageW(s.links, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v.c_str()));
    s.indices.push_back(i);
  }
}
void Refresh(Editor& s) {
  TreeView_DeleteAllItems(s.tree);
  if (s.catalog) Build(s, s.catalog->Tree(), TVI_ROOT);
  Links(s);
  Scope(s, s.draft.scope);
}
bool ReadDraft(HWND owner, Editor& s, credentials::Credential& out) {
  out = s.draft;
  out.title = Trim(Read(s.title));
  out.user_name = Trim(Read(s.user));
  out.password = Read(s.password);
  if (SendMessageW(s.all, BM_GETCHECK, 0, 0) == BST_CHECKED)
    out.scope = credentials::ScopeMode::all;
  else if (SendMessageW(s.selected, BM_GETCHECK, 0, 0) == BST_CHECKED)
    out.scope = credentials::ScopeMode::selected;
  else
    out.scope = credentials::ScopeMode::invalid;
  out.targets = Gather(s);
  if (out.scope == credentials::ScopeMode::selected) {
    if (out.targets.empty()) {
      Msg(owner, L"Выберите хотя бы одну базу или группу.");
      return false;
    }
  }
  const std::wstring err = credentials::Validate(out, *s.records);
  if (!err.empty()) {
    Msg(owner, err);
    return false;
  }
  return true;
}
void Remove(Editor& s) {
  int r = static_cast<int>(SendMessageW(s.links, LB_GETCURSEL, 0, 0));
  if (r < 0 || r >= static_cast<int>(s.indices.size())) return;
  const credentials::Target old = s.draft.targets[s.indices[static_cast<size_t>(r)]];
  s.draft.targets = Gather(s);
  auto it = std::find(s.draft.targets.begin(), s.draft.targets.end(), old);
  if (it != s.draft.targets.end()) s.draft.targets.erase(it);
  Refresh(s);
}
void Rebind(HWND owner, Editor& s) {
  int r = static_cast<int>(SendMessageW(s.links, LB_GETCURSEL, 0, 0));
  if (r < 0 || r >= static_cast<int>(s.indices.size()) || s.path.empty()) return;
  const credentials::Target old = s.draft.targets[s.indices[static_cast<size_t>(r)]];
  const std::wstring q =
      L"Перепривязать\n" + old.catalog_path.wstring() + L"\nк\n" + s.path.wstring() + L"?";
  if (MessageBoxW(owner, q.c_str(), L"Перепривязка", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  credentials::Credential c = s.draft;
  c.targets = Gather(s);
  auto it = std::find(c.targets.begin(), c.targets.end(), old);
  if (it == c.targets.end()) return;
  it->catalog_path = credentials::NormalizeCatalogPath(s.path);
  s.draft = std::move(c);
  Refresh(s);
}
LRESULT CALLBACK EditorProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  auto* s = reinterpret_cast<Editor*>(GetWindowLongPtrW(w, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    s = reinterpret_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
  }
  if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) return DialogControlColor(m, wp, lp);
  if (!s) return DefWindowProcW(w, m, wp, lp);
  if (m == WM_COMMAND) {
    int id = LOWORD(wp);
    if (id == eAll || id == eSelected)
      Scope(*s, id == eAll ? credentials::ScopeMode::all : credentials::ScopeMode::selected);
    else if (id == eRemove)
      Remove(*s);
    else if (id == eRebind)
      Rebind(w, *s);
    else if (id == IDOK || id == eSave) {
      credentials::Credential c;
      if (ReadDraft(w, *s, c)) {
        s->result = std::move(c);
        s->done = true;
      }
    } else if (id == IDCANCEL || id == eCancel) {
      s->done = true;
    }
    return 0;
  }
  if (m == WM_CLOSE) {
    s->done = true;
    return 0;
  }
  return DefWindowProcW(w, m, wp, lp);
}
bool Register(const wchar_t* name, WNDPROC proc) {
  WNDCLASSW c{};
  c.hInstance = GetModuleHandleW(nullptr);
  c.lpszClassName = name;
  c.lpfnWndProc = proc;
  c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  c.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  return RegisterClassW(&c) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
std::optional<credentials::Credential> EditOne(HWND owner, const credentials::Credential& input,
                                               const std::vector<credentials::Credential>& records,
                                               const std::filesystem::path& path,
                                               const catalog::Catalog* catalog) {
  Editor s;
  s.draft = input;
  s.records = &records;
  s.path = path;
  s.catalog = catalog;
  if (!Register(kEditorClass, EditorProc)) return std::nullopt;
  DisableModalOwner(owner);
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP,
                  ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  SIZE z = DialogOuterSize(owner, 720, 610, style, ex);
  s.w = CreateWindowExW(ex, kEditorClass, L"Учётная запись", style, CW_USEDEFAULT, CW_USEDEFAULT,
                        z.cx, z.cy, owner, nullptr, GetModuleHandleW(nullptr), &s);
  if (!s.w) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  s.font = CreateUiFont(s.w, 9, FW_NORMAL);
  s.button_font = CreateUiFont(s.w, 9, FW_NORMAL);
  UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  auto px = [dpi](int x) { return ScaleForDpi(x, dpi); };
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD e, DWORD st, int x, int y, int ww,
                  int hh, int id, bool button = false) {
    HWND h =
        CreateWindowExW(e, cls, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(ww), px(hh), s.w,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SetControlFont(h, button ? (s.button_font ? s.button_font : s.font) : s.font);
    return h;
  };
  make(L"STATIC", L"Название", 0, 0, 10, 10, 330, 18, 0);
  s.title =
      make(L"EDIT", L"", WS_EX_CLIENTEDGE, ES_AUTOHSCROLL | WS_TABSTOP, 10, 31, 330, 25, eTitle);
  make(L"STATIC", L"Пользователь", 0, 0, 360, 10, 330, 18, 0);
  s.user =
      make(L"EDIT", L"", WS_EX_CLIENTEDGE, ES_AUTOHSCROLL | WS_TABSTOP, 360, 31, 330, 25, eUser);
  make(L"STATIC", L"Пароль (хранится открытым текстом)", 0, 0, 10, 67, 680, 18, 0);
  s.password =
      make(L"EDIT", L"", WS_EX_CLIENTEDGE, ES_AUTOHSCROLL | WS_TABSTOP, 10, 88, 680, 25, ePassword);
  std::wstring cat = L"Каталог: " + path.wstring();
  make(L"STATIC", cat.c_str(), 0, 0, 10, 123, 680, 18, 0);
  s.all = make(L"BUTTON", L"Все базы", 0, BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 10, 151, 180,
               25, eAll, true);
  s.selected = make(L"BUTTON", L"Выбранные базы и группы", 0, BS_AUTORADIOBUTTON | WS_TABSTOP, 205,
                    151, 250, 25, eSelected, true);
  make(L"STATIC", L"Выбор группы включает её дочерние элементы.", 0, 0, 10, 176, 680, 18, 0);
  s.tree = make(L"SysTreeView32", L"", WS_EX_CLIENTEDGE,
                TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_CHECKBOXES | WS_TABSTOP, 10,
                198, 680, 216, eTree);
  make(L"STATIC", L"Сохранённые ссылки вне текущего каталога или не найденные:", 0, 0, 10, 425, 680,
       18, 0);
  s.links = make(L"LISTBOX", L"", WS_EX_CLIENTEDGE, LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
                 10, 445, 680, 70, eLinks);
  make(L"BUTTON", L"Удалить ссылку", 0, WS_TABSTOP, 10, 525, 150, 28, eRemove, true);
  make(L"BUTTON", L"Перепривязать…", 0, WS_TABSTOP, 170, 525, 170, 28, eRebind, true);
  make(L"BUTTON", L"Сохранить", 0, WS_TABSTOP | BS_DEFPUSHBUTTON, 500, 560, 90, 28, eSave, true);
  make(L"BUTTON", L"Отмена", 0, WS_TABSTOP, 600, 560, 90, 28, eCancel, true);
  SetWindowTextW(s.title, s.draft.title.c_str());
  SetWindowTextW(s.user, s.draft.user_name.c_str());
  SetWindowTextW(s.password, s.draft.password.c_str());
  Refresh(s);
  PositionDialogNearOwner(s.w, owner);
  ShowWindow(s.w, SW_SHOW);
  s.images = TreeView_GetImageList(s.tree, TVSIL_STATE);
  SetFocus(s.title);
  MSG msg{};
  int r = 1;
  while (!s.done && (r = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(s.w, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CloseModalDialog(s.w, owner);
  if (s.images) ImageList_Destroy(s.images);
  if (s.font) DeleteObject(s.font);
  if (s.button_font) DeleteObject(s.button_font);
  if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));
  return s.result;
}
void List(Manager& s, int selected = -1) {
  ListView_DeleteAllItems(s.list);
  for (int i = 0; i < static_cast<int>(s.records.size()); ++i) {
    auto& c = s.records[static_cast<size_t>(i)];
    LVITEMW x{};
    x.mask = LVIF_TEXT;
    x.iItem = i;
    x.pszText = const_cast<wchar_t*>(c.title.c_str());
    ListView_InsertItem(s.list, &x);
    ListView_SetItemText(s.list, i, 1, const_cast<wchar_t*>(c.user_name.c_str()));
    ListView_SetItemText(s.list, i, 2, const_cast<wchar_t*>(c.password.c_str()));
    ListView_SetItemText(
        s.list, i, 3,
        const_cast<wchar_t*>(c.scope == credentials::ScopeMode::all ? L"Все базы" : L"Выбранные"));
  }
  EnableWindow(s.edit, selected >= 0);
  EnableWindow(s.remove, selected >= 0);
  if (selected >= 0)
    ListView_SetItemState(s.list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
}
void Edit(Manager& s, HWND owner, int i) {
  if (i < 0 || i >= static_cast<int>(s.records.size())) return;
  if (auto c = EditOne(owner, s.records[static_cast<size_t>(i)], s.records, s.path, s.catalog)) {
    s.records[static_cast<size_t>(i)] = std::move(*c);
    List(s, i);
  }
}
LRESULT CALLBACK ManagerProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  auto* s = reinterpret_cast<Manager*>(GetWindowLongPtrW(w, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    s = reinterpret_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
  }
  if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) return DialogControlColor(m, wp, lp);
  if (!s) return DefWindowProcW(w, m, wp, lp);
  if (m == WM_NOTIFY) {
    auto* n = reinterpret_cast<const NMHDR*>(lp);
    if (n && n->idFrom == kList && n->code == LVN_ITEMCHANGED) {
      const int selected = ListView_GetNextItem(s->list, -1, LVNI_SELECTED);
      EnableWindow(s->edit, selected >= 0);
      EnableWindow(s->remove, selected >= 0);
    }
    if (n && n->idFrom == kList && n->code == NM_DBLCLK) {
      Edit(*s, w, ListView_GetNextItem(s->list, -1, LVNI_SELECTED));
      return 0;
    }
  }
  if (m == WM_COMMAND) {
    int id = LOWORD(wp), i = ListView_GetNextItem(s->list, -1, LVNI_SELECTED);
    if (id == kAdd) {
      credentials::Credential c;
      c.id = NewId();
      if (c.id.empty())
        Msg(w, L"Не удалось создать идентификатор.", L"Учётные записи", MB_OK | MB_ICONERROR);
      else if (auto x = EditOne(w, c, s->records, s->path, s->catalog)) {
        s->records.push_back(std::move(*x));
        List(*s, static_cast<int>(s->records.size()) - 1);
      }
    } else if (id == kEdit)
      Edit(*s, w, i);
    else if (id == kDelete && i >= 0 &&
             MessageBoxW(w, L"Удалить выбранную учётную запись?", L"Учётные записи",
                         MB_YESNO | MB_ICONQUESTION) == IDYES) {
      s->records.erase(s->records.begin() + i);
      List(*s, std::min(i, static_cast<int>(s->records.size()) - 1));
    } else if (id == IDOK || id == kSave) {
      s->result = s->records;
      s->done = true;
    } else if (id == IDCANCEL || id == kCancel) {
      s->done = true;
    }
    return 0;
  }
  if (m == WM_CLOSE) {
    s->done = true;
    return 0;
  }
  return DefWindowProcW(w, m, wp, lp);
}
}  // namespace
std::optional<std::vector<credentials::Credential>> EditCredentialManager(
    HWND owner, const std::vector<credentials::Credential>& records,
    const std::filesystem::path& path, const catalog::Catalog* catalog) {
  Manager s;
  s.records = records;
  s.path = path;
  s.catalog = catalog;
  if (!Register(kManagerClass, ManagerProc)) return std::nullopt;
  DisableModalOwner(owner);
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP,
                  ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  SIZE z = DialogOuterSize(owner, 820, 440, style, ex);
  s.w = CreateWindowExW(ex, kManagerClass, L"Учётные записи", style, CW_USEDEFAULT, CW_USEDEFAULT,
                        z.cx, z.cy, owner, nullptr, GetModuleHandleW(nullptr), &s);
  if (!s.w) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  s.font = CreateUiFont(s.w, 9, FW_NORMAL);
  s.button_font = CreateUiFont(s.w, 9, FW_NORMAL);
  UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  auto px = [dpi](int x) { return ScaleForDpi(x, dpi); };
  auto make = [&](const wchar_t* cls, DWORD e, DWORD st, int x, int y, int ww, int hh, int id) {
    HWND h =
        CreateWindowExW(e, cls, L"", WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(ww), px(hh), s.w,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SetControlFont(h, s.font);
    return h;
  };
  s.list =
      make(WC_LISTVIEWW, WS_EX_CLIENTEDGE,
           LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 10, 10, 800, 345, kList);
  ListView_SetExtendedListViewStyle(s.list,
                                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
  const wchar_t* h[] = {L"Название", L"Пользователь", L"Пароль", L"Область"};
  const int ww[] = {220, 190, 190, 150};
  for (int j = 0; j < 4; ++j) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.cx = px(ww[j]);
    c.pszText = const_cast<wchar_t*>(h[j]);
    ListView_InsertColumn(s.list, j, &c);
  }
  auto button = [&](const wchar_t* text, int x, int id) {
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(x), px(365),
                           px(110), px(28), s.w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           nullptr, nullptr);
    SetControlFont(b, s.button_font ? s.button_font : s.font);
    return b;
  };
  button(L"Добавить…", 10, kAdd);
  s.edit = button(L"Изменить…", 130, kEdit);
  s.remove = button(L"Удалить", 250, kDelete);
  button(L"Сохранить", 580, kSave);
  button(L"Отмена", 700, kCancel);
  List(s, s.records.empty() ? -1 : 0);
  PositionDialogNearOwner(s.w, owner);
  ShowWindow(s.w, SW_SHOW);
  SetFocus(s.list);
  MSG msg{};
  int r = 1;
  while (!s.done && (r = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(s.w, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CloseModalDialog(s.w, owner);
  if (s.font) DeleteObject(s.font);
  if (s.button_font) DeleteObject(s.button_font);
  if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));
  return s.result;
}
}  // namespace ibstart::ui::dialog
