#include "ui/main_window.hpp"

#include "app/resource.h"
#include "core/cache/cache_service.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/shell/shortcut.hpp"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ibstart::ui {
namespace {
constexpr wchar_t kClassName[] = L"IBStart.MainWindow";
constexpr wchar_t kInputBoxClass[] = L"IBStart.InputBox";
constexpr wchar_t kDatabaseEditorClass[] = L"IBStart.DatabaseEditor";
constexpr wchar_t kAdvancedDatabaseOptionsClass[] = L"IBStart.AdvancedDatabaseOptions";
constexpr UINT kActivateMessage = WM_APP + 23;
constexpr ULONG_PTR kLaunchCopyData = 0x49425354;
constexpr int kMinimumWindowWidth = 940;
constexpr int kMinimumWindowHeight = 460;
enum Command : int { kEnterprise = 100, kDesigner, kEdit, kCache, kShortcut, kDelete, kAddFile, kAddServer, kAddGroup, kOpenList, kRefresh, kSimpleMode, kToggleFavorite, kFocusSearch, kAbout, kMoveUp, kMoveDown, kFavorite1 = 200 };
enum TreeImage : int { kFileDatabaseImage, kServerDatabaseImage, kFolderImage, kFavoriteImage, kRecentImage };

void Message(HWND owner, std::wstring_view text, std::wstring_view title = L"ИБ Старт", UINT type = MB_OK | MB_ICONINFORMATION) { MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type); }
HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}
HFONT CreateUiFont(HWND window, int points, LONG weight) {
  return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
int ScaleForDpi(int logicalPixels, UINT dpi) { return MulDiv(logicalPixels, static_cast<int>(dpi == 0 ? 96 : dpi), 96); }
SIZE DialogOuterSize(HWND owner, int clientWidth, int clientHeight, DWORD style, DWORD extendedStyle) {
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  RECT bounds{0, 0, ScaleForDpi(clientWidth, dpi), ScaleForDpi(clientHeight, dpi)};
  if (!AdjustWindowRectExForDpi(&bounds, style, FALSE, extendedStyle, dpi)) return {bounds.right, bounds.bottom};
  return {bounds.right - bounds.left, bounds.bottom - bounds.top};
}
void AttachButtonIcon(HWND button, HINSTANCE instance, int resource, std::vector<HIMAGELIST>& storage) {
  HIMAGELIST images = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 1, 1);
  HICON icon = LoadResourceIcon(instance, resource, 20);
  if (!images || !icon || ImageList_AddIcon(images, icon) < 0) {
    if (icon) DestroyIcon(icon);
    if (images) ImageList_Destroy(images);
    return;
  }
  DestroyIcon(icon);
  BUTTON_IMAGELIST layout{}; layout.himl = images; layout.margin = {7, 0, 6, 0}; layout.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
  if (!SendMessageW(button, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&layout))) { ImageList_Destroy(images); return; }
  storage.push_back(images);
}
void AddButtonTooltip(HWND tooltip, HWND button, const wchar_t* text) {
  if (!tooltip || !button || !text) return;
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  info.hwnd = GetParent(button);
  info.uId = reinterpret_cast<UINT_PTR>(button);
  info.lpszText = const_cast<wchar_t*>(text);
  SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}
