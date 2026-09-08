#include "ui/application_settings_dialog.hpp"

#include "ui/dialog_support.hpp"

#include <CommCtrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"IBStart.ApplicationSettings";
constexpr wchar_t kGroupClassName[] = L"IBStart.ApplicationSettings.Group";
constexpr int kSimpleMode = 1800;
constexpr int kShowTags = 1801;
constexpr int kFoldersFirst = 1802;
constexpr int kEditCredentials = 1803;
constexpr int kEditTags = 1804;
constexpr int kPlatformPaths = 1805;
constexpr int kAddPlatformPath = 1806;
constexpr int kRemovePlatformPath = 1807;
constexpr int kRecentLists = 1808;
constexpr int kRecentLimit = 1809;
constexpr int kRemoveRecentList = 1810;
constexpr int kClearRecentLists = 1811;
constexpr int kClearRecentBases = 1812;
constexpr int kResetWindowLayout = 1813;
constexpr int kOpenLastList = 1814;
constexpr int kRestoreSelection = 1815;
constexpr int kConfirmDestructive = 1816;
constexpr int kConfirmSecret = 1817;
constexpr int kDefaultClient = 1818;
constexpr int kDefaultArchitecture = 1819;
constexpr int kDefaultVersion = 1820;
constexpr int kShowDetails = 1821;
constexpr int kShowStatus = 1822;
constexpr int kRememberHistory = 1823;
constexpr int kTreeDensity = 1824;
constexpr int kHistoryLimit = 1825;
constexpr int kMovePlatformPathUp = 1826;
constexpr int kMovePlatformPathDown = 1827;
constexpr int kOpenProfileFolder = 1828;
constexpr int kExportProfile = 1829;
constexpr int kImportProfile = 1830;
constexpr int kSettingsTabs = 1831;

enum class SettingsPage : int {
  interface_page,
  launch,
  data,
  profile,
};

struct State {
  HWND tabs{};
  std::array<std::vector<HWND>, 4> page_controls{};
  HWND simple_mode{};
  HWND show_tags{};
  HWND folders_first{};
  HWND platform_paths{};
  HWND recent_lists{};
  HWND recent_limit{};
  HWND open_last_list{};
  HWND restore_selection{};
  HWND confirm_destructive{};
  HWND confirm_secret{};
  HWND default_client{};
  HWND default_architecture{};
  HWND default_version{};
  HWND show_details{};
  HWND show_status{};
  HWND remember_history{};
  HWND tree_density{};
  HWND history_limit{};
  HFONT font{};
  HFONT button_font{};
  storage::Settings settings;
  std::filesystem::path profile_root;
  bool portable{false};
  CredentialsEditor edit_credentials;
  TagsEditor edit_tags;
  RecentBasesCleaner clear_recent_bases;
  ProfileAction export_profile;
  ProfileAction import_profile;
  bool reset_window_layout{false};
  std::optional<ApplicationSettingsResult> result;
};

