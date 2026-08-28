#include "ui/database_editor_dialog.hpp"

#include "ui/advanced_database_options_dialog.hpp"
#include "ui/dialog_support.hpp"

#include "core/catalog/catalog.hpp"
#include "core/connection/connection_string.hpp"

#include <CommCtrl.h>
#include <ShlObj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kDatabaseEditorClass[] = L"IBStart.DatabaseEditor";

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

bool IsUncPath(std::wstring_view path) {
  return path.size() >= 2 && ((path[0] == L'\\' && path[1] == L'\\') ||
      (path[0] == L'/' && path[1] == L'/'));
}

std::wstring ReadControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(length));
  return result;
}

DatabaseConnectionKind DetectConnectionKind(std::wstring_view connect) {
  if (catalog::Catalog::WebUrl(connect)) return DatabaseConnectionKind::web;
  if (!connection::ValueOrEmpty(connect, L"File").empty()) return DatabaseConnectionKind::file;
  return DatabaseConnectionKind::server;
}

enum DatabaseEditorControl : int {
  kDatabaseName = 1000,
  kConnectionFile,
  kConnectionWeb,
  kConnectionServer,
  kFilePath,
  kBrowseFilePath,
  kWebAddress,
  kServerCluster,
  kServerReference,
  kLaunchVersion,
  kLaunchApp,
  kLaunchWindowsAuth,
  kLaunchSpeed,
  kLaunchArchitecture,
  kOpenAdvancedDatabaseOptions
};

struct DatabaseEditorState {
  HWND name{};
  HWND file{};
  HWND file_browse{};
  HWND web{};
  HWND server{};
  HWND reference{};
  HWND file_radio{};
  HWND web_radio{};
  HWND server_radio{};
  HWND file_label{};
  HWND web_label{};
  HWND server_label{};
  HWND reference_label{};
  HWND version{};
  HWND architecture{};
  HWND app{};
  HWND windows_auth{};
  HWND speed{};
  HFONT font{};
  HFONT button_font{};
  DatabaseEditorData initial;
  std::optional<DatabaseEditorData> result;
  DatabaseConnectionKind kind{DatabaseConnectionKind::server};
  bool done{false};
};

void SetComboValue(HWND combo, std::wstring_view value) {
  const std::wstring text(value);
  LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
      reinterpret_cast<LPARAM>(text.c_str()));
  if (index == CB_ERR) index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  if (index != CB_ERR) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

std::wstring ApplicationLabel(std::wstring_view value) {
  if (value.empty() || EqualNoCase(value, L"Auto")) return L"Автоматически";
  if (EqualNoCase(value, L"ThickClient")) return L"Толстый клиент";
  if (EqualNoCase(value, L"ThinClient")) return L"Тонкий клиент";
  if (EqualNoCase(value, L"WebClient")) return L"Веб-клиент";
  return std::wstring(value);
}

std::wstring ApplicationValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Автоматически")) return L"Auto";
  if (EqualNoCase(label, L"Толстый клиент")) return L"ThickClient";
  if (EqualNoCase(label, L"Тонкий клиент")) return L"ThinClient";
  if (EqualNoCase(label, L"Веб-клиент")) return L"WebClient";
  return std::wstring(label);
}

std::wstring DefaultApplicationLabel(std::wstring_view value) {
  if (value.empty()) return L"Не задано";
  if (EqualNoCase(value, L"ThickClient")) return L"Толстый клиент";
  if (EqualNoCase(value, L"ThinClient")) return L"Тонкий клиент";
  return std::wstring(value);
}

std::wstring DefaultApplicationValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Не задано")) return {};
  if (EqualNoCase(label, L"Толстый клиент")) return L"ThickClient";
  if (EqualNoCase(label, L"Тонкий клиент")) return L"ThinClient";
  return std::wstring(label);
}