int ButtonIdealWidth(HWND button, int fallback) {
  SIZE size{};
  if (!button || !SendMessageW(button, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&size)) || size.cx <= 0) return fallback;
  return std::max(fallback, static_cast<int>(size.cx) + 8);
}
std::wstring FriendlyFieldName(std::wstring_view key) {
  struct Label { std::wstring_view key; std::wstring_view text; };
  constexpr Label labels[] = {{L"Connect", L"Подключение"}, {L"ID", L"Идентификатор"}, {L"Folder", L"Группа"},
      {L"OrderInList", L"Порядок"}, {L"Version", L"Версия платформы"}, {L"App", L"Приложение"},
      {L"DefaultApp", L"Приложение по умолчанию"}, {L"WA", L"Аутентификация ОС"}, {L"External", L"Внешняя"},
      {L"Locale", L"Локаль"}, {L"ClientConnectionSpeed", L"Скорость соединения"}, {L"AppArch", L"Разрядность"}, {L"AdditionalParameters", L"Доп. параметры"}};
  for (const auto& label : labels) if (CompareStringOrdinal(key.data(), static_cast<int>(key.size()), label.key.data(), static_cast<int>(label.key.size()), TRUE) == CSTR_EQUAL) return std::wstring(label.text);
  return std::wstring(key);
}
std::wstring ConnectionKind(std::wstring_view connect) {
  if (catalog::Catalog::IsWebConnection(connect)) return L"Веб-база";
  if (utf::FindNoCaseOrdinal(connect, L"File=") != std::wstring_view::npos) return L"Файловая информационная база";
  if (utf::FindNoCaseOrdinal(connect, L"Srvr=") != std::wstring_view::npos) return L"Серверная информационная база";
  return L"Информационная база";
}
int DatabaseTreeImage(const domain::Entry* entry) {
  if (entry && utf::FindNoCaseOrdinal(entry->ValueOr(L"Connect"), L"File=") != std::wstring_view::npos) return kFileDatabaseImage;
  return kServerDatabaseImage;
}
std::wstring SingleLine(std::wstring value) {
  std::replace(value.begin(), value.end(), L'\r', L' '); std::replace(value.begin(), value.end(), L'\n', L' '); std::replace(value.begin(), value.end(), L'\t', L' '); return value;
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
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
void SetControlFont(HWND control, HFONT font) {
  if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}
void PositionDialogNearOwner(HWND dialog, HWND owner) {
  RECT dialogRect{};
  if (!GetWindowRect(dialog, &dialogRect)) return;
  const int width = dialogRect.right - dialogRect.left;
  const int height = dialogRect.bottom - dialogRect.top;
  RECT ownerRect{};
  if (!owner || !GetWindowRect(owner, &ownerRect)) ownerRect = dialogRect;
  MONITORINFO monitor{sizeof(monitor)};
  const HMONITOR selected = MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(selected, &monitor)) return;
  const RECT work = monitor.rcWork;
  int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
  int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
  const int minimumX = static_cast<int>(work.left);
  const int minimumY = static_cast<int>(work.top);
  const int maximumX = std::max(minimumX, static_cast<int>(work.right) - width);
  const int maximumY = std::max(minimumY, static_cast<int>(work.bottom) - height);
  x = std::clamp(x, minimumX, maximumX);
  y = std::clamp(y, minimumY, maximumY);
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

enum class DatabaseConnectionKind { file, web, server };
struct DatabaseEditorData {
  std::wstring name;
  std::wstring connect;
  std::wstring id;
  std::wstring folder;
  std::wstring order_in_list;
  std::wstring order_in_tree;
  std::wstring version;
  std::wstring default_version;
  std::wstring app;
  std::wstring default_app;
  std::wstring wa;
  std::wstring external;
  std::wstring locale;
  std::wstring client_connection_speed;
  std::wstring app_arch;
  std::wstring additional_parameters;
  DatabaseConnectionKind kind{DatabaseConnectionKind::server};
};

std::vector<std::wstring> SplitConnection(std::wstring_view connect) {
  std::vector<std::wstring> result;
  size_t begin = 0;
  bool quoted = false;
  for (size_t index = 0; index <= connect.size(); ++index) {
    const wchar_t character = index < connect.size() ? connect[index] : L';';
    if (character == L'"') quoted = !quoted;
    if (character != L';' || quoted) continue;
    const auto part = TrimText(connect.substr(begin, index - begin));
    if (!part.empty()) result.push_back(part);
    begin = index + 1;
  }
  return result;
}
std::wstring UnquoteConnectionValue(std::wstring value) {
  value = TrimText(value);
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
  return value;
}
std::wstring ConnectionValue(std::wstring_view connect, std::wstring_view key) {
  for (const auto& part : SplitConnection(connect)) {
    const size_t separator = part.find(L'=');
    if (separator != std::wstring::npos && EqualNoCase(TrimText(std::wstring_view(part).substr(0, separator)), key)) {
      return UnquoteConnectionValue(part.substr(separator + 1));
    }
  }
  return {};
}
DatabaseConnectionKind DetectConnectionKind(std::wstring_view connect) {
  if (catalog::Catalog::WebUrl(connect)) return DatabaseConnectionKind::web;
  if (!ConnectionValue(connect, L"File").empty()) return DatabaseConnectionKind::file;
  return DatabaseConnectionKind::server;
}
std::wstring QuoteConnectionValue(std::wstring value) {
  std::replace(value.begin(), value.end(), L'"', L'\'');
  return L"\"" + value + L"\"";
}
std::wstring BuildConnection(DatabaseConnectionKind kind, std::wstring_view original, std::wstring_view file,
    std::wstring_view web, std::wstring_view server, std::wstring_view reference) {
  std::wstring result;
  const auto append = [&result](std::wstring_view value) {
    if (value.empty()) return;
    if (!result.empty()) result.push_back(L';');
    result += value;
  };
  if (kind == DatabaseConnectionKind::file) append(L"File=" + QuoteConnectionValue(std::wstring(file)));
  else if (kind == DatabaseConnectionKind::web) append(L"WS=" + QuoteConnectionValue(std::wstring(web)));
  else {
    append(L"Srvr=" + QuoteConnectionValue(std::wstring(server)));
    append(L"Ref=" + QuoteConnectionValue(std::wstring(reference)));
  }
  // A legacy direct URL (https://host/base) becomes WS="…" above.  It has no
  // key, so treating it as an unknown fragment would append the original URL
  // after the new WS field and produce an invalid Connect value.
  if (kind == DatabaseConnectionKind::web && catalog::IsBareWebConnection(original)) return result;
  for (const auto& part : SplitConnection(original)) {
    const size_t separator = part.find(L'=');
    const auto key = separator == std::wstring::npos ? std::wstring_view{} : std::wstring_view(part).substr(0, separator);
    if (EqualNoCase(TrimText(key), L"File") || EqualNoCase(TrimText(key), L"WS") ||
        EqualNoCase(TrimText(key), L"Srvr") || EqualNoCase(TrimText(key), L"Ref")) continue;
    append(part);
  }
  return result;
}

enum DatabaseEditorControl : int {
  kDatabaseName = 1000, kConnectionFile, kConnectionWeb, kConnectionServer, kFilePath, kBrowseFilePath,
  kWebAddress, kServerCluster, kServerReference, kLaunchVersion, kLaunchApp, kLaunchWindowsAuth,
  kLaunchSpeed, kLaunchArchitecture, kOpenAdvancedDatabaseOptions
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
  LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
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
domain::ClientType ClientTypeFromApplication(std::wstring_view value) {
  if (EqualNoCase(value, L"ThinClient")) return domain::ClientType::thin;
  if (EqualNoCase(value, L"ThickClient")) return domain::ClientType::thick;
  if (EqualNoCase(value, L"WebClient")) return domain::ClientType::web;
  return domain::ClientType::automatic;
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
  return !value.empty() && !EqualNoCase(value, L"0") && !EqualNoCase(value, L"false") && !EqualNoCase(value, L"no");
}
std::wstring FlagValue(HWND checkbox, std::wstring_view previous) {
  if (SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED) return previous.empty() || !IsEnabledFlag(previous) ? L"1" : std::wstring(previous);
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
  for (const HWND control : {state.server, state.reference, state.server_label, state.reference_label}) EnableWindow(control, server);
}
void BrowseForFileBase(HWND dialog, DatabaseEditorState& state) {
  BROWSEINFOW info{};
  info.hwndOwner = dialog;
  info.lpszTitle = L"Выберите каталог файловой информационной базы";
  const PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
  if (!id) return;
  wchar_t path[MAX_PATH]{};
  const bool valid = SHGetPathFromIDListW(id, path);
  CoTaskMemFree(id);
  if (valid) SetWindowTextW(state.file, path);
}

enum AdvancedDatabaseOptionsControl : int {
  kAdvancedDefaultApp = 1400, kAdvancedDefaultVersion, kAdvancedOrderInList, kAdvancedOrderInTree,
  kAdvancedExternal, kAdvancedLocale, kAdvancedParameters,
  kAdvancedHelpDefaultApp, kAdvancedHelpDefaultVersion, kAdvancedHelpId, kAdvancedHelpFolder,
  kAdvancedHelpOrderInList, kAdvancedHelpOrderInTree, kAdvancedHelpExternal, kAdvancedHelpLocale,
  kAdvancedHelpParameters
};

struct AdvancedDatabaseOptionsState {
  HWND default_app{};
  HWND default_version{};
  HWND order_in_list{};
  HWND order_in_tree{};
  HWND external{};
  HWND locale{};
  HWND parameters{};
  HFONT font{};
  HFONT button_font{};
  DatabaseEditorData initial;
  std::optional<DatabaseEditorData> result;
  bool done{false};
};

std::wstring FolderLabel(std::wstring_view folder) {
  return folder.empty() || folder == L"/" ? L"Корневой уровень" : std::wstring(folder);
}
void ShowAdvancedParameterHelp(HWND owner, int command) {
  std::wstring title;
  std::wstring text;
  switch (command) {
    case kAdvancedHelpDefaultApp:
      title = L"Приложение при автоопределении (DefaultApp)";
      text = L"Используется стандартным стартером 1С, когда «Режим клиента» имеет значение «Автоматически». "
             L"Допустимы тонкий или толстый клиент. Не задавайте это поле, если предпочтение не требуется.";
      break;
    case kAdvancedHelpDefaultVersion:
      title = L"Версия по умолчанию (DefaultVersion)";
      text = L"Версия платформы, фактически выбранная стартером 1С при автоматическом выборе. "
             L"IBStart использует её, если основная «Версия платформы» не задана.";
      break;
    case kAdvancedHelpId:
      title = L"Идентификатор базы (ID)";
      text = L"Уникальный идентификатор записи списка баз. Он создаётся при добавлении базы и показан только для справки. "
             L"Повторяющийся ID может привести к объединению разных записей стандартным стартером.";
      break;
    case kAdvancedHelpFolder:
      title = L"Группа списка (Folder)";
      text = L"Путь к группе в дереве списка баз. В IBStart он меняется перетаскиванием базы или команды перемещения, "
             L"поэтому здесь показан только для справки.";
      break;
    case kAdvancedHelpOrderInList:
      title = L"Порядок в списке (OrderInList)";
      text = L"Числовой порядок записи для плоского списка баз. При перемещении базы IBStart автоматически перенумеровывает записи.";
      break;
    case kAdvancedHelpOrderInTree:
      title = L"Порядок в дереве (OrderInTree)";
      text = L"Числовой порядок базы внутри её группы. IBStart учитывает его при построении дерева и обновляет при перемещении базы.";
      break;
    case kAdvancedHelpExternal:
      title = L"Внешняя запись (External)";
      text = L"Отмечает запись, полученную из подключаемого общего списка баз. Обычно это значение задаётся источником общего списка.";
      break;
    case kAdvancedHelpLocale:
      title = L"Локаль (Locale)";
      text = L"Локаль запуска 1С, например ru_RU. Оставьте пустым, чтобы применялись обычные настройки платформы.";
      break;
    case kAdvancedHelpParameters:
      title = L"Дополнительные параметры запуска (AdditionalParameters)";
      text = L"Необязательные ключи командной строки 1С, передаваемые при запуске выбранной базы. "
             L"Например: /AppArch x86_64_prt. Параметры с паролями хранить здесь небезопасно.";
      break;
    default: return;
  }
  Message(owner, text, title, MB_OK | MB_ICONINFORMATION);
}
void CreateAdvancedDatabaseOptionsControls(HWND dialog, AdvancedDatabaseOptionsState& state) {
  const UINT dpi = GetDpiForWindow(dialog);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD exStyle, const wchar_t* className, std::wstring_view text, DWORD style, int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(exStyle, className, std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | style,
        px(x), px(y), px(width), px(height), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, font);
    return control;
  };
  const HFONT textFont = state.font;
  const HFONT buttonFont = state.button_font ? state.button_font : textFont;
  const auto help = [&](int y, int command) { create(0, L"BUTTON", L"?", WS_TABSTOP, 582, y, 48, 25, command, buttonFont); };

  create(0, L"BUTTON", L"Автоматический выбор запуска", BS_GROUPBOX, 14, 14, 632, 102, 0, textFont);
  create(0, L"STATIC", L"Клиент при автоопределении:", 0, 28, 42, 190, 20, 0, textFont);
  state.default_app = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 224, 38, 348, 120, kAdvancedDefaultApp, textFont);
  for (const auto* label : {L"Не задано", L"Тонкий клиент", L"Толстый клиент"}) SendMessageW(state.default_app, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SetComboValue(state.default_app, DefaultApplicationLabel(state.initial.default_app));
  help(38, kAdvancedHelpDefaultApp);
  create(0, L"STATIC", L"Версия по умолчанию:", 0, 28, 76, 190, 20, 0, textFont);
  state.default_version = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.default_version, WS_TABSTOP | ES_AUTOHSCROLL, 224, 72, 348, 25, kAdvancedDefaultVersion, textFont);
  help(72, kAdvancedHelpDefaultVersion);

  create(0, L"BUTTON", L"Реквизиты списка", BS_GROUPBOX, 14, 128, 632, 172, 0, textFont);
  create(0, L"STATIC", L"Идентификатор базы:", 0, 28, 156, 156, 20, 0, textFont);
  create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.id, ES_READONLY | ES_AUTOHSCROLL, 192, 152, 380, 25, 0, textFont);
  help(152, kAdvancedHelpId);
  create(0, L"STATIC", L"Группа списка:", 0, 28, 190, 156, 20, 0, textFont);
  create(WS_EX_CLIENTEDGE, L"EDIT", FolderLabel(state.initial.folder), ES_READONLY | ES_AUTOHSCROLL, 192, 186, 380, 25, 0, textFont);
  help(186, kAdvancedHelpFolder);
  create(0, L"STATIC", L"Порядок в списке:", 0, 28, 224, 156, 20, 0, textFont);
  state.order_in_list = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.order_in_list, WS_TABSTOP | ES_AUTOHSCROLL, 192, 220, 380, 25, kAdvancedOrderInList, textFont);
  help(220, kAdvancedHelpOrderInList);
  create(0, L"STATIC", L"Порядок в дереве:", 0, 28, 258, 156, 20, 0, textFont);
  state.order_in_tree = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.order_in_tree, WS_TABSTOP | ES_AUTOHSCROLL, 192, 254, 380, 25, kAdvancedOrderInTree, textFont);
  help(254, kAdvancedHelpOrderInTree);

  create(0, L"BUTTON", L"Другие параметры", BS_GROUPBOX, 14, 312, 632, 130, 0, textFont);
  state.external = create(0, L"BUTTON", L"Внешняя информационная база", WS_TABSTOP | BS_AUTOCHECKBOX, 28, 338, 300, 20, kAdvancedExternal, textFont);
  SendMessageW(state.external, BM_SETCHECK, IsEnabledFlag(state.initial.external) ? BST_CHECKED : BST_UNCHECKED, 0);
  help(334, kAdvancedHelpExternal);
  create(0, L"STATIC", L"Локаль:", 0, 28, 370, 156, 20, 0, textFont);
  state.locale = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.locale, WS_TABSTOP | ES_AUTOHSCROLL, 192, 366, 380, 25, kAdvancedLocale, textFont);
  help(366, kAdvancedHelpLocale);
  create(0, L"STATIC", L"Параметры командной строки:", 0, 28, 404, 164, 20, 0, textFont);
  state.parameters = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.additional_parameters, WS_TABSTOP | ES_AUTOHSCROLL, 192, 400, 380, 25, kAdvancedParameters, textFont);
  help(400, kAdvancedHelpParameters);
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, 450, 462, 90, 28, IDOK, buttonFont);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 462, 84, 28, IDCANCEL, buttonFont);
}
std::optional<std::wstring> CollectAdvancedDatabaseOptions(AdvancedDatabaseOptionsState& state) {
  DatabaseEditorData result = state.initial;
  result.default_app = DefaultApplicationValue(ReadControlText(state.default_app));
  result.default_version = TrimText(ReadControlText(state.default_version));
  result.order_in_list = TrimText(ReadControlText(state.order_in_list));
  result.order_in_tree = TrimText(ReadControlText(state.order_in_tree));
  if (!IsOptionalOrder(result.order_in_list) || !IsOptionalOrder(result.order_in_tree)) {
    return L"Порядок в списке и порядок в дереве должны быть целыми неотрицательными числами или пустыми.";
  }
  result.external = FlagValue(state.external, state.initial.external);
  result.locale = TrimText(ReadControlText(state.locale));
  result.additional_parameters = ReadControlText(state.parameters);
  state.result = std::move(result);
  return std::nullopt;
}
LRESULT CALLBACK AdvancedDatabaseOptionsProc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AdvancedDatabaseOptionsState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command >= kAdvancedHelpDefaultApp && command <= kAdvancedHelpParameters) { ShowAdvancedParameterHelp(wnd, command); return 0; }
    if (command == IDOK) {
      if (const auto error = CollectAdvancedDatabaseOptions(*state)) { Message(wnd, *error, L"Проверка данных", MB_OK | MB_ICONWARNING); return 0; }
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
    if (command == IDCANCEL) { state->done = true; DestroyWindow(wnd); return 0; }
  }
  if (message == WM_CLOSE && state) { state->done = true; DestroyWindow(wnd); return 0; }
  return DefWindowProcW(wnd, message, wparam, lparam);
}
std::optional<DatabaseEditorData> EditAdvancedDatabaseOptions(HWND owner, DatabaseEditorData initial) {
  AdvancedDatabaseOptionsState state;
  state.initial = std::move(initial);
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kAdvancedDatabaseOptionsClass;
    klass.lpfnWndProc = AdvancedDatabaseOptionsProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 660, 510, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kAdvancedDatabaseOptionsClass, L"Дополнительные настройки базы", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    if (owner) EnableWindow(owner, TRUE);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_SEMIBOLD);
  CreateAdvancedDatabaseOptionsControls(dialog, state);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.default_app);
  MSG message{};
  int pumpResult = 1;
  while (!state.done && (pumpResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (pumpResult == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
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
  if (state.kind == DatabaseConnectionKind::web && !catalog::Catalog::WebUrl(web)) return L"Укажите корректный адрес веб-сервера, начинающийся с http:// или https://.";
  if (state.kind == DatabaseConnectionKind::server && (server.empty() || reference.empty())) return L"Укажите кластер серверов и имя информационной базы.";
  result.connect = BuildConnection(state.kind, state.initial.connect, file, web, server, reference);
  result.version = TrimText(ReadControlText(state.version));
  if (EqualNoCase(result.version, L"Авто")) result.version.clear();
  result.app = ApplicationValue(ReadControlText(state.app));
  result.wa = FlagValue(state.windows_auth, state.initial.wa);
  result.client_connection_speed = SpeedValue(ReadControlText(state.speed));
  result.app_arch = ArchitectureValue(ReadControlText(state.architecture));
  state.result = std::move(result);
  return std::nullopt;
}
void CreateDatabaseEditorControls(HWND dialog, DatabaseEditorState& state, const std::vector<domain::PlatformInstallation>& platforms) {
  const UINT dpi = GetDpiForWindow(dialog);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD exStyle, const wchar_t* className, std::wstring_view text, DWORD style, int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(exStyle, className, std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | style,
        px(x), px(y), px(width), px(height), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, font);
    return control;
  };
  const HFONT textFont = state.font;
  const HFONT buttonFont = state.button_font ? state.button_font : textFont;
  create(0, L"STATIC", L"Имя базы в списке:", 0, 18, 16, 260, 20, 0, textFont);
  state.name = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.name, WS_TABSTOP | ES_AUTOHSCROLL, 18, 36, 624, 25, kDatabaseName, textFont);
  create(0, L"BUTTON", L"Расположение информационной базы", BS_GROUPBOX, 14, 74, 632, 260, 0, textFont);
  state.file_radio = create(0, L"BUTTON", L"Файловая база", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 28, 98, 180, 20, kConnectionFile, textFont);
  state.file_label = create(0, L"STATIC", L"Каталог файловой базы:", 0, 48, 122, 190, 20, 0, textFont);
  state.file = create(WS_EX_CLIENTEDGE, L"EDIT", ConnectionValue(state.initial.connect, L"File"), WS_TABSTOP | ES_AUTOHSCROLL, 48, 142, 506, 25, kFilePath, textFont);
  state.file_browse = create(0, L"BUTTON", L"Обзор…", WS_TABSTOP, 564, 142, 70, 25, kBrowseFilePath, buttonFont);
  state.web_radio = create(0, L"BUTTON", L"Веб-база", WS_TABSTOP | BS_AUTORADIOBUTTON, 28, 178, 180, 20, kConnectionWeb, textFont);
  state.web_label = create(0, L"STATIC", L"Адрес веб-сервера:", 0, 48, 202, 190, 20, 0, textFont);
  const auto web = catalog::Catalog::WebUrl(state.initial.connect);
  state.web = create(WS_EX_CLIENTEDGE, L"EDIT", web ? *web : L"", WS_TABSTOP | ES_AUTOHSCROLL, 48, 222, 586, 25, kWebAddress, textFont);
  state.server_radio = create(0, L"BUTTON", L"Серверная база 1С:Предприятия", WS_TABSTOP | BS_AUTORADIOBUTTON, 28, 258, 270, 20, kConnectionServer, textFont);
  state.server_label = create(0, L"STATIC", L"Кластер серверов:", 0, 48, 282, 150, 20, 0, textFont);
  state.server = create(WS_EX_CLIENTEDGE, L"EDIT", ConnectionValue(state.initial.connect, L"Srvr"), WS_TABSTOP | ES_AUTOHSCROLL, 205, 278, 429, 25, kServerCluster, textFont);
  state.reference_label = create(0, L"STATIC", L"Имя информационной базы:", 0, 48, 310, 170, 20, 0, textFont);
  state.reference = create(WS_EX_CLIENTEDGE, L"EDIT", ConnectionValue(state.initial.connect, L"Ref"), WS_TABSTOP | ES_AUTOHSCROLL, 220, 306, 414, 25, kServerReference, textFont);

  create(0, L"BUTTON", L"Параметры запуска", BS_GROUPBOX, 14, 342, 632, 166, 0, textFont);
  create(0, L"STATIC", L"Версия платформы:", 0, 28, 366, 150, 20, 0, textFont);
  state.version = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL, 160, 362, 175, 160, kLaunchVersion, textFont);
  SendMessageW(state.version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Авто"));
  for (const auto& platform : platforms) {
    if (SendMessageW(state.version, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(platform.version.c_str())) == CB_ERR) {
      SendMessageW(state.version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(platform.version.c_str()));
    }
  }
  SetComboValue(state.version, state.initial.version.empty() ? L"Авто" : state.initial.version);
  create(0, L"STATIC", L"Разрядность:", 0, 355, 366, 96, 20, 0, textFont);
  state.architecture = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 455, 362, 179, 180, kLaunchArchitecture, textFont);
  for (const auto* label : {L"Автоматически", L"Только 32 (x86)", L"Только 64 (x86_64)", L"Приоритет 32 (x86_prt)", L"Приоритет 64 (x86_64_prt)"}) {
    SendMessageW(state.architecture, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.architecture, ArchitectureLabel(state.initial.app_arch));
  create(0, L"STATIC", L"Режим клиента:", 0, 28, 400, 120, 20, 0, textFont);
  state.app = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 160, 396, 175, 160, kLaunchApp, textFont);
  for (const auto* label : {L"Автоматически", L"Толстый клиент", L"Тонкий клиент", L"Веб-клиент"}) SendMessageW(state.app, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SetComboValue(state.app, ApplicationLabel(state.initial.app));
  create(0, L"STATIC", L"Скорость соединения:", 0, 355, 400, 110, 20, 0, textFont);
  state.speed = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 470, 396, 164, 100, kLaunchSpeed, textFont);
  for (const auto* label : {L"Обычная", L"Низкая"}) SendMessageW(state.speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  SetComboValue(state.speed, SpeedLabel(state.initial.client_connection_speed));
  state.windows_auth = create(0, L"BUTTON", L"Использовать аутентификацию ОС", WS_TABSTOP | BS_AUTOCHECKBOX, 28, 434, 290, 20, kLaunchWindowsAuth, textFont);
  SendMessageW(state.windows_auth, BM_SETCHECK, IsEnabledFlag(state.initial.wa) ? BST_CHECKED : BST_UNCHECKED, 0);
  create(0, L"BUTTON", L"Дополнительные настройки…", WS_TABSTOP, 28, 468, 230, 28, kOpenAdvancedDatabaseOptions, buttonFont);
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, 450, 532, 90, 28, IDOK, buttonFont);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 532, 84, 28, IDCANCEL, buttonFont);
  state.kind = state.initial.kind;
  UpdateConnectionControls(state);
}
LRESULT CALLBACK DatabaseEditorProc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<DatabaseEditorState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command == kConnectionFile || command == kConnectionWeb || command == kConnectionServer) {
      state->kind = command == kConnectionFile ? DatabaseConnectionKind::file : command == kConnectionWeb ? DatabaseConnectionKind::web : DatabaseConnectionKind::server;
      UpdateConnectionControls(*state);
      return 0;
    }
    if (command == kBrowseFilePath) { BrowseForFileBase(wnd, *state); return 0; }
    if (command == kOpenAdvancedDatabaseOptions) {
      if (const auto updated = EditAdvancedDatabaseOptions(wnd, state->initial)) state->initial = *updated;
      return 0;
    }
    if (command == IDOK) {
      if (const auto error = CollectDatabaseEditorResult(*state)) { Message(wnd, *error, L"Проверка данных", MB_OK | MB_ICONWARNING); return 0; }
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
    if (command == IDCANCEL) { state->done = true; DestroyWindow(wnd); return 0; }
  }
  if (message == WM_CLOSE && state) { state->done = true; DestroyWindow(wnd); return 0; }
  return DefWindowProcW(wnd, message, wparam, lparam);
}
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
  if (owner) EnableWindow(owner, FALSE);
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 660, 570, style, extendedStyle);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDatabaseEditorClass, std::wstring(title).c_str(),
      style, CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy,
      owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    if (owner) EnableWindow(owner, TRUE);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_SEMIBOLD);
  CreateDatabaseEditorControls(dialog, state, platforms);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.name);
  MSG message{};
  int pumpResult = 1;
  while (!state.done && (pumpResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (pumpResult == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
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

struct InputState { HWND edit{}; HFONT font{}; HFONT button_font{}; std::optional<std::wstring> result; bool done{false}; };
LRESULT CALLBACK InputWindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* state = reinterpret_cast<InputState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) { SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams)); return TRUE; }
  if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) return DialogControlColor(msg, wp, lp);
  if (msg == WM_COMMAND && state) {
    if (LOWORD(wp) == IDOK) { const int length = GetWindowTextLengthW(state->edit); std::wstring text(length + 1, L'\0'); GetWindowTextW(state->edit, text.data(), length + 1); text.resize(length); state->result = std::move(text); state->done = true; DestroyWindow(wnd); return 0; }
    if (LOWORD(wp) == IDCANCEL) { state->done = true; DestroyWindow(wnd); return 0; }
  }
  if (msg == WM_CLOSE && state) { state->done = true; DestroyWindow(wnd); return 0; }
  return DefWindowProcW(wnd, msg, wp, lp);
}