LRESULT CALLBACK SettingsGroupProc(HWND group, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) {
    // Group frames are decorative sibling windows. Let mouse input pass through
    // to the check boxes, fields and buttons drawn inside their bounds.
    return HTTRANSPARENT;
  }
  if (message == WM_SETFONT) {
    SetWindowLongPtrW(group, GWLP_USERDATA, static_cast<LONG_PTR>(wparam));
    if (lparam) InvalidateRect(group, nullptr, TRUE);
    return 0;
  }
  if (message == WM_GETFONT) return GetWindowLongPtrW(group, GWLP_USERDATA);
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const HDC context = BeginPaint(group, &paint);
    RECT bounds{};
    GetClientRect(group, &bounds);
    FillRect(context, &bounds, GetSysColorBrush(COLOR_WINDOW));

    const int text_length = GetWindowTextLengthW(group);
    std::wstring title(static_cast<size_t>(std::max(text_length, 0)) + 1, L'\0');
    if (text_length > 0) GetWindowTextW(group, title.data(), text_length + 1);
    title.resize(static_cast<size_t>(std::max(text_length, 0)));
    const HFONT font = reinterpret_cast<HFONT>(GetWindowLongPtrW(group, GWLP_USERDATA));
    const HGDIOBJ previous_font = font ? SelectObject(context, font) : nullptr;
    SIZE title_size{};
    if (!title.empty()) {
      GetTextExtentPoint32W(context, title.c_str(), static_cast<int>(title.size()), &title_size);
    }
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, GetSysColor(COLOR_WINDOWTEXT));
    const HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DLIGHT));
    const HGDIOBJ previous_pen = SelectObject(context, pen);
    const int title_left = 8;
    const int title_gap = 4;
    const int border_y = std::max(10, static_cast<int>(title_size.cy / 2 + 2));
    MoveToEx(context, bounds.left, border_y, nullptr);
    LineTo(context, title_left - title_gap, border_y);
    MoveToEx(context, title_left + title_size.cx + title_gap, border_y, nullptr);
    LineTo(context, bounds.right - 1, border_y);
    MoveToEx(context, bounds.left, border_y, nullptr);
    LineTo(context, bounds.left, bounds.bottom - 1);
    LineTo(context, bounds.right - 1, bounds.bottom - 1);
    LineTo(context, bounds.right - 1, border_y);
    if (!title.empty()) TextOutW(context, title_left, 0, title.c_str(), static_cast<int>(title.size()));
    SelectObject(context, previous_pen);
    DeleteObject(pen);
    if (previous_font) SelectObject(context, previous_font);
    EndPaint(group, &paint);
    return 0;
  }
  return DefWindowProcW(group, message, wparam, lparam);
}

void ShowPage(State& state, int selected) {
  const int safe_selected = std::clamp(selected, 0, 3);
  for (int index = 0; index < static_cast<int>(state.page_controls.size()); ++index) {
    for (const HWND control : state.page_controls[static_cast<size_t>(index)]) {
      if (control) ShowWindow(control, index == safe_selected ? SW_SHOW : SW_HIDE);
    }
  }
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

int SelectedIndex(HWND list) {
  if (!list) return LB_ERR;
  return static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
}

std::wstring ReadControlText(HWND control) {
  if (!control) return {};
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(std::max(copied, 0)));
  return result;
}

int ClientTypeIndex(domain::ClientType value) {
  switch (value) {
    case domain::ClientType::thin: return 1;
    case domain::ClientType::thick: return 2;
    case domain::ClientType::web: return 3;
    default: return 0;
  }
}