std::wstring ArchitectureLabel(std::wstring_view value) {
  if (value.empty()) return L"Автоматически";
  if (EqualNoCase(value, L"x86")) return L"Только 32 (x86)";
  if (EqualNoCase(value, L"x86_64")) return L"Только 64 (x86_64)";
  if (EqualNoCase(value, L"x86_prt")) return L"Приоритет 32 (x86_prt)";
  if (EqualNoCase(value, L"x86_64_prt")) return L"Приоритет 64 (x86_64_prt)";
  return std::wstring(value);
}

std::wstring ArchitectureValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Автоматически")) return {};
  if (EqualNoCase(label, L"Только 32 (x86)")) return L"x86";
  if (EqualNoCase(label, L"Только 64 (x86_64)")) return L"x86_64";
  if (EqualNoCase(label, L"Приоритет 32 (x86_prt)")) return L"x86_prt";
  if (EqualNoCase(label, L"Приоритет 64 (x86_64_prt)")) return L"x86_64_prt";
  return std::wstring(label);
}

std::wstring SpeedLabel(std::wstring_view value) {
  if (value.empty() || EqualNoCase(value, L"Normal")) return L"Обычная";
  if (EqualNoCase(value, L"Low")) return L"Низкая";
  return std::wstring(value);
}

std::wstring SpeedValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Обычная")) return L"Normal";
  if (EqualNoCase(label, L"Низкая")) return L"Low";
  return std::wstring(label);
}

bool IsEnabledFlag(std::wstring_view value) {
  return !value.empty() && !EqualNoCase(value, L"0") && !EqualNoCase(value, L"false") &&
      !EqualNoCase(value, L"no");
}

std::wstring FlagValue(HWND checkbox, std::wstring_view previous) {
  if (SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED) {
    return previous.empty() || !IsEnabledFlag(previous) ? L"1" : std::wstring(previous);
  }
  return L"0";
}

bool IsOptionalOrder(std::wstring_view value) {
  if (value.empty()) return true;
  return std::all_of(value.begin(), value.end(), [](wchar_t character) { return std::iswdigit(character) != 0; });
}

void UpdateConnectionControls(DatabaseEditorState& state) {
  const bool file = state.kind == DatabaseConnectionKind::file;
  const bool web = state.kind == DatabaseConnectionKind::web;
  const bool server = state.kind == DatabaseConnectionKind::server;
  CheckRadioButton(GetParent(state.name), kConnectionFile, kConnectionServer,
      file ? kConnectionFile : web ? kConnectionWeb : kConnectionServer);
  for (const HWND control : {state.file, state.file_browse, state.file_label}) EnableWindow(control, file);
  for (const HWND control : {state.web, state.web_label}) EnableWindow(control, web);
  for (const HWND control : {state.server, state.reference, state.server_label, state.reference_label}) {
    EnableWindow(control, server);
  }
}

void BrowseForFileBase(HWND window, DatabaseEditorState& state) {
  BROWSEINFOW info{};
  info.hwndOwner = window;
  info.lpszTitle = L"Выберите каталог файловой информационной базы";
  const PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
  if (!id) return;
  wchar_t path[MAX_PATH]{};
  const bool valid = SHGetPathFromIDListW(id, path);
  CoTaskMemFree(id);
  if (valid) SetWindowTextW(state.file, path);
}