std::optional<std::wstring> InputBox(HWND owner, std::wstring_view title, std::wstring_view caption, std::wstring_view initial) {
  InputState state;
  static ATOM atom = [] {
    WNDCLASSW klass{}; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = kInputBoxClass; klass.lpfnWndProc = InputWindowProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW); klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); return RegisterClassW(&klass);
  }();
  (void)atom;
  EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 470, 125, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kInputBoxClass, std::wstring(title).c_str(), style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    EnableWindow(owner, TRUE);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_SEMIBOLD);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND captionControl = CreateWindowW(L"STATIC", std::wstring(caption).c_str(), WS_CHILD | WS_VISIBLE, px(14), px(14), px(430), px(20), dialog, nullptr, nullptr, nullptr);
  state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::wstring(initial).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, px(14), px(38), px(430), px(24), dialog, nullptr, nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"ОК", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, px(275), px(78), px(80), px(25), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(364), px(78), px(80), px(25), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(captionControl, state.font);
  SetControlFont(state.edit, state.font);
  SetControlFont(accept, state.button_font ? state.button_font : state.font);
  SetControlFont(cancel, state.button_font ? state.button_font : state.font);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW); SetFocus(state.edit);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) { if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  EnableWindow(owner, TRUE); SetForegroundWindow(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
    storage::Settings settings, std::optional<std::wstring> launch_id)
    : instance_(instance), executable_(std::move(executable)), layout_(std::move(layout)), settings_(std::move(settings)),
      logger_(layout_.root / L"logs"), initial_launch_id_(std::move(launch_id)) {}
MainWindow::~MainWindow() {
  if (window_ && IsWindow(window_)) DestroyWindow(window_);
  ClearContextMenuItems();
  for (const auto images : button_images_) if (images) ImageList_Destroy(images);
  if (tree_images_) ImageList_Destroy(tree_images_);
  if (controls_font_) DeleteObject(controls_font_);
  if (button_font_) DeleteObject(button_font_);
  if (details_title_font_) DeleteObject(details_title_font_);
  if (details_subtitle_font_) DeleteObject(details_subtitle_font_);
  if (details_key_font_) DeleteObject(details_key_font_);
}

int MainWindow::Show(int show_command) {
  WNDCLASSEXW klass{sizeof(klass)}; klass.hInstance = instance_; klass.lpszClassName = kClassName; klass.lpfnWndProc = WindowProc;
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW); klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  klass.hIcon = LoadResourceIcon(instance_, IDI_IBSTART, GetSystemMetrics(SM_CXICON)); klass.hIconSm = LoadResourceIcon(instance_, IDI_IBSTART, GetSystemMetrics(SM_CXSMICON));
  if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
  int windowX = settings_.window_x;
  int windowY = settings_.window_y;
  const int windowWidth = std::max(settings_.window_width, ScaleForDpi(kMinimumWindowWidth, GetDpiForSystem()));
  const int windowHeight = std::max(settings_.window_height, ScaleForDpi(kMinimumWindowHeight, GetDpiForSystem()));
  if (windowX != CW_USEDEFAULT && windowY != CW_USEDEFAULT) {
    const RECT saved{windowX, windowY, windowX + windowWidth, windowY + windowHeight};
    if (!MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) { windowX = CW_USEDEFAULT; windowY = CW_USEDEFAULT; }
  }
  window_ = CreateWindowExW(0, kClassName, L"ИБ Старт — IBStart", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, instance_, this);
  if (!window_) return 1;
  ShowWindow(window_, show_command); UpdateWindow(window_);
  constexpr BYTE control = FVIRTKEY | FCONTROL;
  constexpr BYTE controlAlt = FVIRTKEY | FCONTROL | FALT;
  constexpr BYTE controlShift = FVIRTKEY | FCONTROL | FSHIFT;
  constexpr BYTE altShift = FVIRTKEY | FALT | FSHIFT;
  ACCEL accelerators[] = {{FVIRTKEY, VK_F1, kAbout}, {FVIRTKEY, VK_F2, kEdit}, {FVIRTKEY, VK_F3, kEnterprise}, {FVIRTKEY, VK_F4, kDesigner}, {FVIRTKEY, VK_F5, kRefresh},
      {control, 'F', kFocusSearch}, {control, 'O', kOpenList}, {controlAlt, 'F', kAddFile}, {controlAlt, 'S', kAddServer}, {controlAlt, 'G', kAddGroup},
      {controlAlt, 'I', kToggleFavorite}, {controlAlt, 'M', kSimpleMode}, {controlShift, VK_DELETE, kCache}, {controlShift, 'S', kShortcut},
      {controlShift, VK_UP, kMoveUp}, {controlShift, VK_DOWN, kMoveDown}, {altShift, VK_DELETE, kDelete},
      {static_cast<BYTE>(FVIRTKEY | FALT), '1', kFavorite1}, {static_cast<BYTE>(FVIRTKEY | FALT), '2', kFavorite1 + 1}, {static_cast<BYTE>(FVIRTKEY | FALT), '3', kFavorite1 + 2},
      {static_cast<BYTE>(FVIRTKEY | FALT), '4', kFavorite1 + 3}, {static_cast<BYTE>(FVIRTKEY | FALT), '5', kFavorite1 + 4}, {static_cast<BYTE>(FVIRTKEY | FALT), '6', kFavorite1 + 5},
      {static_cast<BYTE>(FVIRTKEY | FALT), '7', kFavorite1 + 6}, {static_cast<BYTE>(FVIRTKEY | FALT), '8', kFavorite1 + 7}, {static_cast<BYTE>(FVIRTKEY | FALT), '9', kFavorite1 + 8}};
  HACCEL accelerator = CreateAcceleratorTableW(accelerators, static_cast<int>(sizeof(accelerators) / sizeof(*accelerators)));
  MSG message{};
  int getMessageResult = 1;
  while ((getMessageResult = GetMessageW(&message, nullptr, 0, 0)) > 0) { if (!accelerator || !TranslateAcceleratorW(window_, accelerator, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
  if (accelerator) DestroyAcceleratorTable(accelerator);
  if (getMessageResult < 0) return 1;
  return static_cast<int>(message.wParam);
}

void MainWindow::Activate() { if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    if (!self) return FALSE;
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) return DefWindowProcW(window, message, wparam, lparam);
  try {
    return self->Handle(window, message, wparam, lparam);
  } catch (const std::exception& error) {
    self->ReportUnhandledError(error.what());
    return message == WM_NCCREATE ? FALSE : message == WM_CREATE ? -1 : 0;
  } catch (...) {
    self->ReportUnhandledError("Unknown exception");
    return message == WM_NCCREATE ? FALSE : message == WM_CREATE ? -1 : 0;
  }
}

LRESULT MainWindow::Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE: CreateControls(); LoadCatalog(); return 0;
    case WM_SIZE: Layout(LOWORD(lparam), HIWORD(lparam)); return 0;
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      if (limits) {
        const UINT dpi = GetDpiForWindow(window);
        limits->ptMinTrackSize.x = ScaleForDpi(kMinimumWindowWidth, dpi);
        limits->ptMinTrackSize.y = ScaleForDpi(kMinimumWindowHeight, dpi);
      }
      return 0;
    }
    case WM_SETFOCUS: SetFocus(search_); return 0;
    case WM_KEYDOWN: if (wparam == VK_F3) { LaunchSelected(domain::LaunchMode::enterprise); return 0; } if (wparam == VK_F4) { LaunchSelected(domain::LaunchMode::designer); return 0; } break;
    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lparam) == search_ && HIWORD(wparam) == EN_CHANGE) { PopulateTree(); return 0; }
      switch (LOWORD(wparam)) {
        case kEnterprise: LaunchSelected(domain::LaunchMode::enterprise); break; case kDesigner: LaunchSelected(domain::LaunchMode::designer); break;
        case kAddFile: AddFileDatabase(); break; case kAddServer: AddServerDatabase(); break; case kAddGroup: AddGroup(); break; case kOpenList: OpenList(); break; case kRefresh: LoadCatalog(); break;
        case kEdit: EditSelected(); break; case kCache: ClearSelectedCache(); break; case kShortcut: CreateShortcut(); break; case kDelete: DeleteSelected(); break;
        case kSimpleMode: SetSimpleMode(!settings_.simple_mode); break; case kToggleFavorite: ToggleFavorite(); break; case kFocusSearch: SetFocus(search_); break; case kAbout: ShowAbout(); break;
        case kMoveUp: MoveSelected(-1); break; case kMoveDown: MoveSelected(1); break;
        default: if (LOWORD(wparam) >= kFavorite1 && LOWORD(wparam) < kFavorite1 + 9) LaunchFavorite(LOWORD(wparam) - kFavorite1); break;
      } return 0;
    case WM_NOTIFY:
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == tree_) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) return DrawTreeSearchMatches(reinterpret_cast<NMTVCUSTOMDRAW*>(lparam));
        if (notification->code == TVN_SELCHANGEDW) { DisplaySelected(); return 0; }
        if (notification->code == TVN_BEGINDRAGW) { const auto* drag = reinterpret_cast<NMTREEVIEWW*>(lparam); TreeView_SelectItem(tree_, drag->itemNew.hItem); dragging_name_ = SelectedName(); SetCapture(window_); return 0; }
      }
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == details_ && reinterpret_cast<NMHDR*>(lparam)->code == NM_CUSTOMDRAW) {
        return DrawDetailsList(reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
      }
      break;
    case WM_CONTEXTMENU:
      if (reinterpret_cast<HWND>(wparam) == tree_) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ShowTreeContextMenu(screen);
        return 0;
      }
      break;
    case WM_CTLCOLORSTATIC:
      if (reinterpret_cast<HWND>(lparam) == details_title_ || reinterpret_cast<HWND>(lparam) == details_subtitle_) {
        const auto context = reinterpret_cast<HDC>(wparam);
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context, reinterpret_cast<HWND>(lparam) == details_title_ ? RGB(0, 116, 136) : RGB(82, 96, 109));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
      }
      return DialogControlColor(message, wparam, lparam);
    case WM_MEASUREITEM:
      if (lparam && MeasureContextMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
      break;
    case WM_DRAWITEM:
      if (lparam && DrawContextMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
      break;
    case WM_LBUTTONUP:
      if (!dragging_name_.empty() && catalog_) {
        ReleaseCapture(); TVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; MapWindowPoints(window_, tree_, &hit.pt, 1); TreeView_HitTest(tree_, &hit);
        if (hit.hItem) { TreeView_SelectItem(tree_, hit.hItem); const auto target = SelectedName(); if (!target.empty() && target != dragging_name_) { const auto* targetEntry = catalog_->Find(target); if (targetEntry && targetEntry->IsGroup()) { catalog_->Move(dragging_name_, target, 0); SaveCatalog(); PopulateTree(); } } }
        dragging_name_.clear(); return 0;
      } break;
    case WM_COPYDATA: {
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      if (!data || data->dwData != kLaunchCopyData || !data->lpData || data->cbData < sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) return FALSE;
      const auto* value = static_cast<const wchar_t*>(data->lpData);
      const size_t length = data->cbData / sizeof(wchar_t);
      if (value[length - 1] != L'\0') return FALSE;
      initial_launch_id_ = std::wstring(value, length - 1);
      SetWindowTextW(search_, L"");
      PopulateTree();
      Activate();
      return TRUE;
    }
    case kActivateMessage: Activate(); return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: {
      WINDOWPLACEMENT placement{sizeof(placement)};
      if (GetWindowPlacement(window, &placement)) { const RECT& rect = placement.rcNormalPosition; settings_.window_x = rect.left; settings_.window_y = rect.top; settings_.window_width = rect.right - rect.left; settings_.window_height = rect.bottom - rect.top; }
      try { storage::SaveSettings(layout_, settings_); } catch (...) {}
      window_ = nullptr;
      PostQuitMessage(0); return 0;
    }
  }
  if (message == WM_KEYDOWN && wparam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) { SetFocus(search_); return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}

void MainWindow::CreateControls() {
  HWND searchLabel = CreateWindowW(L"STATIC", L"Поиск:", WS_CHILD | WS_VISIBLE, 8, 10, 50, 20, window_, nullptr, instance_, nullptr);
  search_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 58, 7, 600, 25, window_, nullptr, instance_, nullptr);
  tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, 8, 42, 360, 420, window_, nullptr, instance_, nullptr);
  TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  details_title_ = CreateWindowW(L"STATIC", L"Выберите базу или группу", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 49, 460, 26, window_, nullptr, instance_, nullptr);
  details_subtitle_ = CreateWindowW(L"STATIC", L"Сведения появятся здесь", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 76, 460, 20, window_, nullptr, instance_, nullptr);
  details_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 380, 100, 480, 182, window_, nullptr, instance_, nullptr);
  controls_font_ = CreateUiFont(window_, 9, FW_NORMAL);
  button_font_ = CreateUiFont(window_, 9, FW_SEMIBOLD);
  if (controls_font_) {
    for (const HWND control : {searchLabel, search_, tree_, details_}) {
      if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controls_font_), TRUE);
    }
  }
  ListView_SetExtendedListViewStyle(details_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  ListView_SetBkColor(details_, RGB(248, 250, 252));
  ListView_SetTextBkColor(details_, RGB(248, 250, 252));
  ListView_SetTextColor(details_, RGB(36, 50, 60));
  LVCOLUMNW keyColumn{}; keyColumn.mask = LVCF_WIDTH; keyColumn.cx = 150; ListView_InsertColumn(details_, 0, &keyColumn);
  LVCOLUMNW valueColumn{}; valueColumn.mask = LVCF_WIDTH; valueColumn.cx = 325; ListView_InsertColumn(details_, 1, &valueColumn);
  details_title_font_ = CreateUiFont(window_, 14, FW_SEMIBOLD);
  details_subtitle_font_ = CreateUiFont(window_, 9, FW_NORMAL);
  details_key_font_ = CreateUiFont(window_, 9, FW_SEMIBOLD);
  if (details_title_font_) SendMessageW(details_title_, WM_SETFONT, reinterpret_cast<WPARAM>(details_title_font_), TRUE);
  if (details_subtitle_font_) SendMessageW(details_subtitle_, WM_SETFONT, reinterpret_cast<WPARAM>(details_subtitle_font_), TRUE);

  tree_images_ = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 5, 1);
  if (tree_images_) {
    constexpr int resources[] = {IDI_TREE_FILE_DATABASE, IDI_TREE_SERVER_DATABASE, IDI_TREE_FOLDER, IDI_ACTION_FAVORITE, IDI_ACTION_REFRESH};
    bool complete = true;
    for (const int resource : resources) {
      const auto icon = LoadResourceIcon(instance_, resource, 20);
      if (!icon || ImageList_AddIcon(tree_images_, icon) < 0) complete = false;
      if (icon) DestroyIcon(icon);
    }
    if (complete) TreeView_SetImageList(tree_, tree_images_, TVSIL_NORMAL);
    else { ImageList_Destroy(tree_images_); tree_images_ = nullptr; }
  }
  enterprise_ = CreateWindowW(L"BUTTON", L"Предприятие (F3)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 380, 292, 150, 30, window_, reinterpret_cast<HMENU>(kEnterprise), instance_, nullptr);
  designer_ = CreateWindowW(L"BUTTON", L"Конфигуратор (F4)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 534, 292, 150, 30, window_, reinterpret_cast<HMENU>(kDesigner), instance_, nullptr);
  edit_ = CreateWindowW(L"BUTTON", L"Изменить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 688, 292, 105, 30, window_, reinterpret_cast<HMENU>(kEdit), instance_, nullptr);
  cache_ = CreateWindowW(L"BUTTON", L"Очистить кэш", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 380, 328, 115, 30, window_, reinterpret_cast<HMENU>(kCache), instance_, nullptr);
  shortcut_ = CreateWindowW(L"BUTTON", L"Создать ярлык", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 504, 328, 130, 30, window_, reinterpret_cast<HMENU>(kShortcut), instance_, nullptr);
  remove_ = CreateWindowW(L"BUTTON", L"Удалить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 643, 328, 100, 30, window_, reinterpret_cast<HMENU>(kDelete), instance_, nullptr);
  if (button_font_) {
    for (const HWND button : {enterprise_, designer_, edit_, cache_, shortcut_, remove_}) {
      SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(button_font_), TRUE);
    }
  }
  const HWND tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
  if (tooltips) {
    SetWindowPos(tooltips, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (controls_font_) SendMessageW(tooltips, WM_SETFONT, reinterpret_cast<WPARAM>(controls_font_), TRUE);
    AddButtonTooltip(tooltips, enterprise_, L"Запустить в режиме Предприятие — F3");
    AddButtonTooltip(tooltips, designer_, L"Запустить в режиме Конфигуратор — F4");
    AddButtonTooltip(tooltips, edit_, L"Изменить выбранную запись — F2");
    AddButtonTooltip(tooltips, cache_, L"Очистить кэш выбранной базы — Ctrl+Shift+Del");
    AddButtonTooltip(tooltips, shortcut_, L"Создать ярлык выбранной базы — Ctrl+Shift+S");
    AddButtonTooltip(tooltips, remove_, L"Удалить выбранную запись — Alt+Shift+Del");
  }
  AttachButtonIcon(enterprise_, instance_, IDI_ACTION_ENTERPRISE, button_images_);
  AttachButtonIcon(designer_, instance_, IDI_ACTION_DESIGNER, button_images_);
  AttachButtonIcon(edit_, instance_, IDI_ACTION_EDIT, button_images_);
  AttachButtonIcon(cache_, instance_, IDI_ACTION_CACHE, button_images_);
  AttachButtonIcon(shortcut_, instance_, IDI_ACTION_SHORTCUT, button_images_);
  AttachButtonIcon(remove_, instance_, IDI_ACTION_DELETE, button_images_);
  status_ = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kOpenList, L"Открыть список баз…\tCtrl+O"); AppendMenuW(file, MF_STRING, kAddFile, L"Добавить файловую базу…\tCtrl+Alt+F"); AppendMenuW(file, MF_STRING, kAddServer, L"Добавить серверную базу…\tCtrl+Alt+S"); AppendMenuW(file, MF_STRING, kAddGroup, L"Добавить группу…\tCtrl+Alt+G"); AppendMenuW(file, MF_STRING, kRefresh, L"Обновить список\tF5");
  AppendMenuW(view, MF_STRING, kToggleFavorite, L"Добавить/убрать из избранного\tCtrl+Alt+I");
  AppendMenuW(view, MF_STRING, kSimpleMode, L"Простой режим\tCtrl+Alt+M"); AppendMenuW(help, MF_STRING, kAbout, L"О программе…\tF1"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"Файл"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"Вид"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Справка"); SetMenu(window_, menu);
  SetSimpleMode(settings_.simple_mode);
  DisplaySelected();
}

void MainWindow::Layout(int width, int height) {
  constexpr int statusHeight = 22;
  constexpr int top = 42;
  constexpr int bottom = statusHeight + 10;
  constexpr int buttonGap = 8;
  constexpr int buttonRowGap = 6;
  constexpr int buttonHeight = 30;
  const int leftWidth = std::clamp(width * 42 / 100, 220, std::max(220, width - 300));
  const int rightX = leftWidth + 18;
  const int rightWidth = std::max(120, width - rightX - 10);

  struct ButtonLayout { HWND window; int width; int x; int y; };
  ButtonLayout buttons[] = {
      {enterprise_, ButtonIdealWidth(enterprise_, 170), 0, 0},
      {designer_, ButtonIdealWidth(designer_, 185), 0, 0},
      {edit_, ButtonIdealWidth(edit_, 115), 0, 0},
      {cache_, ButtonIdealWidth(cache_, 132), 0, 0},
      {shortcut_, ButtonIdealWidth(shortcut_, 150), 0, 0},
      {remove_, ButtonIdealWidth(remove_, 115), 0, 0}};
  int buttonRows = 1;
  int buttonX = 0;
  for (auto& button : buttons) {
    if (buttonX != 0 && buttonX + buttonGap + button.width > rightWidth) {
      ++buttonRows;
      buttonX = 0;
    }
    if (button.width > rightWidth) button.width = rightWidth;
    button.x = buttonX;
    button.y = (buttonRows - 1) * (buttonHeight + buttonRowGap);
    buttonX += button.width + buttonGap;
  }
  const int buttonsHeight = buttonRows * buttonHeight + (buttonRows - 1) * buttonRowGap;
  const int buttonsY = std::max(top + 100, height - bottom - buttonsHeight);
  const int detailsY = top + 58;
  const int detailsHeight = std::max(42, buttonsY - detailsY - 10);
  const int keyWidth = std::clamp(rightWidth * 35 / 100, 80, 190);

  HDWP positions = BeginDeferWindowPos(11);
  const auto defer = [&positions](HWND control, int x, int y, int controlWidth, int controlHeight) {
    if (!positions || !control) return;
    positions = DeferWindowPos(positions, control, nullptr, x, y, std::max(1, controlWidth), std::max(1, controlHeight),
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  };
  defer(search_, 58, 7, width - 66, 25);
  defer(tree_, 8, top, leftWidth, height - top - bottom);
  defer(details_title_, rightX + 10, top + 7, rightWidth - 20, 26);
  defer(details_subtitle_, rightX + 10, top + 34, rightWidth - 20, 20);
  defer(details_, rightX, detailsY, rightWidth, detailsHeight);
  for (const auto& button : buttons) defer(button.window, rightX + button.x, buttonsY + button.y, button.width, buttonHeight);
  const bool positioned = positions && EndDeferWindowPos(positions) != FALSE;
  if (!positioned) {
    // DeferWindowPos can fail only under severe resource pressure.  Keep a
    // complete fallback layout instead of leaving controls at old positions.
    MoveWindow(search_, 58, 7, std::max(1, width - 66), 25, TRUE);
    MoveWindow(tree_, 8, top, leftWidth, std::max(1, height - top - bottom), TRUE);
    MoveWindow(details_title_, rightX + 10, top + 7, std::max(1, rightWidth - 20), 26, TRUE);
    MoveWindow(details_subtitle_, rightX + 10, top + 34, std::max(1, rightWidth - 20), 20, TRUE);
    MoveWindow(details_, rightX, detailsY, rightWidth, detailsHeight, TRUE);
    for (const auto& button : buttons) MoveWindow(button.window, rightX + button.x, buttonsY + button.y, button.width, buttonHeight, TRUE);
  }
  ListView_SetColumnWidth(details_, 0, keyWidth);
  ListView_SetColumnWidth(details_, 1, std::max(1, rightWidth - keyWidth - 5));
  SendMessageW(status_, WM_SIZE, 0, 0);
  // Layout changes move child windows away from their previous rectangles.
  // Repaint the parent and all children in one pass so stale button pixels
  // cannot remain visible while the user drags a window border.
  RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::LoadCatalog(bool report_error) {
  try {
    if (settings_.active_ibases.empty()) { if (const auto standard = storage::FindStandardIbases()) settings_.active_ibases = *standard; }
    auto discoveredPlatforms = platform::Discover(settings_.platform_search_paths);
    if (settings_.active_ibases.empty() || !std::filesystem::exists(settings_.active_ibases)) {
      catalog_.emplace();
      store_.reset();
      platforms_ = std::move(discoveredPlatforms);
      SetStatus(L"Список ibases.v8i не найден — выберите файл или добавьте базу. | " + CatalogStatistics());
    } else {
      v8i::V8iFileStore loadedStore(settings_.active_ibases);
      catalog::Catalog loadedCatalog(loadedStore.Read());
      store_ = std::move(loadedStore);
      catalog_ = std::move(loadedCatalog);
      platforms_ = std::move(discoveredPlatforms);
      SetStatus(settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
      logger_.Info(L"Загружен список баз: " + settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
    }
    PopulateTree();
  } catch (const std::exception& error) { logger_.Error(L"Ошибка загрузки: " + ibstart::utf::FromUtf8(error.what())); if (report_error) Message(window_, L"Не удалось загрузить список баз. Проверьте путь и кодировку UTF-8.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

void MainWindow::SaveCatalog() {
  if (!catalog_) return;
  try {
    if (!store_) {
      if (settings_.active_ibases.empty()) {
        wchar_t filename[MAX_PATH] = L"ibases.v8i";
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_; dialog.lpstrFilter = L"ibases.v8i\0ibases.v8i\0Все файлы\0*.*\0"; dialog.lpstrFile = filename; dialog.nMaxFile = MAX_PATH; dialog.lpstrDefExt = L"v8i"; dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog)) return;
        settings_.active_ibases = filename;
      }
      store_.emplace(settings_.active_ibases);
    }
    store_->Save(catalog_->document()); storage::SaveSettings(layout_, settings_); SetStatus(L"Сохранено: " + settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
  } catch (const v8i::ExternalModificationError&) { const int answer = MessageBoxW(window_, L"Файл ibases.v8i был изменён другой программой. Перечитать его?", L"ИБ Старт", MB_YESNO | MB_ICONWARNING); if (answer == IDYES) LoadCatalog(); }
  catch (const std::exception& error) { logger_.Error(L"Ошибка записи: " + ibstart::utf::FromUtf8(error.what())); Message(window_, L"Не удалось сохранить ibases.v8i. Исходный файл не изменён.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

bool MainWindow::ItemMatches(const catalog::TreeItem& item, std::wstring_view filter) const { if (filter.empty() || utf::FindNoCaseOrdinal(item.name, filter) != std::wstring_view::npos) return true; return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) { return ItemMatches(child, filter); }); }
void MainWindow::AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter) {
  for (const auto& item : items) {
    if (!ItemMatches(item, filter)) continue;
    TVINSERTSTRUCTW row{}; row.hParent = parent; row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    row.item.pszText = const_cast<wchar_t*>(item.name.c_str());
    const auto* entry = catalog_ ? catalog_->Find(item.name) : nullptr;
    row.item.iImage = row.item.iSelectedImage = item.database ? DatabaseTreeImage(entry) : kFolderImage;
    const HTREEITEM handle = TreeView_InsertItem(tree_, &row);
    if (!item.database) {
      AddTreeItems(item.children, handle, filter);
      if (!filter.empty()) TreeView_Expand(tree_, handle, TVE_EXPAND);
    }
  }
}
void MainWindow::PopulateTree() {
  if (!tree_) return;
  wchar_t text[512]{}; GetWindowTextW(search_, text, 512); search_filter_ = text; const std::wstring_view filter = search_filter_; TreeView_DeleteAllItems(tree_);
  if (catalog_) {
    AddTreeItems(catalog_->Tree(), TVI_ROOT, filter);
    const auto addSpecialRoot = [&](std::wstring_view rootName, const std::vector<std::wstring>& names, int image) {
      TVINSERTSTRUCTW root{}; root.hParent = TVI_ROOT; root.hInsertAfter = TVI_LAST;
      root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE; root.item.pszText = const_cast<wchar_t*>(rootName.data());
      root.item.iImage = root.item.iSelectedImage = image;
      const HTREEITEM rootHandle = TreeView_InsertItem(tree_, &root);
      bool any = false;
      for (const auto& name : names) {
        const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase() || (!filter.empty() && utf::FindNoCaseOrdinal(entry->name, filter) == std::wstring_view::npos)) continue;
        TVINSERTSTRUCTW row{}; row.hParent = rootHandle; row.hInsertAfter = TVI_LAST;
        row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE; row.item.pszText = const_cast<wchar_t*>(entry->name.c_str());
        row.item.iImage = row.item.iSelectedImage = DatabaseTreeImage(entry); TreeView_InsertItem(tree_, &row); any = true;
      }
      if (any) TreeView_Expand(tree_, rootHandle, TVE_EXPAND); else TreeView_DeleteItem(tree_, rootHandle);
    };
    addSpecialRoot(L"Избранное", storage::LoadFavorites(layout_), kFavoriteImage);
    std::vector<std::wstring> recent;
    for (const auto& history : storage::LoadHistory(layout_)) for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == history.database_id) { recent.push_back(entry->name); break; }
    addSpecialRoot(L"Недавние", recent, kRecentImage);
  }
  if (initial_launch_id_) {
    auto wanted = *initial_launch_id_; initial_launch_id_.reset();
    if (catalog_) for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == wanted) { wanted = entry->name; break; }
    SelectTreeItem(wanted);
  }
  DisplaySelected();
}
LRESULT MainWindow::DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return search_filter_.empty() ? CDRF_DODEFAULT : CDRF_NOTIFYPOSTPAINT;
  if (draw->nmcd.dwDrawStage != CDDS_ITEMPOSTPAINT || search_filter_.empty()) return CDRF_DODEFAULT;

  const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
  wchar_t text[512]{};
  TVITEMW treeItem{}; treeItem.mask = TVIF_TEXT; treeItem.hItem = item; treeItem.pszText = text; treeItem.cchTextMax = 512;
  if (!TreeView_GetItem(tree_, &treeItem)) return CDRF_DODEFAULT;
  const std::wstring_view label(text);
  if (utf::FindNoCaseOrdinal(label, search_filter_) == std::wstring_view::npos) return CDRF_DODEFAULT;

  RECT labelRect{};
  if (!TreeView_GetItemRect(tree_, item, &labelRect, TRUE)) return CDRF_DODEFAULT;
  const int saved = SaveDC(draw->nmcd.hdc);
  if (const auto font = reinterpret_cast<HFONT>(SendMessageW(tree_, WM_GETFONT, 0, 0))) SelectObject(draw->nmcd.hdc, font);
  SetBkMode(draw->nmcd.hdc, TRANSPARENT);
  SetTextColor(draw->nmcd.hdc, RGB(0, 97, 0));
  const HBRUSH matchBrush = CreateSolidBrush(RGB(198, 239, 206));
  if (!matchBrush) { RestoreDC(draw->nmcd.hdc, saved); return CDRF_DODEFAULT; }

  size_t start = 0;
  size_t match = utf::FindNoCaseOrdinal(label, search_filter_, start);
  while (match != std::wstring_view::npos) {
    SIZE prefixSize{}, matchSize{};
    GetTextExtentPoint32W(draw->nmcd.hdc, label.data(), static_cast<int>(match), &prefixSize);
    GetTextExtentPoint32W(draw->nmcd.hdc, label.data() + match, static_cast<int>(search_filter_.size()), &matchSize);
    RECT matchRect{labelRect.left + prefixSize.cx, labelRect.top + 1, labelRect.left + prefixSize.cx + matchSize.cx, labelRect.bottom - 1};
    FillRect(draw->nmcd.hdc, &matchRect, matchBrush);
    RECT textRect{matchRect.left, labelRect.top, matchRect.right, labelRect.bottom};
    DrawTextW(draw->nmcd.hdc, label.data() + match, static_cast<int>(search_filter_.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    start = match + search_filter_.size();
    match = utf::FindNoCaseOrdinal(label, search_filter_, start);
  }
  DeleteObject(matchBrush);
  RestoreDC(draw->nmcd.hdc, saved);
  return CDRF_DODEFAULT;
}
LRESULT MainWindow::DrawDetailsList(NMLVCUSTOMDRAW* draw) const {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
  if (draw->nmcd.dwDrawStage != (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) return CDRF_DODEFAULT;
  const auto row = static_cast<int>(draw->nmcd.dwItemSpec);
  draw->clrTextBk = row % 2 == 0 ? RGB(242, 248, 249) : RGB(250, 252, 253);
  if (draw->iSubItem == 0) {
    draw->clrText = RGB(0, 111, 129);
    if (details_key_font_) {
      SelectObject(draw->nmcd.hdc, details_key_font_);
      return CDRF_NEWFONT;
    }
  } else {
    draw->clrText = RGB(36, 50, 60);
  }
  return CDRF_DODEFAULT;
}
bool MainWindow::MeasureContextMenuItem(MEASUREITEMSTRUCT* measure) const {
  if (!measure || measure->CtlType != ODT_MENU) return false;
  const auto* item = reinterpret_cast<const ContextMenuItem*>(measure->itemData);
  const auto found = std::find_if(context_menu_items_.begin(), context_menu_items_.end(), [item](const auto& candidate) { return &candidate == item; });
  if (found == context_menu_items_.end()) return false;

  HDC context = GetDC(window_);
  if (!context) return false;
  const HFONT font = controls_font_ ? controls_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  const auto previous = SelectObject(context, font);
  SIZE titleSize{};
  SIZE shortcutSize{};
  GetTextExtentPoint32W(context, item->text.c_str(), static_cast<int>(item->text.size()), &titleSize);
  if (!item->shortcut.empty()) {
    GetTextExtentPoint32W(context, item->shortcut.c_str(), static_cast<int>(item->shortcut.size()), &shortcutSize);
  }
  SelectObject(context, previous);
  ReleaseDC(window_, context);
  measure->itemHeight = 28;
  measure->itemWidth = std::max<UINT>(210u, static_cast<UINT>(titleSize.cx) + static_cast<UINT>(shortcutSize.cx) + 66u);
  return true;
}

bool MainWindow::DrawContextMenuItem(const DRAWITEMSTRUCT* draw) const {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const auto* item = reinterpret_cast<const ContextMenuItem*>(draw->itemData);
  const auto found = std::find_if(context_menu_items_.begin(), context_menu_items_.end(), [item](const auto& candidate) { return &candidate == item; });
  if (found == context_menu_items_.end()) return false;

  const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
  const bool selected = (draw->itemState & ODS_SELECTED) != 0 && !disabled;
  const int saved = SaveDC(draw->hDC);
  FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));
  const int iconX = draw->rcItem.left + 7;
  const int iconY = draw->rcItem.top + (static_cast<int>(draw->rcItem.bottom - draw->rcItem.top) - 20) / 2;
  if (item->command == kMoveUp || item->command == kMoveDown) {
    const COLORREF color = disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(0, 144, 162);
    const HBRUSH brush = CreateSolidBrush(color);
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto previousBrush = SelectObject(draw->hDC, brush);
    const auto previousPen = SelectObject(draw->hDC, pen);
    POINT arrow[] = {{iconX + 10, item->command == kMoveUp ? iconY + 1 : iconY + 19},
        {iconX + 2, item->command == kMoveUp ? iconY + 9 : iconY + 11},
        {iconX + 7, item->command == kMoveUp ? iconY + 9 : iconY + 11},
        {iconX + 7, item->command == kMoveUp ? iconY + 19 : iconY + 1},
        {iconX + 13, item->command == kMoveUp ? iconY + 19 : iconY + 1},
        {iconX + 13, item->command == kMoveUp ? iconY + 9 : iconY + 11},
        {iconX + 18, item->command == kMoveUp ? iconY + 9 : iconY + 11}};
    Polygon(draw->hDC, arrow, 7);
    SelectObject(draw->hDC, previousBrush);
    SelectObject(draw->hDC, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);
  } else if (item->icon) {
    if (disabled) DrawStateW(draw->hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(item->icon), 0, iconX, iconY, 20, 20, DST_ICON | DSS_DISABLED);
    else DrawIconEx(draw->hDC, iconX, iconY, item->icon, 20, 20, 0, nullptr, DI_NORMAL);
  }
  const HFONT font = controls_font_ ? controls_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SelectObject(draw->hDC, font);
  SetBkMode(draw->hDC, TRANSPARENT);
  RECT textRect = draw->rcItem;
  textRect.left += 35;
  textRect.right -= 10;
  if (!item->shortcut.empty()) {
    SIZE shortcutSize{};
    GetTextExtentPoint32W(draw->hDC, item->shortcut.c_str(), static_cast<int>(item->shortcut.size()), &shortcutSize);
    RECT shortcutRect = draw->rcItem;
    shortcutRect.right -= 10;
    shortcutRect.left = std::max(textRect.left + 64, shortcutRect.right - shortcutSize.cx);
    textRect.right = shortcutRect.left - 12;
    const COLORREF shortcutColor = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(91, 109, 121);
    SetTextColor(draw->hDC, shortcutColor);
    DrawTextW(draw->hDC, item->shortcut.c_str(), static_cast<int>(item->shortcut.size()), &shortcutRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
  }
  SetTextColor(draw->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : (disabled ? COLOR_GRAYTEXT : COLOR_MENUTEXT)));
  DrawTextW(draw->hDC, item->text.c_str(), static_cast<int>(item->text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
  RestoreDC(draw->hDC, saved);
  return true;
}

void MainWindow::ClearContextMenuItems() noexcept {
  for (const auto& item : context_menu_items_) if (item.icon) DestroyIcon(item.icon);
  context_menu_items_.clear();
}

std::wstring MainWindow::SelectedName() const { const auto item = TreeView_GetSelection(tree_); if (!item) return {}; wchar_t text[512]{}; TVITEMW data{}; data.mask = TVIF_TEXT; data.hItem = item; data.pszText = text; data.cchTextMax = 512; return TreeView_GetItem(tree_, &data) ? text : L""; }
bool MainWindow::SelectTreeItem(std::wstring_view name) {
  if (!tree_ || name.empty()) return false;
  const auto find = [&](auto&& self, HTREEITEM item) -> HTREEITEM {
    for (; item; item = TreeView_GetNextSibling(tree_, item)) {
      wchar_t text[512]{}; TVITEMW row{}; row.mask = TVIF_TEXT; row.hItem = item; row.pszText = text; row.cchTextMax = 512;
      if (TreeView_GetItem(tree_, &row) && name == text) return item;
      if (const auto child = self(self, TreeView_GetChild(tree_, item))) return child;
    }
    return nullptr;
  };
  const auto item = find(find, TreeView_GetRoot(tree_));
  if (!item) return false;
  TreeView_SelectItem(tree_, item);
  TreeView_EnsureVisible(tree_, item);
  return true;
}

void MainWindow::ShowTreeContextMenu(POINT screen) {
  if (!tree_ || !catalog_) return;
  ClearContextMenuItems();
  if (screen.x == -1 && screen.y == -1) {
    const auto selected = TreeView_GetSelection(tree_);
    RECT bounds{};
    if (selected && TreeView_GetItemRect(tree_, selected, &bounds, TRUE)) {
      screen = {bounds.left, bounds.bottom};
      MapWindowPoints(tree_, nullptr, &screen, 1);
    } else {
      screen = {8, 8};
      MapWindowPoints(tree_, nullptr, &screen, 1);
    }
  } else {
    POINT client = screen;
    ScreenToClient(tree_, &client);
    TVHITTESTINFO hit{}; hit.pt = client;
    if (TreeView_HitTest(tree_, &hit) && (hit.flags & TVHT_ONITEM)) TreeView_SelectItem(tree_, hit.hItem);
    else TreeView_SelectItem(tree_, nullptr);
  }

  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  const bool database = entry && entry->IsDatabase();
  const bool group = entry && entry->IsGroup();
  const bool editable = entry && !settings_.simple_mode;
  const std::wstring addParent = group ? entry->name : entry ? catalog_->ParentOf(entry->name) : std::wstring();
  const auto favorites = storage::LoadFavorites(layout_);
  const bool favorite = std::find(favorites.begin(), favorites.end(), name) != favorites.end();

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  context_menu_items_.reserve(16);
  const auto append = [&](bool enabled, bool checked, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}) {
    ContextMenuItem visual{command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20), std::move(text), std::move(shortcut)};
    context_menu_items_.push_back(std::move(visual));
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW;
    item.wID = command;
    item.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
    item.dwItemData = reinterpret_cast<ULONG_PTR>(&context_menu_items_.back());
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
  };
  const auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
  append(database, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
  append(database, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
  separator();
  append(database, favorite, kToggleFavorite, IDI_ACTION_FAVORITE, favorite ? L"Убрать из избранного" : L"Добавить в избранное", L"Ctrl+Alt+I");
  append(editable, false, kEdit, IDI_ACTION_EDIT, L"Изменить…", L"F2");
  append(database && !settings_.simple_mode, false, kCache, IDI_ACTION_CACHE, L"Очистить кэш…", L"Ctrl+Shift+Del");
  append(database && !settings_.simple_mode, false, kShortcut, IDI_ACTION_SHORTCUT, L"Создать ярлык", L"Ctrl+Shift+S");
  separator();
  append(editable, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
  append(editable, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
  append(editable, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
  separator();
  append(!settings_.simple_mode, false, kAddFile, IDI_ACTION_ADD, group ? L"Добавить файловую базу в группу…" : L"Добавить файловую базу…", L"Ctrl+Alt+F");
  append(!settings_.simple_mode, false, kAddServer, IDI_ACTION_ADD, group ? L"Добавить серверную базу в группу…" : L"Добавить серверную базу…", L"Ctrl+Alt+S");
  append(!settings_.simple_mode, false, kAddGroup, IDI_TREE_FOLDER, group ? L"Добавить вложенную группу…" : L"Добавить группу…", L"Ctrl+Alt+G");
  separator();
  append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список", L"F5");

  SetForegroundWindow(window_);
  const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
  DestroyMenu(menu);
  ClearContextMenuItems();
  if (!command) return;
  if (command == kAddFile) AddFileDatabase(addParent);
  else if (command == kAddServer) AddServerDatabase(addParent);
  else if (command == kAddGroup) AddGroup(addParent);
  else SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MainWindow::DisplaySelected() {
  if (!details_) return;
  ListView_DeleteAllItems(details_);
  const auto name = SelectedName();
  const auto* entry = catalog_ ? catalog_->Find(name) : nullptr;
  if (!entry) {
    SetWindowTextW(details_title_, name.empty() ? L"Выберите базу или группу" : name.c_str());
    SetWindowTextW(details_subtitle_, name.empty() ? L"Сведения появятся здесь" : L"Служебный раздел списка");
    EnableWindow(enterprise_, FALSE); EnableWindow(designer_, FALSE); EnableWindow(edit_, FALSE);
    EnableWindow(cache_, FALSE); EnableWindow(shortcut_, FALSE); EnableWindow(remove_, FALSE);
    return;
  }

  const std::wstring type = entry->IsDatabase() ? ConnectionKind(entry->ValueOr(L"Connect")) : L"Группа списка баз";
  SetWindowTextW(details_title_, entry->name.c_str());
  const std::wstring subtitle = type + L"  •  Полей: " + std::to_wstring(entry->fields.size());
  SetWindowTextW(details_subtitle_, subtitle.c_str());
  const auto addRow = [&](std::wstring key, std::wstring value) {
    if (value.empty()) value = L"—";
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = ListView_GetItemCount(details_); item.pszText = key.data();
    const int index = ListView_InsertItem(details_, &item);
    if (index >= 0) ListView_SetItemText(details_, index, 1, value.data());
  };
  addRow(L"Тип", type);
  for (const auto& field : entry->fields) {
    auto value = SingleLine(field.value);
    if (_wcsicmp(field.key.c_str(), L"Folder") == 0 && (value.empty() || value == L"/")) value = L"Корневой уровень";
    addRow(FriendlyFieldName(field.key), std::move(value));
  }
  const bool database = entry->IsDatabase();
  EnableWindow(enterprise_, database); EnableWindow(designer_, database);
  EnableWindow(edit_, !settings_.simple_mode); EnableWindow(remove_, !settings_.simple_mode);
  EnableWindow(cache_, database && !settings_.simple_mode); EnableWindow(shortcut_, database && !settings_.simple_mode);
  InvalidateRect(details_, nullptr, TRUE);
}

void MainWindow::LaunchSelected(domain::LaunchMode mode) {
  if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите информационную базу."); return; }
  try { const auto database = catalog_->DatabaseFor(name); if (const auto webUrl = catalog::Catalog::WebUrl(database.connect)) { const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", webUrl->c_str(), nullptr, nullptr, SW_SHOWNORMAL)); if (result <= 32) throw std::runtime_error("Unable to open the web database URL."); return; }
    domain::LaunchOptions options;
    options.mode = mode;
    options.client_type = ClientTypeFromApplication(database.app);
    if (options.client_type == domain::ClientType::automatic) options.client_type = ClientTypeFromApplication(database.default_app);
    if (const auto fromParameters = launcher::AppArchitectureFromParameters(database.additional_parameters)) options.architecture = *fromParameters;
    else if (const auto fromDatabase = launcher::ParseAppArchitecture(database.app_arch)) options.architecture = *fromDatabase;
    const auto& selectedVersion = database.version.empty() ? database.default_version : database.version;
    if (selectedVersion != L"" && selectedVersion != L"Авто") options.version = selectedVersion;
    if (options.client_type == domain::ClientType::web) { Message(window_, L"Веб-клиент можно использовать только для веб-базы с адресом http:// или https://.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; }
    const auto selected = launcher::SelectPlatform(platforms_, options); if (!selected) { Message(window_, L"Подходящая платформа 1С не найдена. Проверьте установку и настройки поиска.", L"ИБ Старт", MB_OK | MB_ICONERROR); return; }
    const auto parameters = database.additional_parameters; if (utf::FindNoCaseOrdinal(parameters, L"/p") != std::wstring_view::npos && MessageBoxW(window_, L"В дополнительных параметрах обнаружен /P. Пароль может храниться в открытом виде в ibases.v8i. Продолжить?", L"Предупреждение", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    const auto command = launcher::BuildCommand(database, *selected, options); logger_.Info(L"Запуск: " + command.CommandLine()); launcher::Launch(command); storage::AppendHistory(layout_, {database.id, std::chrono::system_clock::now(), mode}); SetStatus(L"Запущена база: " + database.name);
  } catch (const std::exception& error) { logger_.Error(L"Ошибка запуска: " + ibstart::utf::FromUtf8(error.what())); Message(window_, L"Не удалось запустить базу. Подробности — в последнем логе.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

std::wstring MainWindow::NextName(std::wstring_view stem) const { for (unsigned number = 1;; ++number) { const auto candidate = std::wstring(stem) + L" " + std::to_wstring(number); if (!catalog_ || !catalog_->Find(candidate)) return candidate; } }
void MainWindow::AddFileDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  DatabaseEditorData initial;
  initial.name = NextName(L"Файловая база");
  initial.kind = DatabaseConnectionKind::file;
  auto entered = EditDatabase(window_, L"Добавление информационной базы", std::move(initial), platforms_);
  if (!entered) return;
  bool added = false;
  if (entered->kind == DatabaseConnectionKind::file) {
    added = catalog_->AddFileDatabase(entered->name, std::filesystem::path(ConnectionValue(entered->connect, L"File")), parent);
  } else {
    added = catalog_->AddServerDatabase(entered->name, entered->connect, parent);
  }
  if (!added) {
    const std::wstring message = entered->kind == DatabaseConnectionKind::file
        ? L"Не удалось добавить базу. Укажите уникальное имя, существующую группу и каталог, содержащий 1Cv8.1CD."
        : L"Не удалось добавить базу. Укажите уникальное имя, корректное подключение и существующую группу.";
    Message(window_, message, L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  if (auto* entry = catalog_->Find(entered->name)) {
    entered->id = entry->ValueOr(L"ID");
    entered->folder = entry->ValueOr(L"Folder");
    if (entered->order_in_list.empty()) entered->order_in_list = entry->ValueOr(L"OrderInList");
    if (entered->order_in_tree.empty()) entered->order_in_tree = entry->ValueOr(L"OrderInTree");
    ApplyDatabaseEditorData(*entry, *entered);
  }
  SaveCatalog(); PopulateTree(); SelectTreeItem(entered->name);
}
void MainWindow::AddServerDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  DatabaseEditorData initial;
  initial.name = NextName(L"Серверная база");
  initial.kind = DatabaseConnectionKind::server;
  auto entered = EditDatabase(window_, L"Добавление информационной базы", std::move(initial), platforms_);
  if (!entered) return;
  bool added = false;
  if (entered->kind == DatabaseConnectionKind::file) {
    added = catalog_->AddFileDatabase(entered->name, std::filesystem::path(ConnectionValue(entered->connect, L"File")), parent);
  } else {
    added = catalog_->AddServerDatabase(entered->name, entered->connect, parent);
  }
  if (!added) {
    const std::wstring message = entered->kind == DatabaseConnectionKind::file
        ? L"Не удалось добавить базу. Укажите уникальное имя, существующую группу и каталог, содержащий 1Cv8.1CD."
        : L"Не удалось добавить базу. Укажите уникальное имя, корректное подключение и существующую группу.";
    Message(window_, message, L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  if (auto* entry = catalog_->Find(entered->name)) {
    entered->id = entry->ValueOr(L"ID");
    entered->folder = entry->ValueOr(L"Folder");
    if (entered->order_in_list.empty()) entered->order_in_list = entry->ValueOr(L"OrderInList");
    if (entered->order_in_tree.empty()) entered->order_in_tree = entry->ValueOr(L"OrderInTree");
    ApplyDatabaseEditorData(*entry, *entered);
  }
  SaveCatalog(); PopulateTree(); SelectTreeItem(entered->name);
}
void MainWindow::AddGroup(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = InputBox(window_, L"Добавить группу", L"Название группы:", NextName(L"Новая группа"));
  if (!name) return;
  if (!catalog_->AddGroup(*name, parent)) {
    Message(window_, L"Имя группы уже используется или родительская группа недоступна.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  } else {
    SaveCatalog(); PopulateTree(); SelectTreeItem(*name);
  }
}
void MainWindow::EditSelected() {
  if (settings_.simple_mode || !catalog_) return;
  const auto selected = SelectedName();
  auto* entry = catalog_->Find(selected);
  if (!entry) return;
  if (!entry->IsDatabase()) {
    const auto changed = InputBox(window_, L"Изменить группу", L"Название группы:", entry->name);
    if (!changed || TrimText(*changed).empty()) return;
    if (!catalog_->RenameGroup(selected, TrimText(*changed))) {
      Message(window_, L"Имя группы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
      return;
    }
    SaveCatalog();
    PopulateTree();
    return;
  }
  const auto edited = EditDatabase(window_, L"Редактирование информационной базы", DatabaseEditorDataFromEntry(*entry), platforms_);
  if (!edited) return;
  if (selected != edited->name && !catalog_->RenameDatabase(selected, edited->name)) {
    Message(window_, L"Имя базы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  entry = catalog_->Find(edited->name);
  if (!entry) return;
  ApplyDatabaseEditorData(*entry, *edited);
  if (selected != edited->name) {
    auto favorites = storage::LoadFavorites(layout_);
    bool changed = false;
    for (auto& favorite : favorites) {
      if (EqualNoCase(favorite, selected)) { favorite = edited->name; changed = true; }
    }
    if (changed) storage::SaveFavorites(layout_, favorites);
  }
  SaveCatalog();
  PopulateTree();
  SelectTreeItem(edited->name);
}
void MainWindow::DeleteSelected() { if (settings_.simple_mode || !catalog_) return; const auto name = SelectedName(); if (name.empty()) return; if (MessageBoxW(window_, (L"Удалить «" + name + L"» только из списка баз? Каталог файловой базы не удаляется.").c_str(), L"ИБ Старт", MB_YESNO | MB_ICONWARNING) != IDYES) return; catalog_->Remove(name); SaveCatalog(); PopulateTree(); }
void MainWindow::MoveSelected(int offset) {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = SelectedName();
  if (!catalog_->MoveBy(name, offset)) {
    SetStatus(offset < 0 ? L"Элемент уже находится первым в группе." : L"Элемент уже находится последним в группе.");
    return;
  }
  SaveCatalog(); PopulateTree(); SelectTreeItem(name);
}
void MainWindow::ClearSelectedCache() { if (settings_.simple_mode || !catalog_) return; try { const auto database = catalog_->DatabaseFor(SelectedName()); const auto candidates = cache::CandidatesFor(database); if (candidates.empty()) { Message(window_, L"Безопасных каталогов кэша для этой базы не найдено."); return; } std::wstring list = L"Будут очищены только следующие каталоги кэша:\n"; for (const auto& item : candidates) list += item.path.wstring() + L"\n"; if (cache::HasActiveOneCProcess()) list += L"\nОбнаружен активный процесс 1С. Закройте его перед очисткой.\n"; if (MessageBoxW(window_, list.c_str(), L"Очистка кэша", MB_YESNO | MB_ICONWARNING) != IDYES) return; const auto result = cache::Clear(candidates); logger_.Info(L"Очистка кэша: файлов=" + std::to_wstring(result.files) + L", байт=" + std::to_wstring(result.bytes)); Message(window_, L"Очищено файлов: " + std::to_wstring(result.files) + L"\nОсвобождено байт: " + std::to_wstring(result.bytes)); } catch (...) { Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING); } }
void MainWindow::CreateShortcut() { if (!catalog_) return; try { const auto database = catalog_->DatabaseFor(SelectedName()); shell::CreateDesktopShortcut(executable_, database.id, database.name); Message(window_, L"Ярлык создан на рабочем столе."); } catch (...) { Message(window_, L"Не удалось создать ярлык.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }
void MainWindow::OpenList() { if (settings_.simple_mode) return; wchar_t filename[MAX_PATH]{}; OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_; dialog.lpstrFilter = L"ibases.v8i\0ibases.v8i\0Все файлы\0*.*\0"; dialog.lpstrFile = filename; dialog.nMaxFile = MAX_PATH; dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST; if (GetOpenFileNameW(&dialog)) { settings_.active_ibases = filename; storage::SaveSettings(layout_, settings_); LoadCatalog(); } }
void MainWindow::SetStatus(std::wstring text) { if (status_) SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); }
std::wstring MainWindow::CatalogStatistics() const { return L"Баз: " + std::to_wstring(catalog_ ? catalog_->Databases().size() : 0) + L" | Платформ: " + std::to_wstring(platforms_.size()); }
void MainWindow::SetSimpleMode(bool enabled) { settings_.simple_mode = enabled; const int visible = enabled ? SW_HIDE : SW_SHOW; if (edit_) ShowWindow(edit_, visible); if (cache_) ShowWindow(cache_, visible); if (shortcut_) ShowWindow(shortcut_, visible); if (remove_) ShowWindow(remove_, visible); HMENU menu = GetMenu(window_); if (menu) CheckMenuItem(menu, kSimpleMode, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED)); DisplaySelected(); }
void MainWindow::ToggleFavorite() { if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите базу для добавления в избранное."); return; } auto favorites = storage::LoadFavorites(layout_); const auto found = std::find(favorites.begin(), favorites.end(), name); if (found == favorites.end()) { favorites.insert(favorites.begin(), name); if (favorites.size() > 9) favorites.resize(9); SetStatus(L"Добавлено в избранное: " + name); } else { favorites.erase(found); SetStatus(L"Удалено из избранного: " + name); } storage::SaveFavorites(layout_, favorites); PopulateTree(); }
void MainWindow::LaunchFavorite(size_t slot) { auto favorites = storage::LoadFavorites(layout_); if (slot >= favorites.size()) { Message(window_, L"Этот слот избранного пока не назначен."); return; } SetWindowTextW(search_, L""); PopulateTree(); if (SelectTreeItem(favorites[slot])) LaunchSelected(domain::LaunchMode::enterprise); }
void MainWindow::ShowAbout() const { const std::wstring text = L"ИБ Старт (IBStart)\nВерсия " + std::wstring(version::value) + L"\n\nЛёгкий менеджер запусков информационных баз 1С:Предприятие.\n\nЛицензия MIT. IBStart не является официальным продуктом фирмы «1С»."; MessageBoxW(window_, text.c_str(), L"О программе — ИБ Старт", MB_OK | MB_ICONINFORMATION); }
void MainWindow::ReportUnhandledError(std::string_view message) noexcept { try { const auto wide = utf::FromUtf8(message); logger_.Error(L"Необработанная ошибка UI: " + wide); const auto text = L"Произошла непредвиденная ошибка. Подробности записаны в:\n" + logger_.path().wstring(); MessageBoxW(window_, text.c_str(), L"ИБ Старт", MB_OK | MB_ICONERROR); } catch (...) { MessageBoxW(window_, L"Произошла непредвиденная ошибка.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }

}  // namespace ibstart::ui
