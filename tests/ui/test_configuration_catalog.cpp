#include "ui/main_window.hpp"
#include "ui/command_ids.hpp"
#include "core/domain/utf.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures{}, phase{}, ticks{};
std::filesystem::path profile_root, list_path, captures;
std::string original_list;
void Check(bool condition, const char* description) {
  if (!condition) { std::cerr << "FAILED " << description << '\n'; ++failures; }
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary); return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
HWND Window(std::wstring_view klass) {
  struct Search { std::wstring_view klass; HWND result{}; } search{klass};
  EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
    auto& value = *reinterpret_cast<Search*>(parameter);
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (value.klass == name) { value.result = window; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}
HTREEITEM Find(HWND tree, HTREEITEM item, std::wstring_view name) {
  for (; item; item = TreeView_GetNextSibling(tree, item)) {
    wchar_t text[256]{}; TVITEMW data{}; data.mask = TVIF_TEXT; data.hItem = item;
    data.pszText = text; data.cchTextMax = 256; TreeView_GetItem(tree, &data);
    if (name == text) return item;
    if (const auto child = Find(tree, TreeView_GetChild(tree, item), name)) return child;
  }
  return nullptr;
}
void Capture(HWND window, const wchar_t* name) {
  if (captures.empty()) return;
  RECT rect{}; GetWindowRect(window, &rect);
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  const HDC screen = GetDC(window), dc = CreateCompatibleDC(screen);
  BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  void* pixels{}; const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (bitmap) {
    const auto previous = SelectObject(dc, bitmap);
    RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    Check(PrintWindow(window, dc, 0) != FALSE, "Capture actual application window");
    GdiFlush();
    const DWORD bytes = static_cast<DWORD>(width * height * 4);
    const BITMAPFILEHEADER header{0x4d42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bytes,
        0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    std::ofstream stream(captures / name, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels), bytes);
    Check(stream.good(), "Write application evidence");
    SelectObject(dc, previous); DeleteObject(bitmap);
  } else Check(false, "Allocate application screenshot");
  DeleteDC(dc); ReleaseDC(window, screen);
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
  if (++ticks > 1500) { Check(false, "Application driver timeout"); PostQuitMessage(1); return; }
  const auto main = Window(L"IBStart.MainWindow");
  if (!main) return;
  const auto tree = FindWindowExW(main, nullptr, WC_TREEVIEWW, nullptr);
  if (phase == 0) {
    if (!tree || TreeView_GetCount(tree) < 3) return;
    const auto selected = Find(tree, TreeView_GetRoot(tree), L"Торговая база");
    Check(selected != nullptr, "Find synthetic base in actual main window");
    TreeView_SelectItem(tree, selected);
    phase = 1;
    SendMessageW(main, WM_COMMAND, ibstart::ui::commands::kEdit, 0);
    return;
  }
  if (phase == 1) {
    const auto editor = Window(L"IBStart.DatabaseEditor");
    if (!editor) return;
    Check(GetWindowTextLengthW(editor) > 0, "Database editor has its window title");
    const auto combo = GetDlgItem(editor, 1015);
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    int selection = -1;
    for (int i = 0; i < count; ++i) {
      const auto size = SendMessageW(combo, CB_GETLBTEXTLEN, i, 0);
      if (size == CB_ERR) continue;
      std::wstring text(static_cast<size_t>(size) + 1, L'\0');
      SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(text.data()));
      if (text.starts_with(L"УТ11 —")) selection = i;
    }
    Check(selection >= 0, "Predefined UT11 available in actual editor");
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    SetWindowTextW(GetDlgItem(editor, 1016), L"11.5.22.123");
    Capture(editor, L"configuration-editor.bmp");
    phase = 2;
    SendMessageW(editor, WM_COMMAND, IDOK, 0);
    return;
  }
  if (phase == 2) {
    if (Window(L"IBStart.DatabaseEditor")) return;
    const auto state = ibstart::storage::LoadCatalogState({profile_root, true});
    const auto* a = ibstart::configuration::FindAssignment(state.database_configurations,
        ibstart::configuration::NormalizeCatalogPath(list_path), L"Торговая база");
    Check(a && a->version == L"11.5.22.123", "Main window persists manual version");
    Check(a && ibstart::configuration::Find(state.configurations, a->configuration_id), "Main window persists valid dictionary reference");
    Check(Read(list_path) == original_list, "Metadata-only edit leaves exact ibases.v8i bytes unchanged");
    Check(std::none_of(std::filesystem::directory_iterator(profile_root), std::filesystem::directory_iterator{},
        [](const auto& entry) { return entry.path().filename().wstring().find(L".bak_") != std::wstring::npos; }),
        "Metadata-only edit does not create ibases backups");
    Capture(main, L"configuration-main.bmp");
    const auto search = FindWindowExW(main, nullptr, L"EDIT", nullptr);
    SetWindowTextW(search, L"УТ11");
    phase = 3;
    return;
  }
  if (phase == 3) {
    if (!tree || TreeView_GetCount(tree) != 2) return;
    Check(Find(tree, TreeView_GetRoot(tree), L"Торговая база") != nullptr, "Configuration search keeps assigned base");
    Check(Find(tree, TreeView_GetRoot(tree), L"Другая база") == nullptr, "Configuration search excludes unrelated base");
    Capture(main, L"configuration-search.bmp");
    phase = 4;
    SendMessageW(main, WM_COMMAND, ibstart::ui::commands::kConfigureConfigurations, 0);
    return;
  }
  if (phase == 4) {
    const auto dialog = Window(L"IBStart.ConfigurationCatalog");
    if (!dialog) return;
    Check(GetWindowTextLengthW(dialog) > 0, "Configuration dictionary has its window title");
    const auto list = GetDlgItem(dialog, 1801);
    Check((GetWindowLongPtrW(list, GWL_STYLE) & LVS_TYPEMASK) == LVS_REPORT, "Dictionary uses a table");
    Check(Header_GetItemCount(ListView_GetHeader(list)) == 3, "Dictionary has three columns");
    const wchar_t* headings[] = {L"Имя", L"Наименование", L"Краткое наименование"};
    for (int column = 0; column < 3; ++column) {
      wchar_t text[128]{};
      LVCOLUMNW data{}; data.mask = LVCF_TEXT; data.pszText = text; data.cchTextMax = 128;
      Check(ListView_GetColumn(list, column, &data) && std::wstring_view(text) == headings[column],
          "Dictionary column has its field heading");
    }
    const int count = ListView_GetItemCount(list);
    int selected = -1;
    for (int i = 0; i < count; ++i) {
      wchar_t text[256]{};
      ListView_GetItemText(list, i, 2, text, 256);
      if (std::wstring_view(text) == L"УТ11") {
        selected = i;
        ListView_GetItemText(list, i, 0, text, 256);
        Check(std::wstring_view(text) == L"УправлениеТорговлей", "Name is a separate table cell");
        ListView_GetItemText(list, i, 1, text, 256);
        Check(std::wstring_view(text) == L"Управление торговлей, редакция 11", "Title is a separate table cell");
        ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(!GetDlgItem(dialog, 1802), "Dictionary contains no inline editor");
        Check(IsWindowEnabled(GetDlgItem(dialog, 1806)), "Selected row enables Edit");
      }
    }
    Check(selected >= 0, "UT11 row found in dictionary table");
    for (const auto query : {L"УТ11", L"УправлениеТорговлей", L"редакция 11"}) {
      SetWindowTextW(GetDlgItem(dialog, 1800), query);
      Check(ListView_GetItemCount(list) >= 1 && ListView_GetNextItem(list, -1, LVNI_SELECTED) >= 0,
          "Filtering by each field preserves the selected row");
    }
    SetWindowTextW(GetDlgItem(dialog, 1800), L"no-such-configuration");
    Check(ListView_GetItemCount(list) == 0, "No-match filter empties dictionary table");
    Check(!IsWindowEnabled(GetDlgItem(dialog, 1806)) && !IsWindowEnabled(GetDlgItem(dialog, 1807)),
        "Empty selection disables Edit and Delete");
    SetWindowTextW(GetDlgItem(dialog, 1800), L"");
    Check(ListView_GetItemCount(list) == count && ListView_GetNextItem(list, -1, LVNI_SELECTED) == selected,
        "Clearing filter restores rows and selected configuration");
    Check(IsWindowEnabled(GetDlgItem(dialog, 1806)) && IsWindowEnabled(GetDlgItem(dialog, 1807)),
        "Restoring selection re-enables Edit and Delete");
    Capture(dialog, L"configuration-catalog.bmp");
    phase = 5;
    SendMessageW(dialog, WM_COMMAND, 1806, 0);
    return;
  }
  if (phase == 5 || phase == 7) {
    const auto editor = Window(L"IBStart.ConfigurationEditor");
    if (!editor) return;
    Check(!IsWindowEnabled(Window(L"IBStart.ConfigurationCatalog")), "Dictionary disabled while editing a record");
    wchar_t text[256]{};
    GetWindowTextW(editor, text, 256);
    Check(std::wstring_view(text) == L"Изменить конфигурацию", "Edit opens its separate titled window");
    const wchar_t* expected[] = {L"УправлениеТорговлей", L"Управление торговлей, редакция 11", L"УТ11"};
    for (int field = 0; field < 3; ++field) {
      GetWindowTextW(GetDlgItem(editor, 1802 + field), text, 256);
      Check(std::wstring_view(text) == expected[field], "Edit prefills every field from selected record");
    }
    Capture(editor, L"configuration-record-edit.bmp");
    SetWindowTextW(GetDlgItem(editor, 1803), L"Управление торговлей, тестовая редакция 11");
    const bool cancel = phase == 5;
    ++phase;
    if (cancel) SendMessageW(editor, WM_CLOSE, 0, 0);
    else SendMessageW(editor, WM_COMMAND, IDOK, 0);
    return;
  }
  if (phase == 6 || phase == 8) {
    if (Window(L"IBStart.ConfigurationEditor")) return;
    const auto dialog = Window(L"IBStart.ConfigurationCatalog");
    Check(IsWindowEnabled(dialog), "Dictionary is usable after record editor closes");
    const auto state = ibstart::storage::LoadCatalogState({profile_root, true});
    const auto* assignment = ibstart::configuration::FindAssignment(state.database_configurations,
        ibstart::configuration::NormalizeCatalogPath(list_path), L"Торговая база");
    const auto* definition = assignment ? ibstart::configuration::Find(state.configurations, assignment->configuration_id) : nullptr;
    Check(assignment && assignment->version == L"11.5.22.123", "Record edit preserves base assignment and version");
    Check(definition && definition->title == (phase == 6 ? L"Управление торговлей, редакция 11" : L"Управление торговлей, тестовая редакция 11"),
        "Cancel preserves stored record; Save persists changes under the same ID");
    Check(state.configurations.size() == 13, "Editing does not add another record");
    const auto list = GetDlgItem(dialog, 1801);
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    Check(row >= 0, "Edited record remains selected");
    if (phase == 6) {
      Check(IsWindowEnabled(GetDlgItem(dialog, 1806)), "Edit stays enabled after cancelling editor");
      Capture(dialog, L"configuration-catalog.bmp");
      phase = 7;
      NMITEMACTIVATE click{}; click.hdr = {list, 1801, NM_DBLCLK}; click.iItem = row;
      SendMessageW(dialog, WM_NOTIFY, 1801, reinterpret_cast<LPARAM>(&click));
    } else {
      wchar_t text[256]{}; ListView_GetItemText(list, row, 1, text, 256);
      Check(std::wstring_view(text) == L"Управление торговлей, тестовая редакция 11", "Saved changes appear in table immediately");
      phase = 9;
    }
    return;
  }
  if (phase == 9) {
    phase = 10;
    SendMessageW(Window(L"IBStart.ConfigurationCatalog"), WM_COMMAND, 1805, 0);
    return;
  }
  if (phase == 10) {
    const auto editor = Window(L"IBStart.ConfigurationEditor");
    if (!editor) return;
    for (const auto field : {1802, 1803, 1804})
      Check(GetWindowTextLengthW(GetDlgItem(editor, field)) == 0, "Add after editing starts with empty fields");
    Capture(editor, L"configuration-record-add.bmp");
    phase = 11;
    SendMessageW(editor, WM_COMMAND, IDCANCEL, 0);
    return;
  }
  if (phase == 11) {
    if (Window(L"IBStart.ConfigurationEditor")) return;
    phase = 12;
    SendMessageW(Window(L"IBStart.ConfigurationCatalog"), WM_COMMAND, IDCANCEL, 0);
    return;
  }
  if (phase == 12) { phase = 13; SendMessageW(main, WM_CLOSE, 0, 0); }
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--capture") {
    captures = argv[2]; std::filesystem::create_directories(captures);
  }
  const auto previous = GetThreadDesktop(GetCurrentThreadId());
  const auto name = L"IBStart.ConfigurationTests." + std::to_wstring(GetCurrentProcessId());
  const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
      DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) return 1;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES};
  InitCommonControlsEx(&controls);
  profile_root = std::filesystem::temp_directory_path() / (L"ibstart-configuration-app-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(profile_root);
  list_path = profile_root / L"ibases.v8i";
  original_list = ibstart::utf::ToUtf8(L"[Торговая база]\r\nConnect=Srvr=\"demo-server\";Ref=\"trade\";\r\n[Другая база]\r\nConnect=Srvr=\"demo-server\";Ref=\"other\";\r\n");
  { std::ofstream stream(list_path, std::ios::binary); stream << original_list; }
  ibstart::storage::Settings settings;
  settings.active_ibases = list_path; settings.selected_entry = L"Торговая база";
  settings.window_width = 1180; settings.window_height = 650;
  settings.show_cache_size_in_list = false;
  settings.check_updates_on_startup = false;
  ibstart::storage::SaveSettings({profile_root, true}, settings);
  auto state = ibstart::storage::LoadCatalogState({profile_root, true});
  state.tags[L"Торговая база"] = {L"Продуктив"};
  ibstart::storage::SaveCatalogState({profile_root, true}, state);
  const auto timer = SetTimer(nullptr, 0, 20, Drive);
  {
    ibstart::ui::MainWindow application(GetModuleHandleW(nullptr), std::filesystem::path(argv[0]), {profile_root, true});
    static_cast<void>(application.Show(SW_SHOWNOACTIVATE));
  }
  KillTimer(nullptr, timer);
  Check(phase == 13, "Application scenario completed");
  if (SUCCEEDED(com)) CoUninitialize();
  SetThreadDesktop(previous); CloseDesktop(desktop);
  std::error_code error; std::filesystem::remove_all(profile_root, error);
  if (failures) return 1;
  std::cout << "All configuration application contracts passed (isolated profile and desktop)\n";
  return 0;
}