std::optional<std::wstring> CollectDatabaseEditorResult(DatabaseEditorState& state) {
  DatabaseEditorData result = state.initial;
  result.kind = state.kind;
  result.name = TrimText(ReadControlText(state.name));
  if (result.name.empty()) return L"Укажите имя базы в списке.";
  const auto file = TrimText(ReadControlText(state.file));
  const auto web = TrimText(ReadControlText(state.web));
  const auto server = TrimText(ReadControlText(state.server));
  const auto reference = TrimText(ReadControlText(state.reference));
  if (state.kind == DatabaseConnectionKind::file && file.empty()) return L"Укажите путь к файловой базе.";
  if (state.kind == DatabaseConnectionKind::file) {
    const auto path_status = catalog::CheckFileDatabasePath(std::filesystem::path(file));
    if (path_status == catalog::FileDatabasePathStatus::missing) {
      return IsUncPath(file)
          ? L"UNC-каталог файловой базы не содержит файл 1Cv8.1CD. Сохранение заблокировано; проверьте сетевой путь и содержимое базы."
          : L"Каталог файловой базы должен содержать файл 1Cv8.1CD.";
    }
    if (path_status == catalog::FileDatabasePathStatus::inaccessible) {
      return IsUncPath(file)
          ? L"UNC-каталог файловой базы сейчас недоступен. Восстановите доступ к сетевому ресурсу и повторите сохранение; сохранение недоступной базы заблокировано."
          : L"Не удалось проверить каталог файловой базы. Убедитесь, что путь доступен и содержит файл 1Cv8.1CD.";
    }
  }
  if (state.kind == DatabaseConnectionKind::web && !connection::IsValidHttpUrl(web)) {
    return L"Укажите корректный URL http:// или https:// с непустым хостом, допустимым портом и без управляющих символов.";
  }
  if (state.kind == DatabaseConnectionKind::server && (server.empty() || reference.empty())) {
    return L"Укажите кластер серверов и имя информационной базы.";
  }
  try {
    const auto connection_kind = state.kind == DatabaseConnectionKind::file ? connection::ConnectionKind::file :
        state.kind == DatabaseConnectionKind::web ? connection::ConnectionKind::web : connection::ConnectionKind::server;
    result.connect = connection::BuildConnection(connection_kind, state.initial.connect, file, web, server, reference);
  } catch (const std::invalid_argument&) {
    return L"Строку подключения нельзя безопасно разобрать из-за незакрытой или неоднозначной кавычки.";
  }
  result.version = TrimText(ReadControlText(state.version));
  if (EqualNoCase(result.version, L"Авто")) result.version.clear();
  result.app = ApplicationValue(ReadControlText(state.app));
  result.wa = FlagValue(state.windows_auth, state.initial.wa);
  result.client_connection_speed = SpeedValue(ReadControlText(state.speed));
  result.app_arch = ArchitectureValue(ReadControlText(state.architecture));
  state.result = std::move(result);
  return std::nullopt;
}