void RefreshPlatformPaths(State& state) {
  SendMessageW(state.platform_paths, LB_RESETCONTENT, 0, 0);
  for (const auto& path : state.settings.platform_search_paths) {
    const auto text = path.wstring();
    SendMessageW(state.platform_paths, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }
  if (!state.settings.platform_search_paths.empty()) {
    SendMessageW(state.platform_paths, LB_SETCURSEL, 0, 0);
  }
}

void RefreshRecentLists(State& state) {
  SendMessageW(state.recent_lists, LB_RESETCONTENT, 0, 0);
  for (const auto& path : state.settings.recent_ibases) {
    const auto text = path.wstring();
    SendMessageW(state.recent_lists, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }
  if (!state.settings.recent_ibases.empty()) SendMessageW(state.recent_lists, LB_SETCURSEL, 0, 0);
}

HWND CreateControl(HWND parent, const wchar_t* class_name, std::wstring_view text, DWORD style,
    int x, int y, int width, int height, int id, HFONT font, UINT dpi, DWORD extended_style = 0) {
  const auto px = [dpi](int value) { return ScaleForDpi(value, dpi); };
  const HWND control = CreateWindowExW(extended_style, class_name, std::wstring(text).c_str(),
      WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height), parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SetControlFont(control, font);
  return control;
}

void AddPath(State& state, HWND window) {
  const auto selected = PickFolder(window, L"Дополнительный путь поиска платформы 1С");
  if (!selected || selected->empty()) return;
  const auto found = std::find_if(state.settings.platform_search_paths.begin(),
      state.settings.platform_search_paths.end(), [&](const auto& value) {
        return EqualNoCase(value.wstring(), selected->wstring());
      });
  if (found == state.settings.platform_search_paths.end()) {
    state.settings.platform_search_paths.push_back(*selected);
    RefreshPlatformPaths(state);
  }
}

void RemoveSelectedPath(State& state) {
  const int index = SelectedIndex(state.platform_paths);
  if (index == LB_ERR || static_cast<size_t>(index) >= state.settings.platform_search_paths.size()) return;
  state.settings.platform_search_paths.erase(state.settings.platform_search_paths.begin() + index);
  RefreshPlatformPaths(state);
}

void MoveSelectedPath(State& state, int offset) {
  const int index = SelectedIndex(state.platform_paths);
  if (index == LB_ERR) return;
  const int target = index + offset;
  if (target < 0 || static_cast<size_t>(target) >= state.settings.platform_search_paths.size()) return;
  std::swap(state.settings.platform_search_paths[static_cast<size_t>(index)],
      state.settings.platform_search_paths[static_cast<size_t>(target)]);
  RefreshPlatformPaths(state);
  SendMessageW(state.platform_paths, LB_SETCURSEL, target, 0);
}

void RemoveSelectedRecent(State& state) {
  const int index = SelectedIndex(state.recent_lists);
  if (index == LB_ERR || static_cast<size_t>(index) >= state.settings.recent_ibases.size()) return;
  state.settings.recent_ibases.erase(state.settings.recent_ibases.begin() + index);
  RefreshRecentLists(state);
}

void Collect(State& state) {
  state.settings.simple_mode = SendMessageW(state.simple_mode, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.show_tags_in_list = SendMessageW(state.show_tags, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.folders_first_when_sorting = SendMessageW(state.folders_first, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.open_last_list_on_startup = SendMessageW(state.open_last_list, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.restore_last_selection = SendMessageW(state.restore_selection, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.confirm_destructive_actions = SendMessageW(state.confirm_destructive, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.confirm_secret_launch = SendMessageW(state.confirm_secret, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.show_details_panel = SendMessageW(state.show_details, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.show_status_bar = SendMessageW(state.show_status, BM_GETCHECK, 0, 0) == BST_CHECKED;
  state.settings.remember_launch_history = SendMessageW(state.remember_history, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const LRESULT density = SendMessageW(state.tree_density, CB_GETCURSEL, 0, 0);
  state.settings.tree_density = density == CB_ERR ? 1 : std::clamp(static_cast<int>(density), 0, 2);
  const LRESULT history_limit = SendMessageW(state.history_limit, CB_GETCURSEL, 0, 0);
  state.settings.launch_history_limit = history_limit == CB_ERR ? 20 :
      std::clamp(static_cast<int>(history_limit == 0 ? 5 : history_limit == 1 ? 10 : 20), 1, 20);
  const LRESULT client = SendMessageW(state.default_client, CB_GETCURSEL, 0, 0);
  state.settings.default_client_type = client == 1 ? domain::ClientType::thin :
      client == 2 ? domain::ClientType::thick : client == 3 ? domain::ClientType::web : domain::ClientType::automatic;
  const LRESULT architecture = SendMessageW(state.default_architecture, CB_GETCURSEL, 0, 0);
  switch (architecture) {
    case 1: state.settings.default_architecture = domain::ClientArchitecture::x86; break;
    case 2: state.settings.default_architecture = domain::ClientArchitecture::x64; break;
    case 3: state.settings.default_architecture = domain::ClientArchitecture::x86_priority; break;
    case 4: state.settings.default_architecture = domain::ClientArchitecture::x64_priority; break;
    default: state.settings.default_architecture = domain::ClientArchitecture::automatic; break;
  }
  state.settings.default_platform_version = ReadControlText(state.default_version);
  const LRESULT limit = SendMessageW(state.recent_limit, CB_GETCURSEL, 0, 0);
  state.settings.recent_lists_limit = limit == CB_ERR ? 9 : std::clamp(static_cast<int>(limit) + 1, 1, 9);
  state.result = ApplicationSettingsResult{state.settings, state.reset_window_layout};
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
    const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
    if (notification && notification->hwndFrom == state->tabs && notification->code == TCN_SELCHANGE) {
      ShowPage(*state, static_cast<int>(TabCtrl_GetCurSel(state->tabs)));
      return 0;
    }
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command == kAddPlatformPath && HIWORD(wparam) == BN_CLICKED) {
      AddPath(*state, window);
      return 0;
    }
    if (command == kRemovePlatformPath && HIWORD(wparam) == BN_CLICKED) {
      RemoveSelectedPath(*state);
      return 0;
    }
    if (command == kMovePlatformPathUp && HIWORD(wparam) == BN_CLICKED) {
      MoveSelectedPath(*state, -1);
      return 0;
    }
    if (command == kMovePlatformPathDown && HIWORD(wparam) == BN_CLICKED) {
      MoveSelectedPath(*state, 1);
      return 0;
    }
    if (command == kOpenProfileFolder && HIWORD(wparam) == BN_CLICKED) {
      const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
          state->profile_root.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      if (result <= 32) MessageBoxW(window, L"Не удалось открыть папку профиля.", L"Профиль IBStart", MB_OK | MB_ICONWARNING);
      return 0;
    }
    if (command == kExportProfile && HIWORD(wparam) == BN_CLICKED && state->export_profile) {
      state->export_profile(window);
      return 0;
    }
    if (command == kImportProfile && HIWORD(wparam) == BN_CLICKED && state->import_profile) {
      if (state->import_profile(window)) DestroyWindow(window);
      return 0;
    }
    if (command == kRemoveRecentList && HIWORD(wparam) == BN_CLICKED) {
      RemoveSelectedRecent(*state);
      return 0;
    }
    if (command == kClearRecentLists && HIWORD(wparam) == BN_CLICKED) {
      state->settings.recent_ibases.clear();
      RefreshRecentLists(*state);
      return 0;
    }
    if (command == kClearRecentBases && HIWORD(wparam) == BN_CLICKED && state->clear_recent_bases) {
      state->clear_recent_bases(window);
      return 0;
    }
    if (command == kResetWindowLayout && HIWORD(wparam) == BN_CLICKED) {
      state->reset_window_layout = true;
      return 0;
    }
    if (command == kEditCredentials && HIWORD(wparam) == BN_CLICKED && state->edit_credentials) {
      if (const auto updated = state->edit_credentials(window, state->settings.credentials)) {
        state->settings.credentials = *updated;
      }
      return 0;
    }
    if (command == kEditTags && HIWORD(wparam) == BN_CLICKED && state->edit_tags) {
      state->edit_tags(window);
      return 0;
    }
    if (command == IDOK) {
      Collect(*state);
      DestroyWindow(window);
      return 0;
    }
    if (command == IDCANCEL) {
      DestroyWindow(window);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void CreateControls(HWND window, State& state) {
  const UINT dpi = GetDpiForWindow(window);
  const HFONT text_font = state.font;
  const HFONT button_font = state.button_font ? state.button_font : text_font;
  static ATOM group_atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kGroupClassName;
    klass.lpfnWndProc = SettingsGroupProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)group_atom;
  state.tabs = CreateControl(window, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS,
      10, 10, 700, 540, kSettingsTabs, text_font, dpi);
  const std::array<std::wstring_view, 4> tab_labels = {
      L"Интерфейс", L"Запуск", L"Данные", L"Профиль"};
  for (size_t index = 0; index < tab_labels.size(); ++index) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(tab_labels[index].data());
    TabCtrl_InsertItem(state.tabs, static_cast<int>(index), &item);
  }
  RECT page_rect{};
  GetClientRect(state.tabs, &page_rect);
  TabCtrl_AdjustRect(state.tabs, FALSE, &page_rect);
  POINT page_origin{page_rect.left, page_rect.top};
  MapWindowPoints(state.tabs, window, &page_origin, 1);
  const auto to_logical = [dpi](int value) {
    return MulDiv(value, 96, static_cast<int>(dpi == 0 ? 96 : dpi));
  };
  const int page_x = to_logical(page_origin.x);
  const int page_y = to_logical(page_origin.y);
  const auto page_control = [&](SettingsPage page, HWND control) {
    state.page_controls[static_cast<size_t>(page)].push_back(control);
    return control;
  };
  const auto create_page_control = [&](SettingsPage page, const wchar_t* class_name,
      std::wstring_view text, DWORD style, int x, int y, int width, int height, int id,
      HFONT font, DWORD extended_style = 0) {
    // Keep page controls as direct dialog children so IsDialogMessage can reach
    // them. The tab control is moved behind these siblings after creation.
    return page_control(page, CreateControl(window, class_name, text, style,
        page_x + x, page_y + y, width, height, id, font, dpi, extended_style));
  };
  const auto checkbox = [&](SettingsPage page, std::wstring_view text, int x, int y, int width,
                            int id, bool checked) {
    const HWND control = create_page_control(page, L"BUTTON", text,
        WS_TABSTOP | BS_AUTOCHECKBOX, x, y, width, 22, id, text_font);
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return control;
  };
  const auto button = [&](SettingsPage page, std::wstring_view text, int x, int y, int width, int id) {
    return create_page_control(page, L"BUTTON", text, WS_TABSTOP, x, y, width, 27, id,
        button_font);
  };
  const auto group = [&](SettingsPage page, std::wstring_view text, int x, int y, int width, int height) {
    return create_page_control(page, kGroupClassName, text, 0, x, y, width, height, 0,
        text_font);
  };

  group(SettingsPage::interface_page, L"Основные", 12, 12, 670, 104);
  state.simple_mode = checkbox(SettingsPage::interface_page, L"Использовать простой режим", 24, 36, 620,
      kSimpleMode, state.settings.simple_mode);
  state.show_tags = checkbox(SettingsPage::interface_page, L"Показывать теги в списке баз", 24, 62, 620,
      kShowTags, state.settings.show_tags_in_list);
  state.folders_first = checkbox(SettingsPage::interface_page, L"Показывать группы сверху при сортировке", 24, 88, 620,
      kFoldersFirst,
      state.settings.folders_first_when_sorting);

  group(SettingsPage::interface_page, L"Отображение", 12, 126, 670, 170);
  state.show_details = checkbox(SettingsPage::interface_page, L"Показывать карточку выбранной базы", 24, 150, 620,
      kShowDetails, state.settings.show_details_panel);
  state.show_status = checkbox(SettingsPage::interface_page, L"Показывать строку состояния", 24, 176, 620,
      kShowStatus, state.settings.show_status_bar);
  create_page_control(SettingsPage::interface_page, L"STATIC", L"Плотность дерева:", 0,
      24, 214, 220, 20, 0, text_font);
  state.tree_density = create_page_control(SettingsPage::interface_page, WC_COMBOBOXW, L"",
      WS_TABSTOP | CBS_DROPDOWNLIST, 260, 210, 250, 140, kTreeDensity, text_font);
  for (const auto label : {L"Компактная", L"Обычная", L"Увеличенная"})
    SendMessageW(state.tree_density, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SendMessageW(state.tree_density, CB_SETCURSEL, std::clamp(state.settings.tree_density, 0, 2), 0);

  group(SettingsPage::launch, L"Запуск при старте", 12, 12, 670, 96);
  state.open_last_list = checkbox(SettingsPage::launch, L"Открывать последний список баз при запуске", 24, 36, 620,
      kOpenLastList, state.settings.open_last_list_on_startup);
  state.restore_selection = checkbox(SettingsPage::launch, L"Восстанавливать последнюю выбранную базу", 24, 62, 620,
      kRestoreSelection, state.settings.restore_last_selection);

  group(SettingsPage::launch, L"Параметры по умолчанию", 12, 120, 670, 164);
  create_page_control(SettingsPage::launch, L"STATIC", L"Клиент по умолчанию:", 0,
      24, 146, 220, 20, 0, text_font);
  state.default_client = create_page_control(SettingsPage::launch, WC_COMBOBOXW, L"",
      WS_TABSTOP | CBS_DROPDOWNLIST, 260, 140, 380, 140, kDefaultClient, text_font);
  for (const auto label : {L"Авто", L"Тонкий клиент", L"Толстый клиент", L"Веб-клиент"})
    SendMessageW(state.default_client, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SendMessageW(state.default_client, CB_SETCURSEL, ClientTypeIndex(state.settings.default_client_type), 0);
  create_page_control(SettingsPage::launch, L"STATIC", L"Разрядность по умолчанию:", 0,
      24, 178, 220, 20, 0, text_font);
  state.default_architecture = create_page_control(SettingsPage::launch, WC_COMBOBOXW, L"",
      WS_TABSTOP | CBS_DROPDOWNLIST, 260, 172, 380, 140, kDefaultArchitecture, text_font);
  for (const auto label : {L"Авто", L"Только x86", L"Только x64", L"Приоритет x86", L"Приоритет x64"})
    SendMessageW(state.default_architecture, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SendMessageW(state.default_architecture, CB_SETCURSEL, static_cast<int>(state.settings.default_architecture), 0);
  create_page_control(SettingsPage::launch, L"STATIC", L"Версия платформы:", 0,
      24, 210, 220, 20, 0, text_font);
  state.default_version = create_page_control(SettingsPage::launch, L"EDIT",
      state.settings.default_platform_version, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      260, 204, 380, 22, kDefaultVersion, text_font, WS_EX_CLIENTEDGE);
  create_page_control(SettingsPage::launch, L"STATIC",
      L"Пустое значение — настройки базы или автоопределение.", 0,
      260, 232, 380, 20, 0, text_font);

  group(SettingsPage::launch, L"Подтверждения и история", 12, 300, 670, 184);
  state.confirm_destructive = checkbox(SettingsPage::launch, L"Подтверждать удаление и очистку данных", 24, 324, 620,
      kConfirmDestructive, state.settings.confirm_destructive_actions);
  state.confirm_secret = checkbox(SettingsPage::launch, L"Предупреждать перед запуском с секретами", 24, 350, 620,
      kConfirmSecret, state.settings.confirm_secret_launch);
  state.remember_history = checkbox(SettingsPage::launch, L"Сохранять историю запусков", 24, 376, 620,
      kRememberHistory, state.settings.remember_launch_history);
  create_page_control(SettingsPage::launch, L"STATIC", L"Показывать списков в меню:", 0,
      24, 416, 220, 20, 0, text_font);
  state.recent_limit = create_page_control(SettingsPage::launch, WC_COMBOBOXW, L"",
      WS_TABSTOP | CBS_DROPDOWNLIST, 260, 410, 120, 140, kRecentLimit, text_font);
  for (int count = 1; count <= 9; ++count) {
    const auto label = std::to_wstring(count);
    SendMessageW(state.recent_limit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  SendMessageW(state.recent_limit, CB_SETCURSEL,
      std::clamp(state.settings.recent_lists_limit, 1, 9) - 1, 0);
  create_page_control(SettingsPage::launch, L"STATIC", L"Хранить запусков:", 0,
      410, 416, 140, 20, 0, text_font);
  state.history_limit = create_page_control(SettingsPage::launch, WC_COMBOBOXW, L"",
      WS_TABSTOP | CBS_DROPDOWNLIST, 550, 410, 120, 140, kHistoryLimit, text_font);
  for (const auto label : {L"5", L"10", L"20"})
    SendMessageW(state.history_limit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  const int history_index = state.settings.launch_history_limit <= 5 ? 0 : state.settings.launch_history_limit <= 10 ? 1 : 2;
  SendMessageW(state.history_limit, CB_SETCURSEL, history_index, 0);

  group(SettingsPage::data, L"Учётные записи и теги", 12, 12, 670, 96);
  button(SettingsPage::data, L"Настроить учётные записи…", 24, 38, 240, kEditCredentials);
  create_page_control(SettingsPage::data, L"STATIC", L"Данные для подключения к информационным базам.",
      0, 278, 43, 370, 18, 0, text_font);
  button(SettingsPage::data, L"Настроить теги…", 24, 70, 240, kEditTags);
  create_page_control(SettingsPage::data, L"STATIC", L"Названия, цвета и наборы тегов для списка баз.",
      0, 278, 75, 370, 18, 0, text_font);

  group(SettingsPage::data, L"Дополнительные пути поиска платформ 1С", 12, 120, 670, 154);
  create_page_control(SettingsPage::data, L"STATIC",
      L"Эти каталоги добавляются к стандартному поиску платформ и реестра.",
      0, 24, 142, 620, 18, 0, text_font);
  state.platform_paths = create_page_control(SettingsPage::data, L"LISTBOX", L"",
      WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 24, 167, 450, 82,
      kPlatformPaths, text_font, WS_EX_CLIENTEDGE);
  button(SettingsPage::data, L"Добавить…", 500, 167, 160, kAddPlatformPath);
  button(SettingsPage::data, L"Удалить", 500, 200, 160, kRemovePlatformPath);
  button(SettingsPage::data, L"Вверх", 500, 233, 76, kMovePlatformPathUp);
  button(SettingsPage::data, L"Вниз", 584, 233, 76, kMovePlatformPathDown);
  RefreshPlatformPaths(state);

  group(SettingsPage::data, L"Недавно открытые списки баз", 12, 290, 670, 190);
  state.recent_lists = create_page_control(SettingsPage::data, L"LISTBOX", L"",
      WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 24, 320, 450, 100,
      kRecentLists, text_font, WS_EX_CLIENTEDGE);
  button(SettingsPage::data, L"Удалить выбранный", 500, 320, 160, kRemoveRecentList);
  button(SettingsPage::data, L"Очистить список", 500, 353, 160, kClearRecentLists);
  button(SettingsPage::data, L"Очистить историю запусков", 500, 386, 160, kClearRecentBases);
  RefreshRecentLists(state);

  group(SettingsPage::profile, L"Профиль", 12, 12, 670, 164);
  const std::wstring profile = L"Папка профиля: " + state.profile_root.wstring() +
      (state.portable ? L" (portable)" : L" (обычный режим)");
  create_page_control(SettingsPage::profile, L"STATIC", profile, 0,
      24, 38, 620, 20, 0, text_font);
  const std::wstring files = L"Файлы: settings.json и catalog-state.json";
  create_page_control(SettingsPage::profile, L"STATIC", files, 0,
      24, 66, 430, 20, 0, text_font);
  button(SettingsPage::profile, L"Открыть папку профиля", 24, 96, 195, kOpenProfileFolder);
  button(SettingsPage::profile, L"Экспортировать…", 230, 96, 160, kExportProfile);
  button(SettingsPage::profile, L"Импортировать…", 397, 96, 160, kImportProfile);
  button(SettingsPage::profile, L"Сбросить размер и положение", 24, 129, 226, kResetWindowLayout);

  CreateControl(window, L"BUTTON", L"ОК", WS_TABSTOP, 530, 565, 80, 27, IDOK, button_font, dpi);
  CreateControl(window, L"BUTTON", L"Отмена", WS_TABSTOP, 620, 565, 80, 27, IDCANCEL, button_font, dpi);
  SetWindowPos(state.tabs, HWND_BOTTOM, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ShowPage(state, 0);
}

}  // namespace

std::optional<ApplicationSettingsResult> EditApplicationSettings(
    HWND owner, storage::Settings initial, std::filesystem::path profile_root, bool portable,
    CredentialsEditor edit_credentials, TagsEditor edit_tags, RecentBasesCleaner clear_recent_bases,
    ProfileAction export_profile, ProfileAction import_profile) {
  State state;
  state.settings = std::move(initial);
  state.profile_root = std::move(profile_root);
  state.portable = portable;
  state.edit_credentials = std::move(edit_credentials);
  state.edit_tags = std::move(edit_tags);
  state.clear_recent_bases = std::move(clear_recent_bases);
  state.export_profile = std::move(export_profile);
  state.import_profile = std::move(import_profile);
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
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 720, 610, style, extended_style);
  HWND window = CreateWindowExW(extended_style, kClassName, L"Настройки IBStart", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr,
      GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  CreateControls(window, state);
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.simple_mode);
  MSG message{};
  int pump_result = 1;
  while (IsWindow(window) && (pump_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  CloseModalDialog(window, owner);
  if (pump_result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace ibstart::ui::dialog