void CreateDatabaseEditorControls(HWND window, DatabaseEditorState& state,
    const std::vector<domain::PlatformInstallation>& platforms) {
  const UINT dpi = GetDpiForWindow(window);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD extended_style, const wchar_t* class_name, std::wstring_view text, DWORD style,
                          int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(extended_style, class_name, std::wstring(text).c_str(),
        WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, font);
    return control;
  };
  const HFONT text_font = state.font;
  const HFONT button_font = state.button_font ? state.button_font : text_font;
  create(0, L"STATIC", L"Имя базы в списке:", 0, 18, 16, 260, 20, 0, text_font);
  state.name = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.name, WS_TABSTOP | ES_AUTOHSCROLL,
      18, 36, 624, 25, kDatabaseName, text_font);
  create(0, L"BUTTON", L"Расположение информационной базы", BS_GROUPBOX, 14, 74, 632, 260, 0, text_font);
  state.file_radio = create(0, L"BUTTON", L"Файловая база", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
      28, 98, 180, 20, kConnectionFile, text_font);
  state.file_label = create(0, L"STATIC", L"Каталог файловой базы:", 0, 48, 122, 190, 20, 0, text_font);
  state.file = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"File"),
      WS_TABSTOP | ES_AUTOHSCROLL, 48, 142, 498, 25, kFilePath, text_font);
  state.file_browse = create(0, L"BUTTON", L"Обзор…", WS_TABSTOP, 556, 142, 78, 25, kBrowseFilePath, button_font);
  state.web_radio = create(0, L"BUTTON", L"Веб-база", WS_TABSTOP | BS_AUTORADIOBUTTON,
      28, 178, 180, 20, kConnectionWeb, text_font);
  state.web_label = create(0, L"STATIC", L"Адрес веб-сервера:", 0, 48, 202, 190, 20, 0, text_font);
  const auto web = catalog::Catalog::WebUrl(state.initial.connect);
  state.web = create(WS_EX_CLIENTEDGE, L"EDIT", web ? *web : L"", WS_TABSTOP | ES_AUTOHSCROLL,
      48, 222, 586, 25, kWebAddress, text_font);
  state.server_radio = create(0, L"BUTTON", L"Серверная база 1С:Предприятия", WS_TABSTOP | BS_AUTORADIOBUTTON,
      28, 258, 270, 20, kConnectionServer, text_font);
  state.server_label = create(0, L"STATIC", L"Кластер серверов:", 0, 48, 282, 150, 20, 0, text_font);
  state.server = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"Srvr"),
      WS_TABSTOP | ES_AUTOHSCROLL, 205, 278, 429, 25, kServerCluster, text_font);
  state.reference_label = create(0, L"STATIC", L"Имя информационной базы:", 0, 48, 310, 170, 20, 0, text_font);
  state.reference = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"Ref"),
      WS_TABSTOP | ES_AUTOHSCROLL, 220, 306, 414, 25, kServerReference, text_font);

  create(0, L"BUTTON", L"Параметры запуска", BS_GROUPBOX, 14, 342, 632, 166, 0, text_font);
  create(0, L"STATIC", L"Версия платформы:", 0, 28, 366, 150, 20, 0, text_font);
  state.version = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
      160, 362, 175, 160, kLaunchVersion, text_font);
  SendMessageW(state.version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Авто"));
  for (const auto& platform : platforms) {
    if (SendMessageW(state.version, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(platform.version.c_str())) == CB_ERR) {
      SendMessageW(state.version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(platform.version.c_str()));
    }
  }
  SetComboValue(state.version, state.initial.version.empty() ? L"Авто" : state.initial.version);
  create(0, L"STATIC", L"Разрядность:", 0, 355, 366, 96, 20, 0, text_font);
  state.architecture = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      455, 362, 179, 180, kLaunchArchitecture, text_font);
  for (const auto* label : {L"Автоматически", L"Только 32 (x86)", L"Только 64 (x86_64)",
           L"Приоритет 32 (x86_prt)", L"Приоритет 64 (x86_64_prt)"}) {
    SendMessageW(state.architecture, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.architecture, ArchitectureLabel(state.initial.app_arch));
  create(0, L"STATIC", L"Режим клиента:", 0, 28, 400, 120, 20, 0, text_font);
  state.app = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      160, 396, 175, 160, kLaunchApp, text_font);
  for (const auto* label : {L"Автоматически", L"Толстый клиент", L"Тонкий клиент", L"Веб-клиент"}) {
    SendMessageW(state.app, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.app, ApplicationLabel(state.initial.app));
  create(0, L"STATIC", L"Скорость соединения:", 0, 355, 400, 110, 20, 0, text_font);
  state.speed = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      470, 396, 164, 100, kLaunchSpeed, text_font);
  for (const auto* label : {L"Обычная", L"Низкая"}) {
    SendMessageW(state.speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.speed, SpeedLabel(state.initial.client_connection_speed));
  state.windows_auth = create(0, L"BUTTON", L"Использовать аутентификацию ОС",
      WS_TABSTOP | BS_AUTOCHECKBOX, 28, 434, 290, 20, kLaunchWindowsAuth, text_font);
  SendMessageW(state.windows_auth, BM_SETCHECK,
      IsEnabledFlag(state.initial.wa) ? BST_CHECKED : BST_UNCHECKED, 0);
  create(0, L"BUTTON", L"Дополнительные настройки…", WS_TABSTOP,
      28, 468, 230, 28, kOpenAdvancedDatabaseOptions, button_font);
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON,
      430, 532, 110, 28, IDOK, button_font);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 532, 96, 28, IDCANCEL, button_font);
  state.kind = state.initial.kind;
  UpdateConnectionControls(state);
}

LRESULT CALLBACK DatabaseEditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<DatabaseEditorState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
    return DialogControlColor(message, wparam, lparam);
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command == kConnectionFile || command == kConnectionWeb || command == kConnectionServer) {
      state->kind = command == kConnectionFile ? DatabaseConnectionKind::file :
          command == kConnectionWeb ? DatabaseConnectionKind::web : DatabaseConnectionKind::server;
      UpdateConnectionControls(*state);
      return 0;
    }
    if (command == kBrowseFilePath) {
      BrowseForFileBase(window, *state);
      return 0;
    }
    if (command == kOpenAdvancedDatabaseOptions) {
      if (const auto updated = EditAdvancedDatabaseOptions(window, state->initial)) state->initial = *updated;
      return 0;
    }
    if (command == IDOK) {
      if (const auto error = CollectDatabaseEditorResult(*state)) {
        Message(window, *error, L"Проверка данных", MB_OK | MB_ICONWARNING);
        return 0;
      }
      state->done = true;
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

std::optional<DatabaseEditorData> EditDatabase(HWND owner, std::wstring_view title, DatabaseEditorData initial,
    const std::vector<domain::PlatformInstallation>& platforms) {
  DatabaseEditorState state;
  state.initial = std::move(initial);
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kDatabaseEditorClass;
    klass.lpfnWndProc = DatabaseEditorProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 660, 570, style, extended_style);
  HWND window = CreateWindowExW(extended_style, kDatabaseEditorClass, std::wstring(title).c_str(), style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  CreateDatabaseEditorControls(window, state, platforms);
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.name);
  MSG message{};
  int pump_result = 1;
  while (!state.done && (pump_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
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

DatabaseEditorData DatabaseEditorDataFromEntry(const domain::Entry& entry) {
  DatabaseEditorData result;
  result.name = entry.name;
  result.connect = entry.ValueOr(L"Connect");
  result.id = entry.ValueOr(L"ID");
  result.folder = entry.ValueOr(L"Folder");
  result.order_in_list = entry.ValueOr(L"OrderInList");
  result.order_in_tree = entry.ValueOr(L"OrderInTree");
  result.version = entry.ValueOr(L"Version");
  result.default_version = entry.ValueOr(L"DefaultVersion");
  result.app = entry.ValueOr(L"App");
  result.default_app = entry.ValueOr(L"DefaultApp");
  result.wa = entry.ValueOr(L"WA");
  result.external = entry.ValueOr(L"External");
  result.locale = entry.ValueOr(L"Locale");
  result.client_connection_speed = entry.ValueOr(L"ClientConnectionSpeed");
  result.app_arch = entry.ValueOr(L"AppArch");
  result.additional_parameters = entry.ValueOr(L"AdditionalParameters");
  result.kind = DetectConnectionKind(result.connect);
  return result;
}

void ApplyDatabaseEditorData(domain::Entry& entry, const DatabaseEditorData& data) {
  entry.Set(L"Connect", data.connect);
  entry.Set(L"OrderInList", data.order_in_list);
  entry.Set(L"OrderInTree", data.order_in_tree);
  entry.Set(L"Version", data.version);
  entry.Set(L"DefaultVersion", data.default_version);
  entry.Set(L"App", data.app);
  entry.Set(L"DefaultApp", data.default_app);
  entry.Set(L"WA", data.wa);
  entry.Set(L"External", data.external);
  entry.Set(L"Locale", data.locale);
  entry.Set(L"ClientConnectionSpeed", data.client_connection_speed);
  entry.Set(L"AppArch", data.app_arch);
  entry.Set(L"AdditionalParameters", data.additional_parameters);
}

}  // namespace ibstart::ui::dialog
