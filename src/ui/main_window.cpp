#include "ui/main_window.hpp"

#include "app/resource.h"
#include "core/cache/cache_service.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/shell/shortcut.hpp"
#include "core/update/update_service.hpp"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace ibstart::ui {
namespace {
constexpr wchar_t kClassName[] = L"IBStart.MainWindow";
constexpr wchar_t kInputBoxClass[] = L"IBStart.InputBox";
constexpr wchar_t kDatabaseEditorClass[] = L"IBStart.DatabaseEditor";
constexpr wchar_t kAdvancedDatabaseOptionsClass[] = L"IBStart.AdvancedDatabaseOptions";
constexpr wchar_t kTagManagerClass[] = L"IBStart.TagManager";
constexpr wchar_t kTagAssignmentClass[] = L"IBStart.TagAssignment";
constexpr wchar_t kFolderPickerClass[] = L"IBStart.FolderPicker";
constexpr UINT kActivateMessage = WM_APP + 23;
constexpr UINT kUpdateCheckFinishedMessage = WM_APP + 24;
constexpr UINT kFocusShortcutSelectionMessage = WM_APP + 25;
constexpr ULONG_PTR kLaunchCopyData = 0x49425354;
constexpr int kMinimumWindowWidth = 940;
constexpr int kMinimumSimpleWindowWidth = 520;
constexpr int kMinimumWindowHeight = 460;
enum Command : int { kEnterprise = 100, kDesigner, kEdit, kCache, kShortcut, kDelete, kAddFile, kAddServer, kAddGroup, kOpenList, kRefresh, kSimpleMode, kToggleFavorite, kFocusSearch, kCheckForUpdates, kAbout, kMoveUp, kMoveDown, kOpenFolder, kClearRecent, kCopyDetailValue, kCopyDetailPair, kEditTags, kConfigureTagColors, kFolderSortDefault, kFolderSortCatalog, kFolderSortName, kFolderSortLastLaunch, kMoveToFolder, kOpenStandardList, kShowTagsInList, kNewTagForSelected, kFavorite1 = 200 };
constexpr UINT kRecentList1 = 300;
constexpr UINT kQuickTag1 = 400;
constexpr UINT kTagsContextMenu = 250;
constexpr UINT kRecentListsMenu = 299;
enum TreeImage : int { kFileDatabaseImage, kServerDatabaseImage, kWebDatabaseImage, kFolderImage, kFavoriteImage, kRecentImage };
constexpr LPARAM kRecentRootItemData = 1;
constexpr LPARAM kFavoritesRootItemData = 2;

void Message(HWND owner, std::wstring_view text, std::wstring_view title = L"ИБ Старт", UINT type = MB_OK | MB_ICONINFORMATION) { MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type); }
std::wstring WideErrorText(std::string_view message) noexcept {
  try { return utf::FromUtf8(message); }
  catch (...) { return std::wstring(message.begin(), message.end()); }
}
HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}
HICON CreateWebDatabaseIcon() {
  constexpr int size = 20;
  HDC screen = GetDC(nullptr);
  HDC color = screen ? CreateCompatibleDC(screen) : nullptr;
  HDC mask = screen ? CreateCompatibleDC(screen) : nullptr;
  HBITMAP colorBitmap = screen ? CreateCompatibleBitmap(screen, size, size) : nullptr;
  HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
  HICON icon{};
  if (color && mask && colorBitmap && maskBitmap) {
    const auto previousColor = SelectObject(color, colorBitmap);
    RECT bounds{0, 0, size, size};
    FillRect(color, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const HBRUSH globeBrush = CreateSolidBrush(RGB(225, 245, 255));
    const HPEN globePen = CreatePen(PS_SOLID, 2, RGB(0, 112, 156));
    const auto previousBrush = SelectObject(color, globeBrush);
    const auto previousPen = SelectObject(color, globePen);
    Ellipse(color, 2, 2, 18, 18);
    SelectObject(color, GetStockObject(HOLLOW_BRUSH));
    Ellipse(color, 6, 2, 14, 18);
    MoveToEx(color, 3, 10, nullptr);
    LineTo(color, 17, 10);
    MoveToEx(color, 5, 6, nullptr);
    LineTo(color, 15, 6);
    MoveToEx(color, 5, 14, nullptr);
    LineTo(color, 15, 14);
    SelectObject(color, previousBrush);
    SelectObject(color, previousPen);
    DeleteObject(globeBrush);
    DeleteObject(globePen);
    SelectObject(color, previousColor);

    const auto previousMask = SelectObject(mask, maskBitmap);
    PatBlt(mask, 0, 0, size, size, WHITENESS);
    const HBRUSH maskBrush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const HPEN maskPen = static_cast<HPEN>(GetStockObject(BLACK_PEN));
    const auto previousMaskBrush = SelectObject(mask, maskBrush);
    const auto previousMaskPen = SelectObject(mask, maskPen);
    Ellipse(mask, 1, 1, 19, 19);
    SelectObject(mask, previousMaskBrush);
    SelectObject(mask, previousMaskPen);
    SelectObject(mask, previousMask);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colorBitmap;
    info.hbmMask = maskBitmap;
    icon = CreateIconIndirect(&info);
  }
  if (color) DeleteDC(color);
  if (mask) DeleteDC(mask);
  if (colorBitmap) DeleteObject(colorBitmap);
  if (maskBitmap) DeleteObject(maskBitmap);
  if (screen) ReleaseDC(nullptr, screen);
  return icon;
}
HFONT CreateUiFont(HWND window, int points, LONG weight) {
  return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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
  if (!entry) return kServerDatabaseImage;
  const auto connect = entry->ValueOr(L"Connect");
  if (catalog::Catalog::IsWebConnection(connect)) return kWebDatabaseImage;
  if (utf::FindNoCaseOrdinal(connect, L"File=") != std::wstring_view::npos) return kFileDatabaseImage;
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
std::wstring TreeItemName(HWND tree, HTREEITEM item) {
  if (!tree || !item) return {};
  wchar_t text[512]{};
  TVITEMW data{}; data.mask = TVIF_TEXT; data.hItem = item; data.pszText = text; data.cchTextMax = 512;
  return TreeView_GetItem(tree, &data) ? text : L"";
}
LPARAM TreeItemData(HWND tree, HTREEITEM item) {
  if (!tree || !item) return 0;
  TVITEMW data{}; data.mask = TVIF_PARAM; data.hItem = item;
  return TreeView_GetItem(tree, &data) ? data.lParam : 0;
}
bool IsVirtualTreeBranch(HWND tree, HTREEITEM item) {
  for (auto current = item; current; current = TreeView_GetParent(tree, current)) if (TreeItemData(tree, current) != 0) return true;
  return false;
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
struct FileDatabasePassport {
  std::filesystem::path directory;
  std::filesystem::path database_file;
  std::optional<uintmax_t> size;
  std::wstring modified;
  bool network_path{false};
};
std::wstring FormatFileModificationTime(const FILETIME& value) {
  FILETIME local{};
  SYSTEMTIME time{};
  if (!FileTimeToLocalFileTime(&value, &local) || !FileTimeToSystemTime(&local, &time)) return {};
  wchar_t text[32]{};
  swprintf_s(text, L"%02u.%02u.%04u %02u:%02u", time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute);
  return text;
}
bool IsNetworkFilePath(const std::filesystem::path& path) {
  const std::wstring_view native = path.native();
  const bool extendedUnc = native.size() >= 8 && native.starts_with(L"\\\\?\\") &&
      _wcsnicmp(native.data() + 4, L"UNC\\", 4) == 0;
  if (extendedUnc || (native.starts_with(L"\\\\") && !native.starts_with(L"\\\\?\\")) || native.starts_with(L"//")) return true;
  const auto root = path.root_path();
  return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}
FileDatabasePassport ReadFileDatabasePassport(std::wstring_view connect) {
  FileDatabasePassport result;
  result.directory = ConnectionValue(connect, L"File");
  result.database_file = result.directory / L"1Cv8.1CD";
  if (result.directory.empty()) return result;
  result.network_path = IsNetworkFilePath(result.directory);
  if (result.network_path) return result;
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (!GetFileAttributesExW(result.database_file.c_str(), GetFileExInfoStandard, &attributes) ||
      attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return result;
  result.size = (static_cast<uintmax_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  result.modified = FormatFileModificationTime(attributes.ftLastWriteTime);
  return result;
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
  bool firstFragment = true;
  for (const auto& part : SplitConnection(original)) {
    // A legacy direct URL (https://host/base) becomes WS="…" above. It has no
    // key, so it must not be copied as an unknown fragment. Keep every later
    // fragment, including vendor-specific values such as Custom=keep.
    if (kind == DatabaseConnectionKind::web && firstFragment && catalog::IsBareWebConnection(part)) {
      firstFragment = false;
      continue;
    }
    firstFragment = false;
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
std::vector<std::wstring> ParseTags(std::wstring_view text) {
  std::vector<std::wstring> result;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find_first_of(L",;\r\n", start);
    const auto tag = TrimText(text.substr(start, end == std::wstring_view::npos ? text.size() - start : end - start));
    if (!tag.empty() && std::none_of(result.begin(), result.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) result.push_back(tag);
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return result;
}
std::wstring TagsText(const std::vector<std::wstring>& tags) {
  std::wstring result;
  for (const auto& tag : tags) {
    if (!result.empty()) result += L", ";
    result += tag;
  }
  return result;
}
std::wstring TagId(const domain::Entry& entry) { return entry.ValueOr(L"ID", entry.name); }
const std::vector<std::wstring>& TagsFor(const storage::DatabaseTags& tags, const domain::Entry& entry) {
  static const std::vector<std::wstring> empty;
  const auto found = tags.find(TagId(entry));
  return found == tags.end() ? empty : found->second;
}
const storage::TagStyle* TagStyleFor(const storage::TagStyles& styles, std::wstring_view tag) {
  if (const auto exact = styles.find(std::wstring(tag)); exact != styles.end()) return &exact->second;
  const auto found = std::find_if(styles.begin(), styles.end(), [&](const auto& item) { return EqualNoCase(item.first, tag); });
  return found == styles.end() ? nullptr : &found->second;
}
std::vector<std::wstring> KnownTags(const storage::DatabaseTags& tags, const storage::TagStyles& styles) {
  std::vector<std::wstring> result;
  const auto append = [&](std::wstring_view tag) {
    if (!tag.empty() && std::none_of(result.begin(), result.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) result.emplace_back(tag);
  };
  for (const auto& [_, values] : tags) for (const auto& tag : values) append(tag);
  for (const auto& [tag, _] : styles) append(tag);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
  return result;
}
std::wstring ColorText(COLORREF color) {
  wchar_t value[8]{};
  swprintf_s(value, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
  return value;
}
std::optional<COLORREF> ParseColorText(std::wstring_view value) {
  const std::wstring trimmed = TrimText(value);
  std::wstring_view text = trimmed;
  if (!text.empty() && text.front() == L'#') text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  const auto digit = [](wchar_t character) -> int {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
  };
  const auto byte = [&](size_t index) -> std::optional<BYTE> {
    const int high = digit(text[index]);
    const int low = digit(text[index + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    return static_cast<BYTE>(high * 16 + low);
  };
  const auto red = byte(0), green = byte(2), blue = byte(4);
  if (!red || !green || !blue) return std::nullopt;
  return RGB(*red, *green, *blue);
}
void EraseTagStyle(storage::TagStyles& styles, std::wstring_view name) {
  for (auto it = styles.begin(); it != styles.end();) {
    if (EqualNoCase(it->first, name)) it = styles.erase(it);
    else ++it;
  }
}
bool ContainsTag(const std::vector<std::wstring>& tags, std::wstring_view value) {
  return std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return EqualNoCase(tag, value); });
}

void RestoreModalOwner(HWND owner) {
  if (!owner || !IsWindow(owner)) return;
  EnableWindow(owner, TRUE);
  // The owner shares this UI thread; activation restores input without forcing the app to the foreground.
  SetActiveWindow(owner);
}

std::wstring ListViewText(HWND list, int row, int column) {
  std::wstring text(256, L'\0');
  for (;;) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = text.data();
    item.cchTextMax = static_cast<int>(text.size());
    const int copied = static_cast<int>(SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item)));
    if (copied < static_cast<int>(text.size()) - 1) {
      text.resize(std::max(0, copied));
      return text;
    }
    text.resize(text.size() * 2);
  }
}

bool CopyTextToClipboard(HWND owner, std::wstring_view text) {
  const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
  const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) return false;
  auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
  if (!destination) {
    GlobalFree(memory);
    return false;
  }
  std::copy(text.begin(), text.end(), destination);
  destination[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    return false;
  }
  struct ClipboardCloser {
    ~ClipboardCloser() { CloseClipboard(); }
  } closer;
  if (!EmptyClipboard()) {
    GlobalFree(memory);
    return false;
  }
  if (SetClipboardData(CF_UNICODETEXT, memory)) return true;
  GlobalFree(memory);
  return false;
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
  create(0, L"STATIC", L"Параметры командной строки:", 0, 28, 404, 184, 20, 0, textFont);
  state.parameters = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.additional_parameters, WS_TABSTOP | ES_AUTOHSCROLL, 220, 400, 352, 25, kAdvancedParameters, textFont);
  help(400, kAdvancedHelpParameters);
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, 430, 462, 110, 28, IDOK, buttonFont);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 462, 96, 28, IDCANCEL, buttonFont);
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
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
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
  RestoreModalOwner(owner);
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
  state.file = create(WS_EX_CLIENTEDGE, L"EDIT", ConnectionValue(state.initial.connect, L"File"), WS_TABSTOP | ES_AUTOHSCROLL, 48, 142, 498, 25, kFilePath, textFont);
  state.file_browse = create(0, L"BUTTON", L"Обзор…", WS_TABSTOP, 556, 142, 78, 25, kBrowseFilePath, buttonFont);
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
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON, 430, 532, 110, 28, IDOK, buttonFont);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 532, 96, 28, IDCANCEL, buttonFont);
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
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
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
  RestoreModalOwner(owner);
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
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND captionControl = CreateWindowW(L"STATIC", std::wstring(caption).c_str(), WS_CHILD | WS_VISIBLE, px(14), px(14), px(430), px(20), dialog, nullptr, nullptr, nullptr);
  state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::wstring(initial).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, px(14), px(38), px(430), px(24), dialog, nullptr, nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"ОК", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, px(258), px(78), px(96), px(25), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(364), px(78), px(96), px(25), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
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
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

enum TagManagerControl : int {
  kTagManagerList = 1700,
  kTagManagerName,
  kTagManagerBackground,
  kTagManagerText,
  kTagManagerBackgroundPalette,
  kTagManagerTextPalette,
  kTagManagerPreview,
  kTagManagerNew,
  kTagManagerSave,
  kTagManagerDelete
};

struct TagManagerResult {
  storage::DatabaseTags tags;
  storage::TagStyles styles;
};

struct TagManagerState {
  HWND list{};
  HWND name{};
  HWND background{};
  HWND text{};
  HWND preview{};
  HFONT font{};
  HFONT button_font{};
  std::array<COLORREF, 16> custom_colors{};
  storage::DatabaseTags tags;
  storage::TagStyles styles;
  std::wstring selected;
  std::optional<TagManagerResult> result;
  bool done{false};
};

std::wstring ListBoxText(HWND list, int index) {
  if (!list || index == LB_ERR) return {};
  const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
  if (length == LB_ERR) return {};
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  if (SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(value.data())) == LB_ERR) return {};
  value.resize(static_cast<size_t>(length));
  return value;
}

void UpdateTagManagerPreview(const TagManagerState& state);

void SetTagManagerFields(TagManagerState& state, std::wstring_view name) {
  state.selected = std::wstring(name);
  const auto* style = TagStyleFor(state.styles, name);
  const storage::TagStyle value = style ? *style : storage::TagStyle{};
  SetWindowTextW(state.name, std::wstring(name).c_str());
  SetWindowTextW(state.background, ColorText(value.background).c_str());
  SetWindowTextW(state.text, ColorText(value.text).c_str());
  UpdateTagManagerPreview(state);
}

void UpdateTagManagerPreview(const TagManagerState& state) {
  if (state.preview) InvalidateRect(state.preview, nullptr, TRUE);
}

void ChooseTagManagerColor(HWND dialog, TagManagerState& state, HWND field, COLORREF fallback) {
  CHOOSECOLORW choice{};
  choice.lStructSize = sizeof(choice);
  choice.hwndOwner = dialog;
  choice.rgbResult = ParseColorText(ReadControlText(field)).value_or(fallback);
  choice.lpCustColors = state.custom_colors.data();
  choice.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (!ChooseColorW(&choice)) return;
  SetWindowTextW(field, ColorText(choice.rgbResult).c_str());
  UpdateTagManagerPreview(state);
}

void DrawTagManagerPreview(const DRAWITEMSTRUCT& draw, const TagManagerState& state) {
  RECT preview = draw.rcItem;
  FillRect(draw.hDC, &preview, GetSysColorBrush(COLOR_WINDOW));
  const storage::TagStyle defaults{};
  const COLORREF background = ParseColorText(ReadControlText(state.background)).value_or(defaults.background);
  const COLORREF text = ParseColorText(ReadControlText(state.text)).value_or(defaults.text);
  std::wstring label = TrimText(ReadControlText(state.name));
  if (label.empty()) label = L"Название тега";

  const HFONT font = state.button_font ? state.button_font : state.font;
  const HGDIOBJ previousFont = font ? SelectObject(draw.hDC, font) : nullptr;
  SetBkMode(draw.hDC, TRANSPARENT);
  SIZE textSize{};
  GetTextExtentPoint32W(draw.hDC, label.c_str(), static_cast<int>(label.size()), &textSize);
  const int previewWidth = static_cast<int>(preview.right - preview.left);
  const int previewHeight = static_cast<int>(preview.bottom - preview.top);
  const int textWidth = static_cast<int>(textSize.cx);
  const int textHeight = static_cast<int>(textSize.cy);
  const int width = std::min(previewWidth - 16, std::max(88, textWidth + 22));
  const int height = std::min(previewHeight - 10, std::max(22, textHeight + 8));
  const int left = static_cast<int>(preview.left) + (previewWidth - width) / 2;
  const int top = static_cast<int>(preview.top) + (previewHeight - height) / 2;
  RECT tag{left, top, left + width, top + height};
  const HBRUSH brush = CreateSolidBrush(background);
  const HPEN pen = CreatePen(PS_SOLID, 1, background);
  const HGDIOBJ previousBrush = SelectObject(draw.hDC, brush);
  const HGDIOBJ previousPen = SelectObject(draw.hDC, pen);
  RoundRect(draw.hDC, tag.left, tag.top, tag.right, tag.bottom, height, height);
  SetTextColor(draw.hDC, text);
  DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()), &tag, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(draw.hDC, previousBrush);
  SelectObject(draw.hDC, previousPen);
  if (previousFont) SelectObject(draw.hDC, previousFont);
  DeleteObject(brush);
  DeleteObject(pen);
}

void RefreshTagManagerList(TagManagerState& state, std::wstring_view selected = {}) {
  const auto tags = KnownTags(state.tags, state.styles);
  SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
  int selection = LB_ERR;
  for (const auto& tag : tags) {
    const int index = static_cast<int>(SendMessageW(state.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tag.c_str())));
    if (selection == LB_ERR && EqualNoCase(tag, selected)) selection = index;
  }
  if (selection != LB_ERR) {
    SendMessageW(state.list, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    SetTagManagerFields(state, ListBoxText(state.list, selection));
  } else {
    SetTagManagerFields(state, L"");
  }
}

bool SaveTagManagerEntry(HWND dialog, TagManagerState& state) {
  const std::wstring name = TrimText(ReadControlText(state.name));
  const auto background = ParseColorText(ReadControlText(state.background));
  const auto text = ParseColorText(ReadControlText(state.text));
  if (name.empty()) {
    Message(dialog, L"Укажите название тега.", L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!background || !text) {
    Message(dialog, L"Цвета указываются в виде #RRGGBB, например #E2F2F4.", L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }
  const auto known = KnownTags(state.tags, state.styles);
  const auto existing = std::find_if(known.begin(), known.end(), [&](const auto& tag) { return EqualNoCase(tag, name); });
  if (existing != known.end() && (state.selected.empty() || !EqualNoCase(state.selected, name))) {
    Message(dialog, L"Тег с таким названием уже есть.", L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }

  if (!state.selected.empty() && !EqualNoCase(state.selected, name)) {
    for (auto& [_, values] : state.tags) {
      for (auto& tag : values) if (EqualNoCase(tag, state.selected)) tag = name;
    }
    EraseTagStyle(state.styles, state.selected);
  }
  EraseTagStyle(state.styles, name);
  state.styles[name] = {*background, *text};
  RefreshTagManagerList(state, name);
  return true;
}

void DeleteTagManagerEntry(HWND dialog, TagManagerState& state) {
  if (state.selected.empty()) return;
  const std::wstring message = L"Удалить тег «" + state.selected + L"» у всех баз и из настроек?";
  if (MessageBoxW(dialog, message.c_str(), L"Настройка тегов", MB_YESNO | MB_ICONWARNING) != IDYES) return;
  for (auto it = state.tags.begin(); it != state.tags.end();) {
    auto& values = it->second;
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& tag) { return EqualNoCase(tag, state.selected); }), values.end());
    if (values.empty()) it = state.tags.erase(it);
    else ++it;
  }
  EraseTagStyle(state.styles, state.selected);
  RefreshTagManagerList(state);
}

LRESULT CALLBACK TagManagerProc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<TagManagerState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_DRAWITEM && state) {
    const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (draw && draw->CtlID == kTagManagerPreview) {
      DrawTagManagerPreview(*draw, *state);
      return TRUE;
    }
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (command == kTagManagerList && HIWORD(wparam) == LBN_SELCHANGE) {
      const int selection = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
      SetTagManagerFields(*state, ListBoxText(state->list, selection));
      return 0;
    }
    if ((command == kTagManagerName || command == kTagManagerBackground || command == kTagManagerText) && notification == EN_CHANGE) {
      UpdateTagManagerPreview(*state);
      return 0;
    }
    if (command == kTagManagerBackgroundPalette) {
      ChooseTagManagerColor(wnd, *state, state->background, storage::TagStyle{}.background);
      return 0;
    }
    if (command == kTagManagerTextPalette) {
      ChooseTagManagerColor(wnd, *state, state->text, storage::TagStyle{}.text);
      return 0;
    }
    if (command == kTagManagerNew) {
      SendMessageW(state->list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
      SetTagManagerFields(*state, L"");
      SetFocus(state->name);
      return 0;
    }
    if (command == kTagManagerSave) {
      SaveTagManagerEntry(wnd, *state);
      return 0;
    }
    if (command == kTagManagerDelete) {
      DeleteTagManagerEntry(wnd, *state);
      return 0;
    }
    if (command == IDOK) {
      state->result = TagManagerResult{state->tags, state->styles};
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    DestroyWindow(wnd);
    return 0;
  }
  return DefWindowProcW(wnd, message, wparam, lparam);
}

std::optional<TagManagerResult> EditTagManager(HWND owner, const storage::DatabaseTags& tags, const storage::TagStyles& styles) {
  TagManagerState state;
  state.tags = tags;
  state.styles = styles;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kTagManagerClass;
    klass.lpfnWndProc = TagManagerProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 650, 322, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kTagManagerClass, L"Настройка тегов", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD exStyle, const wchar_t* className, std::wstring_view text, DWORD controlStyle, int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(exStyle, className, std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | controlStyle,
        px(x), px(y), px(width), px(height), dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SetControlFont(control, font);
    return control;
  };
  create(0, L"STATIC", L"Теги", 0, 10, 10, 230, 18, 0, state.font);
  state.list = create(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL, 10, 30, 230, 234, kTagManagerList, state.font);
  create(0, L"STATIC", L"Параметры тега", 0, 270, 10, 200, 18, 0, state.font);
  create(0, L"STATIC", L"Название", 0, 270, 32, 370, 18, 0, state.font);
  state.name = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 270, 51, 370, 25, kTagManagerName, state.font);
  create(0, L"STATIC", L"Цвет фона", 0, 270, 87, 190, 18, 0, state.font);
  state.background = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 270, 106, 190, 25, kTagManagerBackground, state.font);
  create(0, L"BUTTON", L"Выбрать…", WS_TABSTOP, 470, 105, 170, 27, kTagManagerBackgroundPalette, state.button_font ? state.button_font : state.font);
  create(0, L"STATIC", L"Цвет текста", 0, 270, 143, 190, 18, 0, state.font);
  state.text = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 270, 162, 190, 25, kTagManagerText, state.font);
  create(0, L"BUTTON", L"Выбрать…", WS_TABSTOP, 470, 161, 170, 27, kTagManagerTextPalette, state.button_font ? state.button_font : state.font);
  create(0, L"STATIC", L"Предпросмотр", 0, 270, 199, 370, 18, 0, state.font);
  state.preview = create(WS_EX_CLIENTEDGE, L"STATIC", L"", SS_OWNERDRAW, 270, 218, 370, 47, kTagManagerPreview, state.font);
  create(0, L"BUTTON", L"Новый", WS_TABSTOP, 10, 278, 108, 28, kTagManagerNew, state.button_font ? state.button_font : state.font);
  create(0, L"BUTTON", L"Удалить", WS_TABSTOP, 128, 278, 112, 28, kTagManagerDelete, state.button_font ? state.button_font : state.font);
  create(0, L"BUTTON", L"Сохранить тег", WS_TABSTOP, 270, 278, 140, 28, kTagManagerSave, state.button_font ? state.button_font : state.font);
  create(0, L"BUTTON", L"Готово", WS_TABSTOP | BS_DEFPUSHBUTTON, 448, 278, 92, 28, IDOK, state.button_font ? state.button_font : state.font);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 548, 278, 92, 28, IDCANCEL, state.button_font ? state.button_font : state.font);
  RefreshTagManagerList(state);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.list);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

enum TagAssignmentControl : int {
  kTagAssignmentList = 1750,
  kTagAssignmentName,
  kTagAssignmentAdd
};

struct TagAssignmentState {
  HWND list{};
  HWND name{};
  HFONT font{};
  HFONT button_font{};
  const storage::TagStyles* styles{};
  std::optional<std::vector<std::wstring>> result;
  bool done{false};
};

int AddTagAssignmentItem(TagAssignmentState& state, std::wstring_view tag, bool checked) {
  std::wstring value(tag);
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = ListView_GetItemCount(state.list);
  item.pszText = value.data();
  const int row = ListView_InsertItem(state.list, &item);
  if (row >= 0) ListView_SetCheckState(state.list, row, checked);
  return row;
}

bool AddTagAssignmentEntry(HWND dialog, TagAssignmentState& state) {
  const std::wstring name = TrimText(ReadControlText(state.name));
  if (name.empty()) {
    Message(dialog, L"Укажите название нового тега.", L"Теги базы", MB_OK | MB_ICONWARNING);
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
  const int row = AddTagAssignmentItem(state, name, true);
  if (row < 0) {
    Message(dialog, L"Не удалось добавить тег в список.", L"Теги базы", MB_OK | MB_ICONERROR);
    return false;
  }
  ListView_SetItemState(state.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(state.list, row, FALSE);
  SetWindowTextW(state.name, L"");
  return true;
}

std::vector<std::wstring> SelectedAssignmentTags(const TagAssignmentState& state) {
  std::vector<std::wstring> result;
  const int count = ListView_GetItemCount(state.list);
  for (int row = 0; row < count; ++row) {
    if (ListView_GetCheckState(state.list, row)) result.push_back(ListViewText(state.list, row, 0));
  }
  return result;
}

LRESULT CALLBACK TagAssignmentProc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<TagAssignmentState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header && header->idFrom == kTagAssignmentList && header->code == NM_CUSTOMDRAW) {
      auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
      if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const std::wstring tag = ListViewText(state->list, static_cast<int>(draw->nmcd.dwItemSpec), 0);
        const auto* configured = state->styles ? TagStyleFor(*state->styles, tag) : nullptr;
        const storage::TagStyle style = configured ? *configured : storage::TagStyle{};
        draw->clrTextBk = style.background;
        draw->clrText = style.text;
        return CDRF_DODEFAULT;
      }
    }
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command == kTagAssignmentAdd) {
      AddTagAssignmentEntry(wnd, *state);
      return 0;
    }
    if (command == IDOK) {
      state->result = SelectedAssignmentTags(*state);
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    DestroyWindow(wnd);
    return 0;
  }
  return DefWindowProcW(wnd, message, wparam, lparam);
}

std::optional<std::vector<std::wstring>> EditTagAssignment(HWND owner, const std::vector<std::wstring>& assigned,
    const storage::DatabaseTags& tags, const storage::TagStyles& styles) {
  TagAssignmentState state;
  state.styles = &styles;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kTagAssignmentClass;
    klass.lpfnWndProc = TagAssignmentProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 570, 358, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kTagAssignmentClass, L"Теги базы", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const HWND caption = CreateWindowW(L"STATIC", L"Отметьте существующие теги или быстро добавьте новый.", WS_CHILD | WS_VISIBLE,
      px(10), px(10), px(550), px(18), dialog, nullptr, nullptr, nullptr);
  state.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      px(10), px(31), px(550), px(219), dialog, reinterpret_cast<HMENU>(kTagAssignmentList), nullptr, nullptr);
  const HWND newCaption = CreateWindowW(L"STATIC", L"Новый тег", WS_CHILD | WS_VISIBLE,
      px(10), px(261), px(370), px(18), dialog, nullptr, nullptr, nullptr);
  state.name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
      px(10), px(280), px(370), px(25), dialog, reinterpret_cast<HMENU>(kTagAssignmentName), nullptr, nullptr);
  const HWND add = CreateWindowW(L"BUTTON", L"Добавить и отметить", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(390), px(279), px(170), px(27), dialog, reinterpret_cast<HMENU>(kTagAssignmentAdd), nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"Готово", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      px(370), px(320), px(90), px(28), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(470), px(320), px(90), px(28), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(caption, state.font);
  SetControlFont(newCaption, state.font);
  SetControlFont(state.list, state.font);
  SetControlFont(state.name, state.font);
  SetControlFont(add, state.button_font ? state.button_font : state.font);
  SetControlFont(accept, state.button_font ? state.button_font : state.font);
  SetControlFont(cancel, state.button_font ? state.button_font : state.font);
  ListView_SetExtendedListViewStyle(state.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW column{};
  column.mask = LVCF_WIDTH;
  column.cx = px(524);
  ListView_InsertColumn(state.list, 0, &column);
  for (const auto& tag : KnownTags(tags, styles)) AddTagAssignmentItem(state, tag, ContainsTag(assigned, tag));
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.list);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

enum FolderPickerControl : int { kFolderPickerTree = 1800 };

struct FolderPickerState {
  HWND tree{};
  HFONT font{};
  HFONT button_font{};
  std::vector<std::wstring> folders;
  std::optional<std::wstring> result;
  bool done{false};
};

HTREEITEM AddFolderPickerItem(FolderPickerState& state, const catalog::TreeItem& source, HTREEITEM parent, std::wstring_view selected) {
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
  for (const auto& child : source.children) AddFolderPickerItem(state, child, node, selected);
  TreeView_Expand(state.tree, node, TVE_EXPAND);
  if (EqualNoCase(source.name, selected)) TreeView_SelectItem(state.tree, node);
  return node;
}

LRESULT CALLBACK FolderPickerProc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<FolderPickerState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
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
      DestroyWindow(wnd);
      return 0;
    }
    if (LOWORD(wparam) == IDCANCEL) {
      state->done = true;
      DestroyWindow(wnd);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    DestroyWindow(wnd);
    return 0;
  }
  return DefWindowProcW(wnd, message, wparam, lparam);
}

std::optional<std::wstring> SelectCatalogFolder(HWND owner, const std::vector<catalog::TreeItem>& items, std::wstring_view initial) {
  FolderPickerState state;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kFolderPickerClass;
    klass.lpfnWndProc = FolderPickerProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 460, 400, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kFolderPickerClass, L"Переместить в папку", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
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
      px(10), px(32), px(440), px(315), dialog, reinterpret_cast<HMENU>(kFolderPickerTree), nullptr, nullptr);
  const HWND accept = CreateWindowW(L"BUTTON", L"Переместить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      px(224), px(360), px(120), px(28), dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  const HWND cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      px(354), px(360), px(96), px(28), dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  SetControlFont(caption, state.font);
  SetControlFont(state.tree, state.font);
  SetControlFont(accept, state.button_font ? state.button_font : state.font);
  SetControlFont(cancel, state.button_font ? state.button_font : state.font);
  state.folders.push_back(L"");
  std::wstring rootLabel = L"Корневой уровень";
  TVINSERTSTRUCTW root{};
  root.hParent = TVI_ROOT;
  root.hInsertAfter = TVI_LAST;
  root.item.mask = TVIF_TEXT | TVIF_PARAM;
  root.item.pszText = rootLabel.data();
  root.item.lParam = 0;
  const HTREEITEM rootItem = TreeView_InsertItem(state.tree, &root);
  if (initial.empty()) TreeView_SelectItem(state.tree, rootItem);
  for (const auto& item : items) AddFolderPickerItem(state, item, rootItem, initial);
  TreeView_Expand(state.tree, rootItem, TVE_EXPAND);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(state.tree);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace

struct MainWindow::UpdateCheckState {
  std::mutex mutex;
  std::optional<update::Release> release;
  std::wstring error;
  bool completed{false};
};

MainWindow::MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
    storage::Settings settings, std::optional<std::wstring> launch_id)
    : instance_(instance), executable_(std::move(executable)), layout_(std::move(layout)), settings_(std::move(settings)),
      logger_(layout_.root / L"logs"), initial_launch_id_(std::move(launch_id)) {}
MainWindow::~MainWindow() {
  CancelTreeDrag();
  if (window_ && IsWindow(window_)) {
    settings_.selected_entry = SelectedName();
    DestroyWindow(window_);
  }
  ClearContextMenuItems();
  ClearMainMenuItems();
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
  const int minimumWidth = settings_.simple_mode ? kMinimumSimpleWindowWidth : kMinimumWindowWidth;
  const int windowWidth = std::max(settings_.window_width, ScaleForDpi(minimumWidth, GetDpiForSystem()));
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
      {controlAlt, 'I', kToggleFavorite}, {controlAlt, 'M', kSimpleMode}, {controlShift, VK_DELETE, kCache}, {controlShift, 'S', kShortcut}, {controlShift, 'O', kOpenFolder},
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
        limits->ptMinTrackSize.x = ScaleForDpi(settings_.simple_mode ? kMinimumSimpleWindowWidth : kMinimumWindowWidth, dpi);
        limits->ptMinTrackSize.y = ScaleForDpi(kMinimumWindowHeight, dpi);
      }
      return 0;
    }
    case WM_SETFOCUS: SetFocus(search_); return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE && !dragging_name_.empty()) {
        CancelTreeDrag();
        if (GetCapture() == window_) ReleaseCapture();
        SetStatus(L"Перемещение отменено.");
        return 0;
      }
      if (wparam == VK_F3) { LaunchSelected(domain::LaunchMode::enterprise); return 0; }
      if (wparam == VK_F4) { LaunchSelected(domain::LaunchMode::designer); return 0; }
      break;
    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lparam) == search_ && HIWORD(wparam) == EN_CHANGE) {
        if (!suppress_search_refresh_) PopulateTree();
        return 0;
      }
      if (reinterpret_cast<HWND>(lparam) == connection_ && HIWORD(wparam) == EN_SETFOCUS) { SendMessageW(connection_, EM_SETSEL, 0, -1); return 0; }
      if (reinterpret_cast<HWND>(lparam) == tag_filter_ && HIWORD(wparam) == CBN_SELCHANGE) { PopulateTree(); return 0; }
      if (reinterpret_cast<HWND>(lparam) == sort_mode_ && HIWORD(wparam) == CBN_SELCHANGE) {
        const int selection = static_cast<int>(SendMessageW(sort_mode_, CB_GETCURSEL, 0, 0));
        if (selection >= static_cast<int>(storage::SortMode::catalog_order) && selection <= static_cast<int>(storage::SortMode::last_launch)) SetDefaultSortMode(static_cast<storage::SortMode>(selection));
        return 0;
      }
      switch (LOWORD(wparam)) {
        case kEnterprise: LaunchSelected(domain::LaunchMode::enterprise); break; case kDesigner: LaunchSelected(domain::LaunchMode::designer); break;
        case kAddFile: AddFileDatabase(); break; case kAddServer: AddServerDatabase(); break; case kAddGroup: AddGroup(); break; case kOpenList: OpenList(); break; case kOpenStandardList: OpenStandardList(); break;
        case kRefresh: {
          const std::wstring selected = SelectedName();
          LoadCatalog();
          if (!selected.empty()) SelectTreeItem(selected);
          break;
        }
        case kEdit: EditSelected(); break; case kCache: ClearSelectedCache(); break; case kClearRecent: ClearRecentBases(); break; case kShortcut: CreateShortcut(); break; case kOpenFolder: OpenSelectedFolder(); break; case kDelete: DeleteSelected(); break;
        case kCopyDetailValue: CopySelectedDetail(false); break; case kCopyDetailPair: CopySelectedDetail(true); break; case kEditTags: EditSelectedTags(); break; case kConfigureTagColors: ConfigureTagColors(); break;
        case kSimpleMode: SetSimpleMode(!settings_.simple_mode); break; case kToggleFavorite: ToggleFavorite(); break; case kShowTagsInList: ToggleTagDisplay(); break; case kFocusSearch: SetFocus(search_); break; case kCheckForUpdates: CheckForUpdates(); break; case kAbout: ShowAbout(); break;
        case kMoveUp: MoveSelected(-1); break; case kMoveDown: MoveSelected(1); break; case kMoveToFolder: MoveSelectedToFolder(); break; case kNewTagForSelected: AddNewTagToSelected(); break;
        default:
          if (LOWORD(wparam) >= kFavorite1 && LOWORD(wparam) < kFavorite1 + 9) LaunchFavorite(LOWORD(wparam) - kFavorite1);
          else if (LOWORD(wparam) >= kRecentList1 && LOWORD(wparam) < kRecentList1 + 10) OpenRecentList(LOWORD(wparam) - kRecentList1);
          break;
      } return 0;
    case WM_NOTIFY:
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == tree_) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) return DrawTreeSearchMatches(reinterpret_cast<NMTVCUSTOMDRAW*>(lparam));
        if (notification->code == TVN_SELCHANGEDW) { DisplaySelected(); return 0; }
        if (notification->code == TVN_GETINFOTIPW) {
          const auto* hint = reinterpret_cast<NMTVGETINFOTIPW*>(lparam);
          if (!settings_.simple_mode && hint && hint->pszText && hint->cchTextMax > 0 && catalog_ && TreeItemData(tree_, hint->hItem) == 0) {
            if (const auto* entry = catalog_->Find(TreeItemName(tree_, hint->hItem)); entry && entry->IsDatabase()) {
              const auto& tags = TagsFor(tags_, *entry);
              if (!tags.empty()) {
                const std::wstring text = L"Теги: " + TagsText(tags);
                wcsncpy_s(hint->pszText, static_cast<size_t>(hint->cchTextMax), text.c_str(), _TRUNCATE);
              }
            }
          }
          return 0;
        }
        if (notification->code == TVN_KEYDOWN) {
          const auto* key = reinterpret_cast<NMTVKEYDOWN*>(lparam);
          if (key->wVKey == VK_ESCAPE && !dragging_name_.empty()) {
            CancelTreeDrag();
            if (GetCapture() == window_) ReleaseCapture();
            SetStatus(L"Перемещение отменено.");
            return 0;
          }
        }
        if (notification->code == TVN_BEGINDRAGW) { const auto* drag = reinterpret_cast<NMTREEVIEWW*>(lparam); BeginTreeDrag(drag->itemNew.hItem, drag->ptDrag); return 0; }
      }
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == details_) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) return DrawDetailsList(reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
        if (notification->code == LVN_KEYDOWN) {
          const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lparam);
          if (key->wVKey == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            CopySelectedDetail(false);
            return 0;
          }
        }
      }
      break;
    case WM_CONTEXTMENU:
      if (reinterpret_cast<HWND>(wparam) == tree_) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ShowTreeContextMenu(screen);
        return 0;
      }
      if (reinterpret_cast<HWND>(wparam) == details_) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ShowDetailsContextMenu(screen);
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
    case WM_MOUSEMOVE:
      if (!dragging_name_.empty()) { UpdateTreeDrag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); return 0; }
      break;
    case WM_LBUTTONUP:
      if (!dragging_name_.empty()) { EndTreeDrag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); return 0; }
      break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
      if (!dragging_name_.empty()) CancelTreeDrag();
      break;
    case WM_COPYDATA: {
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      if (!data || data->dwData != kLaunchCopyData || !data->lpData || data->cbData < sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) return FALSE;
      const auto* value = static_cast<const wchar_t*>(data->lpData);
      const size_t length = data->cbData / sizeof(wchar_t);
      if (value[length - 1] != L'\0') return FALSE;
      initial_launch_id_ = std::wstring(value, length - 1);
      suppress_search_refresh_ = true;
      SetWindowTextW(search_, L"");
      suppress_search_refresh_ = false;
      PopulateTree();
      Activate();
      return TRUE;
    }
    case kActivateMessage: Activate(); return 0;
    case kUpdateCheckFinishedMessage: CompleteUpdateCheck(); return 0;
    case kFocusShortcutSelectionMessage:
      if (tree_) SetFocus(tree_);
      return 0;
    case WM_CLOSE:
      settings_.selected_entry = SelectedName();
      DestroyWindow(window);
      return 0;
    case WM_DESTROY: {
      WINDOWPLACEMENT placement{sizeof(placement)};
      if (GetWindowPlacement(window, &placement)) { const RECT& rect = placement.rcNormalPosition; settings_.window_x = rect.left; settings_.window_y = rect.top; settings_.window_width = rect.right - rect.left; settings_.window_height = rect.bottom - rect.top; }
      if (tree_ && IsWindow(tree_)) settings_.selected_entry = SelectedName();
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
  tag_filter_label_ = CreateWindowW(L"STATIC", L"Фильтр по тегу:", WS_CHILD | WS_VISIBLE, 8, 42, 106, 20, window_, nullptr, instance_, nullptr);
  tag_filter_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 116, 39, 258, 160, window_, nullptr, instance_, nullptr);
  SendMessageW(tag_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все базы"));
  SendMessageW(tag_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Избранные"));
  SendMessageW(tag_filter_, CB_SETCURSEL, 0, 0);
  sort_label_ = CreateWindowW(L"STATIC", L"Сортировка:", WS_CHILD | WS_VISIBLE, 388, 42, 86, 20, window_, nullptr, instance_, nullptr);
  sort_mode_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 476, 39, 230, 100, window_, nullptr, instance_, nullptr);
  SendMessageW(sort_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Исходный порядок"));
  SendMessageW(sort_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"По названию"));
  SendMessageW(sort_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"По последнему запуску"));
  SendMessageW(sort_mode_, CB_SETCURSEL, 0, 0);
  tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_INFOTIP, 8, 74, 360, 420, window_, nullptr, instance_, nullptr);
  TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  details_title_ = CreateWindowW(L"STATIC", L"Выберите базу или группу", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 49, 460, 26, window_, nullptr, instance_, nullptr);
  details_subtitle_ = CreateWindowW(L"STATIC", L"Сведения появятся здесь", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 76, 460, 20, window_, nullptr, instance_, nullptr);
  details_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 380, 100, 480, 182, window_, nullptr, instance_, nullptr);
  connection_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
      8, 0, 720, 22, window_, nullptr, instance_, nullptr);
  controls_font_ = CreateUiFont(window_, 9, FW_NORMAL);
  button_font_ = CreateUiFont(window_, 9, FW_NORMAL);
  if (controls_font_) {
    for (const HWND control : {searchLabel, search_, tag_filter_label_, tag_filter_, sort_label_, sort_mode_, tree_, details_, connection_}) {
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

  tree_images_ = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 6, 1);
  if (tree_images_) {
    bool complete = true;
    const auto appendTreeIcon = [&](HICON icon) {
      if (!icon || ImageList_AddIcon(tree_images_, icon) < 0) complete = false;
      if (icon) DestroyIcon(icon);
    };
    for (const int resource : {IDI_TREE_FILE_DATABASE, IDI_TREE_SERVER_DATABASE}) {
      appendTreeIcon(LoadResourceIcon(instance_, resource, 20));
    }
    HICON webIcon = CreateWebDatabaseIcon();
    if (!webIcon) webIcon = LoadResourceIcon(instance_, IDI_TREE_SERVER_DATABASE, 20);
    appendTreeIcon(webIcon);
    for (const int resource : {IDI_TREE_FOLDER, IDI_ACTION_FAVORITE, IDI_ACTION_REFRESH}) {
      appendTreeIcon(LoadResourceIcon(instance_, resource, 20));
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
    AddButtonTooltip(tooltips, connection_, L"Строку подключения можно выделить и скопировать — Ctrl+C");
  }
  AttachButtonIcon(enterprise_, instance_, IDI_ACTION_ENTERPRISE, button_images_);
  AttachButtonIcon(designer_, instance_, IDI_ACTION_DESIGNER, button_images_);
  AttachButtonIcon(edit_, instance_, IDI_ACTION_EDIT, button_images_);
  AttachButtonIcon(cache_, instance_, IDI_ACTION_CACHE, button_images_);
  AttachButtonIcon(shortcut_, instance_, IDI_ACTION_SHORTCUT, button_images_);
  AttachButtonIcon(remove_, instance_, IDI_ACTION_DELETE, button_images_);
  status_ = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  menu_ = CreateMenu();
  file_menu_ = CreatePopupMenu();
  view_menu_ = CreatePopupMenu();
  help_menu_ = CreatePopupMenu();
  RefreshFileMenu();
  RefreshMainMenuBar();
  SetSimpleMode(settings_.simple_mode);
  DisplaySelected();
  RECT client{};
  if (GetClientRect(window_, &client)) Layout(client.right - client.left, client.bottom - client.top);
}

void MainWindow::Layout(int width, int height) {
  if (settings_.simple_mode) {
    constexpr int footerHeight = 72;
    constexpr int footerPadding = 8;
    constexpr int buttonGap = 8;
    constexpr int buttonHeight = 30;
    const int footerTop = std::max(42, height - footerHeight);
    const int buttonWidth = std::max(1, (width - footerPadding * 2 - buttonGap) / 2);
    MoveWindow(search_, 58, 7, std::max(1, width - 66), 25, TRUE);
    MoveWindow(tree_, footerPadding, 42, std::max(1, width - footerPadding * 2), std::max(1, footerTop - 50), TRUE);
    MoveWindow(connection_, footerPadding, footerTop, std::max(1, width - footerPadding * 2), 22, TRUE);
    MoveWindow(enterprise_, footerPadding, footerTop + 28, buttonWidth, buttonHeight, TRUE);
    MoveWindow(designer_, footerPadding + buttonWidth + buttonGap, footerTop + 28, buttonWidth, buttonHeight, TRUE);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return;
  }

  constexpr int statusHeight = 22;
  constexpr int top = 74;
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
  const int connectionY = top + 58;
  const int detailsY = connectionY + 28;
  const int detailsHeight = std::max(42, buttonsY - detailsY - 10);
  const int keyWidth = std::clamp(rightWidth * 35 / 100, 80, 190);

  HDWP positions = BeginDeferWindowPos(14);
  const auto defer = [&positions](HWND control, int x, int y, int controlWidth, int controlHeight) {
    if (!positions || !control) return;
    positions = DeferWindowPos(positions, control, nullptr, x, y, std::max(1, controlWidth), std::max(1, controlHeight),
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  };
  defer(search_, 58, 7, width - 66, 25);
  defer(tag_filter_, 116, 39, 258, 25);
  defer(sort_mode_, 476, 39, std::max(1, std::min(260, width - 484)), 25);
  defer(tree_, 8, top, leftWidth, height - top - bottom);
  defer(details_title_, rightX + 10, top + 7, rightWidth - 20, 26);
  defer(details_subtitle_, rightX + 10, top + 34, rightWidth - 20, 20);
  defer(details_, rightX, detailsY, rightWidth, detailsHeight);
  defer(connection_, rightX, connectionY, rightWidth, 22);
  for (const auto& button : buttons) defer(button.window, rightX + button.x, buttonsY + button.y, button.width, buttonHeight);
  const bool positioned = positions && EndDeferWindowPos(positions) != FALSE;
  if (!positioned) {
    // DeferWindowPos can fail only under severe resource pressure.  Keep a
    // complete fallback layout instead of leaving controls at old positions.
    MoveWindow(search_, 58, 7, std::max(1, width - 66), 25, TRUE);
    MoveWindow(tag_filter_, 116, 39, 258, 25, TRUE);
    MoveWindow(sort_mode_, 476, 39, std::max(1, std::min(260, width - 484)), 25, TRUE);
    MoveWindow(tree_, 8, top, leftWidth, std::max(1, height - top - bottom), TRUE);
    MoveWindow(details_title_, rightX + 10, top + 7, std::max(1, rightWidth - 20), 26, TRUE);
    MoveWindow(details_subtitle_, rightX + 10, top + 34, std::max(1, rightWidth - 20), 20, TRUE);
    MoveWindow(details_, rightX, detailsY, rightWidth, detailsHeight, TRUE);
    MoveWindow(connection_, rightX, connectionY, rightWidth, 22, TRUE);
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
  const std::wstring selected = SelectedName();
  const bool hasInitialLaunch = initial_launch_id_.has_value();
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
    tags_ = storage::LoadTags(layout_);
    tag_styles_ = storage::LoadTagStyles(layout_);
    sort_settings_ = storage::LoadSortSettings(layout_);
    last_launches_ = storage::LoadLastLaunchTimes(layout_);
    RefreshTagFilter();
    RefreshSortControl();
    PopulateTree();
    if (!hasInitialLaunch) {
      const std::wstring& restore = selected.empty() ? settings_.selected_entry : selected;
      if (!restore.empty()) SelectTreeItem(restore);
    }
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
    store_->Save(catalog_->document());
    RememberRecentList(settings_.active_ibases);
    storage::SaveSettings(layout_, settings_);
    RefreshFileMenu();
    DrawMenuBar(window_);
    SetStatus(L"Сохранено: " + settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
  } catch (const v8i::ExternalModificationError&) { const int answer = MessageBoxW(window_, L"Файл ibases.v8i был изменён другой программой. Перечитать его?", L"ИБ Старт", MB_YESNO | MB_ICONWARNING); if (answer == IDYES) LoadCatalog(); }
  catch (const std::exception& error) { logger_.Error(L"Ошибка записи: " + ibstart::utf::FromUtf8(error.what())); Message(window_, L"Не удалось сохранить ibases.v8i. Исходный файл не изменён.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

bool MainWindow::ItemMatches(const catalog::TreeItem& item, std::wstring_view filter) const {
  if (filter.empty()) return true;
  if (catalog_) {
    if (const auto* entry = catalog_->Find(item.name); entry) {
      if (catalog::MatchesSearchText(*entry, filter)) return true;
      if (entry->IsDatabase()) {
        const auto& tags = TagsFor(tags_, *entry);
        if (std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return utf::FindNoCaseOrdinal(tag, filter) != std::wstring_view::npos; })) return true;
      }
    }
  }
  return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) { return ItemMatches(child, filter); });
}
bool MainWindow::ItemMatchesTagFilter(const catalog::TreeItem& item) const {
  if (settings_.simple_mode) return true;
  const int selection = tag_filter_ ? static_cast<int>(SendMessageW(tag_filter_, CB_GETCURSEL, 0, 0)) : 0;
  if (selection <= 0 || !catalog_) return true;
  const auto* entry = catalog_->Find(item.name);
  if (entry && entry->IsDatabase()) {
    if (selection == 1) return ContainsTag(filter_favorites_, entry->name);
    const size_t tag = static_cast<size_t>(selection - 2);
    return tag < filter_tags_.size() && ContainsTag(TagsFor(tags_, *entry), filter_tags_[tag]);
  }
  return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) { return ItemMatchesTagFilter(child); });
}
void MainWindow::RefreshTagFilter() {
  int selection = tag_filter_ ? static_cast<int>(SendMessageW(tag_filter_, CB_GETCURSEL, 0, 0)) : 0;
  const bool favoritesSelected = selection == 1;
  std::wstring selectedTag;
  if (selection >= 2 && static_cast<size_t>(selection - 2) < filter_tags_.size()) selectedTag = filter_tags_[selection - 2];

  filter_favorites_ = storage::LoadFavorites(layout_);
  filter_tags_.clear();
  if (catalog_) {
    for (const auto* entry : catalog_->Databases()) {
      for (const auto& tag : TagsFor(tags_, *entry)) {
        if (std::none_of(filter_tags_.begin(), filter_tags_.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) filter_tags_.push_back(tag);
      }
    }
  }
  std::sort(filter_tags_.begin(), filter_tags_.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
  if (!tag_filter_) return;

  SendMessageW(tag_filter_, CB_RESETCONTENT, 0, 0);
  SendMessageW(tag_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все базы"));
  SendMessageW(tag_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Избранные"));
  for (const auto& tag : filter_tags_) SendMessageW(tag_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tag.c_str()));
  selection = favoritesSelected ? 1 : 0;
  if (!selectedTag.empty()) {
    for (size_t index = 0; index < filter_tags_.size(); ++index) {
      if (EqualNoCase(filter_tags_[index], selectedTag)) { selection = static_cast<int>(index + 2); break; }
    }
  }
  SendMessageW(tag_filter_, CB_SETCURSEL, selection, 0);
}
void MainWindow::RefreshSortControl() {
  if (sort_mode_) SendMessageW(sort_mode_, CB_SETCURSEL, static_cast<WPARAM>(sort_settings_.default_mode), 0);
}
storage::SortMode MainWindow::SortModeForFolder(std::wstring_view folder) const {
  const auto found = sort_settings_.folder_modes.find(std::wstring(folder));
  return found == sort_settings_.folder_modes.end() ? sort_settings_.default_mode : found->second;
}
void MainWindow::SortTreeItems(std::vector<catalog::TreeItem>& items, std::wstring_view parent) const {
  for (auto& item : items) if (!item.database) SortTreeItems(item.children, item.name);
  const auto mode = SortModeForFolder(parent);
  if (mode == storage::SortMode::catalog_order) return;
  const auto nameLess = [](const catalog::TreeItem& left, const catalog::TreeItem& right) { return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0; };
  if (mode == storage::SortMode::name) {
    std::stable_sort(items.begin(), items.end(), nameLess);
    return;
  }
  const auto lastLaunch = [&](const catalog::TreeItem& item) -> std::optional<std::chrono::system_clock::time_point> {
    if (!item.database || !catalog_) return std::nullopt;
    const auto* entry = catalog_->Find(item.name);
    if (!entry) return std::nullopt;
    const auto found = last_launches_.find(entry->ValueOr(L"ID", entry->name));
    return found == last_launches_.end() ? std::nullopt : std::optional(found->second);
  };
  std::stable_sort(items.begin(), items.end(), [&](const auto& left, const auto& right) {
    if (left.database != right.database) return !left.database;
    if (!left.database) return nameLess(left, right);
    const auto leftTime = lastLaunch(left);
    const auto rightTime = lastLaunch(right);
    if (leftTime && rightTime && *leftTime != *rightTime) return *leftTime > *rightTime;
    if (leftTime.has_value() != rightTime.has_value()) return leftTime.has_value();
    return nameLess(left, right);
  });
}
std::vector<catalog::TreeItem> MainWindow::SortedTree() const {
  if (!catalog_) return {};
  auto items = catalog_->Tree();
  if (!settings_.simple_mode) SortTreeItems(items, L"");
  return items;
}
std::optional<size_t> MainWindow::CatalogPosition(std::wstring_view name, std::wstring_view parent) const {
  if (!catalog_) return std::nullopt;
  const auto find = [&](auto&& self, const std::vector<catalog::TreeItem>& items, std::wstring_view currentParent) -> std::optional<size_t> {
    if (EqualNoCase(currentParent, parent)) {
      for (size_t index = 0; index < items.size(); ++index) if (EqualNoCase(items[index].name, name)) return index;
      return std::nullopt;
    }
    for (const auto& item : items) if (!item.database) if (const auto position = self(self, item.children, item.name)) return position;
    return std::nullopt;
  };
  return find(find, catalog_->Tree(), L"");
}
void MainWindow::SetDefaultSortMode(storage::SortMode mode) {
  if (sort_settings_.default_mode == mode) return;
  const auto previous = sort_settings_;
  sort_settings_.default_mode = mode;
  try {
    storage::SaveSortSettings(layout_, sort_settings_);
  } catch (const std::exception& error) {
    sort_settings_ = previous;
    RefreshSortControl();
    logger_.Error(L"Ошибка сохранения сортировки: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить настройку сортировки.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  const auto selected = SelectedName();
  PopulateTree();
  SelectTreeItem(selected);
  SetStatus(L"Общая сортировка списка изменена.");
}
void MainWindow::SetFolderSortMode(std::wstring_view folder, std::optional<storage::SortMode> mode) {
  if (folder.empty()) return;
  const auto previous = sort_settings_;
  if (mode) sort_settings_.folder_modes[std::wstring(folder)] = *mode;
  else sort_settings_.folder_modes.erase(std::wstring(folder));
  try {
    storage::SaveSortSettings(layout_, sort_settings_);
  } catch (const std::exception& error) {
    sort_settings_ = previous;
    logger_.Error(L"Ошибка сохранения сортировки папки: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить сортировку папки.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  PopulateTree();
  SelectTreeItem(folder);
  SetStatus(L"Сортировка папки изменена.");
}
void MainWindow::AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter) {
  for (const auto& item : items) {
    if (!ItemMatches(item, filter) || !ItemMatchesTagFilter(item)) continue;
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
    const auto addSpecialRoot = [&](std::wstring_view rootName, const std::vector<std::wstring>& names, int image, LPARAM itemData = 0) {
      TVINSERTSTRUCTW root{}; root.hParent = TVI_ROOT; root.hInsertAfter = TVI_LAST;
      root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM; root.item.pszText = const_cast<wchar_t*>(rootName.data()); root.item.lParam = itemData;
      root.item.iImage = root.item.iSelectedImage = image;
      const HTREEITEM rootHandle = TreeView_InsertItem(tree_, &root);
      bool any = false;
      for (const auto& name : names) {
        const auto* entry = catalog_->Find(name);
        const catalog::TreeItem item{name, true, {}, {}};
        if (!entry || !entry->IsDatabase() || !ItemMatches(item, filter) || !ItemMatchesTagFilter(item)) continue;
        TVINSERTSTRUCTW row{}; row.hParent = rootHandle; row.hInsertAfter = TVI_LAST;
        row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE; row.item.pszText = const_cast<wchar_t*>(entry->name.c_str());
        row.item.iImage = row.item.iSelectedImage = DatabaseTreeImage(entry); TreeView_InsertItem(tree_, &row); any = true;
      }
      if (any) TreeView_Expand(tree_, rootHandle, TVE_EXPAND); else TreeView_DeleteItem(tree_, rootHandle);
    };
    if (!settings_.simple_mode) {
      addSpecialRoot(L"Избранное", storage::LoadFavorites(layout_), kFavoriteImage, kFavoritesRootItemData);
      std::vector<std::wstring> recent;
      for (const auto& history : storage::LoadHistory(layout_)) for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == history.database_id) { recent.push_back(entry->name); break; }
      addSpecialRoot(L"Недавние", recent, kRecentImage, kRecentRootItemData);
    }
    AddTreeItems(SortedTree(), TVI_ROOT, filter);
  }
  if (initial_launch_id_) {
    auto wanted = *initial_launch_id_; initial_launch_id_.reset();
    if (catalog_) for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == wanted) { wanted = entry->name; break; }
    if (SelectTreeItem(wanted)) {
      logger_.Info(L"Выбрана база по ярлыку: " + wanted);
      // During application startup the main window receives focus after WM_CREATE.
      // Posting this message makes the shortcut target the active tree row both for
      // a new instance and for an already running instance.
      PostMessageW(window_, kFocusShortcutSelectionMessage, 0, 0);
    } else {
      logger_.Error(L"Не найдена база для ярлыка с идентификатором: " + wanted);
    }
  }
  DisplaySelected();
}
LRESULT MainWindow::DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return (!settings_.simple_mode && settings_.show_tags_in_list) || !search_filter_.empty() ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
  if (draw->nmcd.dwDrawStage != CDDS_ITEMPOSTPAINT) return CDRF_DODEFAULT;

  const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
  wchar_t text[512]{};
  TVITEMW treeItem{}; treeItem.mask = TVIF_TEXT; treeItem.hItem = item; treeItem.pszText = text; treeItem.cchTextMax = 512;
  if (!TreeView_GetItem(tree_, &treeItem)) return CDRF_DODEFAULT;
  const std::wstring_view label(text);

  RECT labelRect{};
  if (!TreeView_GetItemRect(tree_, item, &labelRect, TRUE)) return CDRF_DODEFAULT;
  if (catalog_ && TreeItemData(tree_, item) == 0) {
    if (const auto* entry = catalog_->Find(label); entry && entry->IsDatabase()) {
      const auto& tags = TagsFor(tags_, *entry);
      const bool tagMatchesSearch = !search_filter_.empty() && std::any_of(tags.begin(), tags.end(), [&](const auto& tag) {
        return utf::FindNoCaseOrdinal(tag, search_filter_) != std::wstring_view::npos;
      });
      if (!tags.empty() && !settings_.simple_mode && (settings_.show_tags_in_list || tagMatchesSearch)) {
        RECT client{};
        GetClientRect(tree_, &client);
        const int saved = SaveDC(draw->nmcd.hdc);
        const HFONT font = controls_font_ ? controls_font_ : reinterpret_cast<HFONT>(SendMessageW(tree_, WM_GETFONT, 0, 0));
        HFONT boldFont{};
        LOGFONTW boldDescription{};
        if (font && GetObjectW(font, static_cast<int>(sizeof(boldDescription)), &boldDescription) == static_cast<int>(sizeof(boldDescription))) {
          boldDescription.lfWeight = FW_BOLD;
          boldFont = CreateFontIndirectW(&boldDescription);
        }
        if (font) SelectObject(draw->nmcd.hdc, font);
        SetBkMode(draw->nmcd.hdc, TRANSPARENT);
        int x = labelRect.right + 8;
        const int labelHeight = static_cast<int>(labelRect.bottom - labelRect.top);
        const int height = std::max(16, labelHeight - 2);
        const int y = static_cast<int>(labelRect.top) + (labelHeight - height) / 2;
        const auto measure = [&](std::wstring_view fragment, HFONT selectedFont) {
          SIZE size{};
          const HGDIOBJ previous = selectedFont ? SelectObject(draw->nmcd.hdc, selectedFont) : nullptr;
          GetTextExtentPoint32W(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &size);
          if (previous) SelectObject(draw->nmcd.hdc, previous);
          return size.cx;
        };
        const auto tagIsVisible = [&](const std::wstring& tag) {
          return settings_.show_tags_in_list || (!search_filter_.empty() && utf::FindNoCaseOrdinal(tag, search_filter_) != std::wstring_view::npos);
        };
        const int overflowWidth = measure(L"…", font) + 14;
        const auto drawOverflow = [&](int chipX) {
          const storage::TagStyle style{};
          const HBRUSH brush = CreateSolidBrush(style.background);
          const HPEN pen = CreatePen(PS_SOLID, 1, style.background);
          if (brush && pen) {
            const auto oldBrush = SelectObject(draw->nmcd.hdc, brush);
            const auto oldPen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, chipX, y, chipX + overflowWidth, y + height, height, height);
            SelectObject(draw->nmcd.hdc, oldBrush);
            SelectObject(draw->nmcd.hdc, oldPen);
            SetTextColor(draw->nmcd.hdc, style.text);
            RECT textRect{chipX + 7, y, chipX + overflowWidth - 7, y + height};
            DrawTextW(draw->nmcd.hdc, L"…", 1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
          }
          if (brush) DeleteObject(brush);
          if (pen) DeleteObject(pen);
        };
        for (size_t tagIndex = 0; tagIndex < tags.size(); ++tagIndex) {
          const auto& tag = tags[tagIndex];
          const bool matches = !search_filter_.empty() && utf::FindNoCaseOrdinal(tag, search_filter_) != std::wstring_view::npos;
          if (!tagIsVisible(tag)) continue;
          int textWidth = 0;
          if (matches) {
            size_t start = 0;
            size_t match = utf::FindNoCaseOrdinal(tag, search_filter_, start);
            while (match != std::wstring_view::npos) {
              textWidth += measure(std::wstring_view(tag).substr(start, match - start), font);
              textWidth += measure(std::wstring_view(tag).substr(match, search_filter_.size()), boldFont ? boldFont : font);
              start = match + search_filter_.size();
              match = utf::FindNoCaseOrdinal(tag, search_filter_, start);
            }
            textWidth += measure(std::wstring_view(tag).substr(start), font);
          } else {
            textWidth = measure(tag, font);
          }
          const int width = textWidth + 14;
          const bool hasMoreTags = std::any_of(tags.begin() + static_cast<std::ptrdiff_t>(tagIndex + 1), tags.end(), tagIsVisible);
          if (x + width + (hasMoreTags ? overflowWidth + 4 : 0) > client.right - 4) {
            if (x + overflowWidth <= client.right - 4) drawOverflow(x);
            break;
          }
          const auto* configured = TagStyleFor(tag_styles_, tag);
          const storage::TagStyle style = configured ? *configured : storage::TagStyle{};
          const HBRUSH brush = CreateSolidBrush(style.background);
          const HPEN pen = CreatePen(PS_SOLID, 1, style.background);
          if (brush && pen) {
            const auto oldBrush = SelectObject(draw->nmcd.hdc, brush);
            const auto oldPen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, x, y, x + width, y + height, height, height);
            SelectObject(draw->nmcd.hdc, oldBrush);
            SelectObject(draw->nmcd.hdc, oldPen);
            SetTextColor(draw->nmcd.hdc, style.text);
            int textX = x + 7;
            const auto drawSegment = [&](std::wstring_view fragment, HFONT selectedFont) {
              if (fragment.empty()) return;
              const HGDIOBJ previous = selectedFont ? SelectObject(draw->nmcd.hdc, selectedFont) : nullptr;
              SIZE size{};
              GetTextExtentPoint32W(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &size);
              RECT textRect{textX, y, textX + size.cx, y + height};
              DrawTextW(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
              textX += size.cx;
              if (previous) SelectObject(draw->nmcd.hdc, previous);
            };
            if (matches) {
              size_t start = 0;
              size_t match = utf::FindNoCaseOrdinal(tag, search_filter_, start);
              while (match != std::wstring_view::npos) {
                drawSegment(std::wstring_view(tag).substr(start, match - start), font);
                drawSegment(std::wstring_view(tag).substr(match, search_filter_.size()), boldFont ? boldFont : font);
                start = match + search_filter_.size();
                match = utf::FindNoCaseOrdinal(tag, search_filter_, start);
              }
              drawSegment(std::wstring_view(tag).substr(start), font);
            } else {
              drawSegment(tag, font);
            }
          }
          if (brush) DeleteObject(brush);
          if (pen) DeleteObject(pen);
          x += width + 4;
        }
        if (boldFont) DeleteObject(boldFont);
        RestoreDC(draw->nmcd.hdc, saved);
      }
    }
  }
  if (search_filter_.empty() || utf::FindNoCaseOrdinal(label, search_filter_) == std::wstring_view::npos) return CDRF_DODEFAULT;
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
  if (draw->iSubItem == 1 && details_ && EqualNoCase(ListViewText(details_, row, 0), L"Тег")) {
    if (const auto* style = TagStyleFor(tag_styles_, ListViewText(details_, row, 1))) {
      draw->clrTextBk = style->background;
      draw->clrText = style->text;
      return CDRF_DODEFAULT;
    }
  }
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
  const auto contains = [item](const auto& items) {
    return std::any_of(items.begin(), items.end(), [item](const auto& candidate) { return &candidate == item; });
  };
  if (!contains(context_menu_items_) && !contains(main_menu_items_) && !contains(file_menu_items_)) return false;

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
  const auto contains = [item](const auto& items) {
    return std::any_of(items.begin(), items.end(), [item](const auto& candidate) { return &candidate == item; });
  };
  if (!contains(context_menu_items_) && !contains(main_menu_items_) && !contains(file_menu_items_)) return false;

  const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
  const bool selected = (draw->itemState & ODS_SELECTED) != 0 && !disabled;
  const bool checked = (draw->itemState & ODS_CHECKED) != 0;
  const bool tagIcon = item->command == kTagsContextMenu || item->command == kEditTags || item->command == kConfigureTagColors || item->command == kShowTagsInList;
  const bool simpleModeIcon = item->command == kSimpleMode;
  const bool hasSubmenuArrow = item->command == kTagsContextMenu || item->command == kRecentListsMenu;
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
  } else if (simpleModeIcon) {
    const COLORREF outline = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(0, 144, 162);
    const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : RGB(231, 246, 248);
    const HBRUSH brush = CreateSolidBrush(background);
    const HPEN pen = CreatePen(PS_SOLID, 1, outline);
    const auto previousBrush = SelectObject(draw->hDC, brush);
    const auto previousPen = SelectObject(draw->hDC, pen);
    RoundRect(draw->hDC, iconX + 2, iconY + 3, iconX + 19, iconY + 17, 4, 4);
    SelectObject(draw->hDC, previousBrush);
    SelectObject(draw->hDC, previousPen);
    DeleteObject(brush);
    if (!disabled) {
      const auto previousLinePen = SelectObject(draw->hDC, pen);
      MoveToEx(draw->hDC, iconX + 6, iconY + 8, nullptr);
      LineTo(draw->hDC, iconX + 15, iconY + 8);
      MoveToEx(draw->hDC, iconX + 6, iconY + 12, nullptr);
      LineTo(draw->hDC, iconX + 12, iconY + 12);
      SelectObject(draw->hDC, previousLinePen);
    }
    DeleteObject(pen);
  } else if (tagIcon) {
    const COLORREF color = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(0, 144, 162);
    const HBRUSH brush = CreateSolidBrush(color);
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto previousBrush = SelectObject(draw->hDC, brush);
    const auto previousPen = SelectObject(draw->hDC, pen);
    POINT tag[] = {{iconX + 2, iconY + 3}, {iconX + 11, iconY + 3}, {iconX + 18, iconY + 10}, {iconX + 11, iconY + 17}, {iconX + 2, iconY + 17}};
    Polygon(draw->hDC, tag, 5);
    SelectObject(draw->hDC, previousBrush);
    SelectObject(draw->hDC, previousPen);
    const HBRUSH holeBrush = GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU);
    const HPEN holePen = CreatePen(PS_SOLID, 1, selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_MENU));
    const auto previousHoleBrush = SelectObject(draw->hDC, holeBrush);
    const auto previousHolePen = SelectObject(draw->hDC, holePen);
    Ellipse(draw->hDC, iconX + 5, iconY + 6, iconX + 9, iconY + 10);
    SelectObject(draw->hDC, previousHoleBrush);
    SelectObject(draw->hDC, previousHolePen);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteObject(holePen);
  } else if (item->icon) {
    if (disabled) DrawStateW(draw->hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(item->icon), 0, iconX, iconY, 20, 20, DST_ICON | DSS_DISABLED);
    else DrawIconEx(draw->hDC, iconX, iconY, item->icon, 20, 20, 0, nullptr, DI_NORMAL);
  }
  if (checked) {
    if (tagIcon) {
      const COLORREF border = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 103, 117);
      const HBRUSH badgeBrush = CreateSolidBrush(RGB(255, 255, 255));
      const HPEN badgePen = CreatePen(PS_SOLID, 1, border);
      const auto previousBrush = SelectObject(draw->hDC, badgeBrush);
      const auto previousPen = SelectObject(draw->hDC, badgePen);
      Ellipse(draw->hDC, iconX + 11, iconY + 10, iconX + 22, iconY + 21);
      SelectObject(draw->hDC, previousBrush);
      SelectObject(draw->hDC, previousPen);
      DeleteObject(badgeBrush);
      DeleteObject(badgePen);
    }
    const COLORREF color = tagIcon ? RGB(0, 103, 117) : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 103, 117);
    const HPEN pen = CreatePen(PS_SOLID, 2, color);
    const auto previousPen = SelectObject(draw->hDC, pen);
    POINT check[] = {{iconX + 4, iconY + 11}, {iconX + 8, iconY + 15}, {iconX + 17, iconY + 6}};
    if (tagIcon) {
      check[0] = {iconX + 13, iconY + 15};
      check[1] = {iconX + 16, iconY + 18};
      check[2] = {iconX + 20, iconY + 13};
    }
    Polyline(draw->hDC, check, 3);
    SelectObject(draw->hDC, previousPen);
    DeleteObject(pen);
  }
  const HFONT font = controls_font_ ? controls_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SelectObject(draw->hDC, font);
  SetBkMode(draw->hDC, TRANSPARENT);
  RECT textRect = draw->rcItem;
  textRect.left += 35;
  textRect.right -= 10;
  if (hasSubmenuArrow) textRect.right -= 16;
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
  if (hasSubmenuArrow) {
    const COLORREF arrowColor = disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    const HPEN pen = CreatePen(PS_SOLID, 1, arrowColor);
    const auto previousPen = SelectObject(draw->hDC, pen);
    const int centerY = (static_cast<int>(draw->rcItem.top) + static_cast<int>(draw->rcItem.bottom)) / 2;
    MoveToEx(draw->hDC, draw->rcItem.right - 15, centerY - 4, nullptr);
    LineTo(draw->hDC, draw->rcItem.right - 10, centerY);
    LineTo(draw->hDC, draw->rcItem.right - 15, centerY + 4);
    SelectObject(draw->hDC, previousPen);
    DeleteObject(pen);
  }
  if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
  RestoreDC(draw->hDC, saved);
  return true;
}

void MainWindow::ClearContextMenuItems() noexcept {
  for (const auto& item : context_menu_items_) if (item.icon) DestroyIcon(item.icon);
  context_menu_items_.clear();
}

void MainWindow::ClearMainMenuItems() noexcept {
  for (const auto& item : main_menu_items_) if (item.icon) DestroyIcon(item.icon);
  main_menu_items_.clear();
  for (const auto& item : file_menu_items_) if (item.icon) DestroyIcon(item.icon);
  file_menu_items_.clear();
}

std::wstring MainWindow::SelectedName() const { return TreeItemName(tree_, TreeView_GetSelection(tree_)); }
bool MainWindow::SelectedItemIsRecentRoot() const { return TreeItemData(tree_, TreeView_GetSelection(tree_)) == kRecentRootItemData; }
void MainWindow::BeginTreeDrag(HTREEITEM item, POINT treePoint) {
  if (!tree_ || !catalog_ || !item || TreeItemData(tree_, item) != 0) return;
  TreeView_SelectItem(tree_, item);
  dragging_name_ = SelectedName();
  if (dragging_name_.empty() || !catalog_->Find(dragging_name_)) { dragging_name_.clear(); return; }
  drag_image_ = TreeView_CreateDragImage(tree_, item);
  if (drag_image_) {
    if (ImageList_BeginDrag(drag_image_, 0, 0, 0)) ImageList_DragEnter(tree_, treePoint.x, treePoint.y);
    else { ImageList_Destroy(drag_image_); drag_image_ = nullptr; }
  }
  SetCapture(window_);
  MapWindowPoints(tree_, window_, &treePoint, 1);
  UpdateTreeDrag(treePoint);
}
void MainWindow::UpdateTreeDrag(POINT windowPoint) {
  if (dragging_name_.empty() || !catalog_ || !tree_) return;
  POINT treePoint = windowPoint;
  MapWindowPoints(window_, tree_, &treePoint, 1);
  if (drag_image_) ImageList_DragMove(treePoint.x, treePoint.y);

  const auto* dragged = catalog_->Find(dragging_name_);
  const bool manualSource = dragged && SortModeForFolder(catalog_->ParentOf(dragging_name_)) == storage::SortMode::catalog_order;
  std::wstring targetName;
  bool targetIsGroup = false;
  bool insertAfter = false;
  bool toRoot = false;
  HTREEITEM targetItem{};

  RECT bounds{};
  if (dragged && GetClientRect(tree_, &bounds) && PtInRect(&bounds, treePoint)) {
    TVHITTESTINFO hit{}; hit.pt = treePoint;
    TreeView_HitTest(tree_, &hit);
    if (!hit.hItem) {
      toRoot = manualSource && SortModeForFolder(L"") == storage::SortMode::catalog_order;
    } else if (!IsVirtualTreeBranch(tree_, hit.hItem)) {
      targetName = TreeItemName(tree_, hit.hItem);
      const auto* target = catalog_->Find(targetName);
      if (!target || EqualNoCase(target->name, dragged->name)) targetName.clear();
      else {
        targetIsGroup = target->IsGroup();
        const auto targetParent = targetIsGroup ? target->name : catalog_->ParentOf(target->name);
        bool cycle = false;
        if (dragged->IsGroup()) {
          std::wstring ancestor = targetParent;
          for (size_t depth = 0; !ancestor.empty() && depth != 1000; ++depth) {
            if (EqualNoCase(ancestor, dragged->name)) { cycle = true; break; }
            const auto next = catalog_->ParentOf(ancestor);
            if (next.empty() || EqualNoCase(next, ancestor)) break;
            ancestor = next;
          }
        }
        if (!manualSource || SortModeForFolder(targetParent) != storage::SortMode::catalog_order || cycle) targetName.clear();
        else {
          targetItem = hit.hItem;
          if (!targetIsGroup) {
            RECT itemBounds{};
            if (TreeView_GetItemRect(tree_, hit.hItem, &itemBounds, TRUE)) insertAfter = treePoint.y >= (itemBounds.top + itemBounds.bottom) / 2;
          }
        }
      }
    }
  }

  TreeView_SelectDropTarget(tree_, targetName.empty() || !targetIsGroup ? nullptr : targetItem);
  TreeView_SetInsertMark(tree_, targetName.empty() || targetIsGroup ? nullptr : targetItem, insertAfter);
  drag_target_name_ = std::move(targetName);
  drag_insert_after_ = insertAfter;
  drag_to_root_ = toRoot;
  if (drag_to_root_) SetStatus(L"Отпустите мышь, чтобы переместить в корень списка.");
  else if (!drag_target_name_.empty() && targetIsGroup) SetStatus(L"Отпустите мышь, чтобы переместить в группу: " + drag_target_name_);
  else if (!drag_target_name_.empty()) SetStatus(L"Отпустите мышь, чтобы вставить " + std::wstring(drag_insert_after_ ? L"после: " : L"перед: ") + drag_target_name_);
  else if (!manualSource) SetStatus(L"Перетаскивание доступно только при исходном порядке списка.");
  else SetStatus(L"Наведите указатель на базу, группу или свободное место дерева.");
}
void MainWindow::EndTreeDrag(POINT windowPoint) {
  if (dragging_name_.empty()) return;
  UpdateTreeDrag(windowPoint);
  const auto draggedName = std::move(dragging_name_);
  const auto targetName = std::move(drag_target_name_);
  const bool insertAfter = drag_insert_after_;
  const bool toRoot = drag_to_root_;
  CancelTreeDrag();
  if (GetCapture() == window_) ReleaseCapture();
  if (!catalog_ || (!toRoot && targetName.empty())) { SetStatus(L"Перемещение отменено."); return; }

  const auto* dragged = catalog_->Find(draggedName);
  if (!dragged) return;
  const auto sourceParent = catalog_->ParentOf(draggedName);
  if (SortModeForFolder(sourceParent) != storage::SortMode::catalog_order) { SetStatus(L"Перетаскивание доступно только при исходном порядке списка."); return; }

  std::wstring targetParent;
  size_t position = std::numeric_limits<size_t>::max();
  if (!toRoot) {
    const auto* target = catalog_->Find(targetName);
    if (!target || EqualNoCase(target->name, draggedName)) { SetStatus(L"Перемещение отменено."); return; }
    targetParent = target->IsGroup() ? target->name : catalog_->ParentOf(target->name);
    if (!target->IsGroup()) {
      const auto targetPosition = CatalogPosition(target->name, targetParent);
      if (!targetPosition) { SetStatus(L"Не удалось определить место вставки."); return; }
      position = *targetPosition + (insertAfter ? 1 : 0);
    }
  }
  if (SortModeForFolder(targetParent) != storage::SortMode::catalog_order) { SetStatus(L"Перетаскивание доступно только при исходном порядке списка."); return; }
  if (position != std::numeric_limits<size_t>::max() && EqualNoCase(sourceParent, targetParent)) {
    if (const auto sourcePosition = CatalogPosition(draggedName, sourceParent); sourcePosition && *sourcePosition < position) --position;
  }
  if (!catalog_->Move(draggedName, targetParent, position)) { SetStatus(L"Перемещение невозможно: нельзя поместить группу внутрь самой себя."); return; }
  SaveCatalog();
  PopulateTree();
  SelectTreeItem(draggedName);
  SetStatus(targetParent.empty() ? L"Элемент перемещён в корень списка." : L"Элемент перемещён: " + draggedName);
}
void MainWindow::CancelTreeDrag() {
  if (tree_ && IsWindow(tree_)) {
    TreeView_SelectDropTarget(tree_, nullptr);
    TreeView_SetInsertMark(tree_, nullptr, FALSE);
  }
  if (drag_image_) {
    if (tree_ && IsWindow(tree_)) ImageList_DragLeave(tree_);
    ImageList_EndDrag();
    ImageList_Destroy(drag_image_);
    drag_image_ = nullptr;
  }
  dragging_name_.clear();
  drag_target_name_.clear();
  drag_insert_after_ = false;
  drag_to_root_ = false;
}
void MainWindow::CopySelectedDetail(bool include_name) {
  const int row = details_ ? ListView_GetNextItem(details_, -1, LVNI_SELECTED) : -1;
  if (row < 0) {
    SetStatus(L"Выберите параметр для копирования.");
    return;
  }
  const auto name = ListViewText(details_, row, 0);
  const auto value = ListViewText(details_, row, 1);
  const auto text = include_name ? name + L": " + value : value;
  if (!CopyTextToClipboard(window_, text)) {
    SetStatus(L"Не удалось скопировать параметр в буфер обмена.");
    return;
  }
  SetStatus(include_name ? L"Скопирован параметр: " + name : L"Скопировано значение параметра: " + name);
}
void MainWindow::ShowDetailsContextMenu(POINT screen) {
  if (!details_) return;
  int row = ListView_GetNextItem(details_, -1, LVNI_SELECTED);
  if (screen.x == -1 && screen.y == -1) {
    if (row < 0) return;
    RECT bounds{};
    if (!ListView_GetItemRect(details_, row, &bounds, LVIR_BOUNDS)) return;
    screen = {bounds.left, bounds.bottom};
    ClientToScreen(details_, &screen);
  } else {
    POINT client = screen;
    ScreenToClient(details_, &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    row = ListView_HitTest(details_, &hit);
    if (row < 0) return;
    ListView_SetItemState(details_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(details_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetFocus(details_);
  }

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, kCopyDetailValue, L"Копировать значение\tCtrl+C");
  AppendMenuW(menu, MF_STRING, kCopyDetailPair, L"Копировать параметр и значение");
  const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
  DestroyMenu(menu);
  if (command == kCopyDetailValue) CopySelectedDetail(false);
  else if (command == kCopyDetailPair) CopySelectedDetail(true);
}
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
  const auto selectedItem = TreeView_GetSelection(tree_);
  const bool specialRoot = TreeItemData(tree_, selectedItem) != 0;
  const bool recentRoot = SelectedItemIsRecentRoot();
  const auto* entry = specialRoot ? nullptr : catalog_->Find(name);
  const bool database = entry && entry->IsDatabase();
  const bool group = entry && entry->IsGroup();
  const bool editable = entry && !settings_.simple_mode;
  const bool manualOrder = entry && SortModeForFolder(catalog_->ParentOf(entry->name)) == storage::SortMode::catalog_order;
  const bool file = database && !ConnectionValue(entry->ValueOr(L"Connect"), L"File").empty();
  const std::wstring addParent = group ? entry->name : entry ? catalog_->ParentOf(entry->name) : std::wstring();
  const auto favorites = storage::LoadFavorites(layout_);
  const bool favorite = std::find(favorites.begin(), favorites.end(), name) != favorites.end();

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  context_menu_items_.reserve(24);
  std::vector<std::wstring> quick_tags;
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
  const auto appendPopup = [&](HMENU submenu, UINT identity, std::wstring text) {
    ContextMenuItem visual{identity, nullptr, std::move(text), {}};
    context_menu_items_.push_back(std::move(visual));
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA | MIIM_SUBMENU;
    item.fType = MFT_OWNERDRAW;
    item.wID = identity;
    item.fState = MFS_ENABLED;
    item.hSubMenu = submenu;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(&context_menu_items_.back());
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
  };
  const auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
  if (settings_.simple_mode) {
    if (database) {
      append(true, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
      append(true, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
      separator();
      append(true, false, kEdit, IDI_ACTION_EDIT, L"Изменить…", L"F2");
      append(true, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
      separator();
      append(true, false, kMoveToFolder, IDI_TREE_FOLDER, L"Переместить в папку…");
      append(true, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
      append(true, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
    }
    if (!database) {
      DestroyMenu(menu);
      ClearContextMenuItems();
      return;
    }
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
    DestroyMenu(menu);
    ClearContextMenuItems();
    if (command) SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    return;
  }
  append(database, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
  append(database, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
  separator();
  append(database, favorite, kToggleFavorite, IDI_ACTION_FAVORITE, favorite ? L"Убрать из избранного" : L"Добавить в избранное", L"Ctrl+Alt+I");
  if (database && !settings_.simple_mode) {
    HMENU tagMenu = CreatePopupMenu();
    if (tagMenu) {
      AppendMenuW(tagMenu, MF_STRING, kEditTags, L"Управление тегами…");
      AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
      const auto& assigned = TagsFor(tags_, *entry);
      bool hasAvailableTags = false;
      for (const auto& tag : KnownTags(tags_, tag_styles_)) {
        if (ContainsTag(assigned, tag)) continue;
        const UINT command = kQuickTag1 + static_cast<UINT>(quick_tags.size());
        quick_tags.push_back(tag);
        AppendMenuW(tagMenu, MF_STRING, command, tag.c_str());
        hasAvailableTags = true;
      }
      if (!hasAvailableTags) AppendMenuW(tagMenu, MF_STRING | MF_GRAYED, 0, L"Нет доступных тегов для добавления");
      AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(tagMenu, MF_STRING, kNewTagForSelected, L"Новый тег…");
      AppendMenuW(tagMenu, MF_STRING, kConfigureTagColors, L"Настроить теги…");
      appendPopup(tagMenu, kTagsContextMenu, L"Теги");
    }
  }
  append(editable, false, kEdit, IDI_ACTION_EDIT, L"Изменить…", L"F2");
  append(database && !settings_.simple_mode, false, kCache, IDI_ACTION_CACHE, L"Очистить кэш…", L"Ctrl+Shift+Del");
  append(database && !settings_.simple_mode, false, kShortcut, IDI_ACTION_SHORTCUT, L"Создать ярлык", L"Ctrl+Shift+S");
  append(file, false, kOpenFolder, IDI_TREE_FOLDER, L"Открыть папку", L"Ctrl+Shift+O");
  append(recentRoot, false, kClearRecent, IDI_ACTION_DELETE, L"Очистить недавние базы…");
  separator();
  append(editable && manualOrder, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
  append(editable && manualOrder, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
  append(database && !settings_.simple_mode, false, kMoveToFolder, IDI_TREE_FOLDER, L"Переместить в папку…");
  append(editable, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
  if (group) {
    separator();
    HMENU sorting = CreatePopupMenu();
    if (sorting) {
      const auto configured = sort_settings_.folder_modes.find(entry->name);
      const auto checked = [&](std::optional<storage::SortMode> mode) {
        return mode ? configured != sort_settings_.folder_modes.end() && configured->second == *mode : configured == sort_settings_.folder_modes.end();
      };
      AppendMenuW(sorting, MF_STRING | (checked(std::nullopt) ? MF_CHECKED : MF_UNCHECKED), kFolderSortDefault, L"Как для всего списка");
      AppendMenuW(sorting, MF_STRING | (checked(storage::SortMode::catalog_order) ? MF_CHECKED : MF_UNCHECKED), kFolderSortCatalog, L"Исходный порядок");
      AppendMenuW(sorting, MF_STRING | (checked(storage::SortMode::name) ? MF_CHECKED : MF_UNCHECKED), kFolderSortName, L"По названию");
      AppendMenuW(sorting, MF_STRING | (checked(storage::SortMode::last_launch) ? MF_CHECKED : MF_UNCHECKED), kFolderSortLastLaunch, L"По последнему запуску");
      AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sorting), L"Сортировка в этой папке");
    }
  }
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
  const size_t quickTagIndex = command >= kQuickTag1 ? static_cast<size_t>(command - kQuickTag1) : quick_tags.size();
  if (quickTagIndex < quick_tags.size()) AddTagToSelected(quick_tags[quickTagIndex]);
  else if (command == kAddFile) AddFileDatabase(addParent);
  else if (command == kAddServer) AddServerDatabase(addParent);
  else if (command == kAddGroup) AddGroup(addParent);
  else if (group && command == kFolderSortDefault) SetFolderSortMode(entry->name, std::nullopt);
  else if (group && command == kFolderSortCatalog) SetFolderSortMode(entry->name, storage::SortMode::catalog_order);
  else if (group && command == kFolderSortName) SetFolderSortMode(entry->name, storage::SortMode::name);
  else if (group && command == kFolderSortLastLaunch) SetFolderSortMode(entry->name, storage::SortMode::last_launch);
  else SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MainWindow::DisplaySelected() {
  if (!details_) {
    UpdateConnection();
    return;
  }
  ListView_DeleteAllItems(details_);
  const auto name = SelectedName();
  const auto* entry = catalog_ && TreeItemData(tree_, TreeView_GetSelection(tree_)) == 0 ? catalog_->Find(name) : nullptr;
  if (!entry) {
    SetWindowTextW(details_title_, name.empty() ? L"Выберите базу или группу" : name.c_str());
    SetWindowTextW(details_subtitle_, name.empty() ? L"Сведения появятся здесь" : L"Служебный раздел списка");
    EnableWindow(enterprise_, FALSE); EnableWindow(designer_, FALSE); EnableWindow(edit_, FALSE);
    EnableWindow(cache_, FALSE); EnableWindow(shortcut_, FALSE); EnableWindow(remove_, FALSE);
    UpdateConnection();
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
  const auto addDivider = [&] {
    std::wstring empty;
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = ListView_GetItemCount(details_); item.pszText = empty.data();
    ListView_InsertItem(details_, &item);
  };
  addRow(L"Тип", type);
  for (const auto& field : entry->fields) {
    if (_wcsicmp(field.key.c_str(), L"Connect") == 0) continue;
    auto value = SingleLine(field.value);
    if (_wcsicmp(field.key.c_str(), L"Folder") == 0 && (value.empty() || value == L"/")) value = L"Корневой уровень";
    addRow(FriendlyFieldName(field.key), std::move(value));
  }
  if (entry->IsDatabase()) {
    const auto& tags = TagsFor(tags_, *entry);
    for (const auto& tag : tags) addRow(L"Тег", tag);
    const auto connect = entry->ValueOr(L"Connect");
    if (!ConnectionValue(connect, L"File").empty()) {
      const auto passport = ReadFileDatabasePassport(connect);
      addDivider();
      addRow(L"Каталог", passport.directory.wstring());
      addRow(L"Файл 1Cv8.1CD", passport.database_file.wstring());
      if (passport.network_path) {
        addRow(L"Состояние", L"Сетевая папка: сведения не загружаются");
      } else if (passport.size) {
        addRow(L"Размер 1Cv8.1CD", cache::FormatSize(*passport.size));
        addRow(L"Изменён", passport.modified);
      } else {
        addRow(L"Состояние", L"Файл не найден или недоступен");
      }
    } else {
      const auto server = ConnectionValue(connect, L"Srvr");
      const auto reference = ConnectionValue(connect, L"Ref");
      if (!server.empty() || !reference.empty()) {
        addDivider();
        addRow(L"Сервер 1С", server);
        addRow(L"Имя базы", reference);
      }
    }
  }
  const bool database = entry->IsDatabase();
  EnableWindow(enterprise_, database); EnableWindow(designer_, database);
  EnableWindow(edit_, !settings_.simple_mode); EnableWindow(remove_, !settings_.simple_mode);
  EnableWindow(cache_, database && !settings_.simple_mode); EnableWindow(shortcut_, database && !settings_.simple_mode);
  InvalidateRect(details_, nullptr, TRUE);
  UpdateConnection();
}

void MainWindow::LaunchSelected(domain::LaunchMode mode) {
  if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите информационную базу."); return; }
  try {
    const auto database = catalog_->DatabaseFor(name);
    const auto rememberLaunch = [&] {
      const auto timestamp = std::chrono::system_clock::now();
      storage::AppendHistory(layout_, {database.id, timestamp, mode});
      last_launches_[database.id] = timestamp;
      PopulateTree();
      SelectTreeItem(name);
    };
    if (const auto webUrl = catalog::Catalog::WebUrl(database.connect)) {
      const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", webUrl->c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      if (result <= 32) throw std::runtime_error("Unable to open the web database URL.");
      rememberLaunch();
      SetStatus(L"Запущена база: " + database.name);
      return;
    }
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
    const auto command = launcher::BuildCommand(database, *selected, options); logger_.Info(L"Запуск: " + command.CommandLine()); launcher::Launch(command); rememberLaunch(); SetStatus(L"Запущена база: " + database.name);
  } catch (const std::exception& error) { logger_.Error(L"Ошибка запуска: " + ibstart::utf::FromUtf8(error.what())); Message(window_, L"Не удалось запустить базу. Подробности — в последнем логе.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

std::wstring MainWindow::NextName(std::wstring_view stem) const { for (unsigned number = 1;; ++number) { const auto candidate = std::wstring(stem) + L" " + std::to_wstring(number); if (!catalog_ || !catalog_->Find(candidate)) return candidate; } }
void MainWindow::OpenSelectedFolder() {
  if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) return;
  const auto folder = ConnectionValue(entry->ValueOr(L"Connect"), L"File");
  if (folder.empty()) return;
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error) || error) { Message(window_, L"Каталог файловой базы не найден.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) Message(window_, L"Не удалось открыть каталог базы.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
}
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
  if (!catalog_) return;
  const auto selected = SelectedName();
  auto* entry = catalog_->Find(selected);
  if (!entry) return;
  if (settings_.simple_mode && !entry->IsDatabase()) return;
  if (!entry->IsDatabase()) {
    const auto changed = InputBox(window_, L"Изменить группу", L"Название группы:", entry->name);
    if (!changed || TrimText(*changed).empty()) return;
    if (!catalog_->RenameGroup(selected, TrimText(*changed))) {
      Message(window_, L"Имя группы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
      return;
    }
    const auto renamed = TrimText(*changed);
    if (const auto configured = sort_settings_.folder_modes.find(selected); configured != sort_settings_.folder_modes.end()) {
      const auto previousSorting = sort_settings_;
      sort_settings_.folder_modes[renamed] = configured->second;
      sort_settings_.folder_modes.erase(selected);
      try {
        storage::SaveSortSettings(layout_, sort_settings_);
      } catch (const std::exception& error) {
        sort_settings_ = previousSorting;
        logger_.Error(L"Ошибка переноса сортировки при переименовании группы: " + ibstart::utf::FromUtf8(error.what()));
        Message(window_, L"Группа переименована, но её настройку сортировки не удалось сохранить.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
      }
    }
    SaveCatalog();
    PopulateTree();
    SelectTreeItem(renamed);
    return;
  }
  const auto previousTagId = TagId(*entry);
  const auto previousTags = tags_;
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
    if (previousTagId != TagId(*entry)) {
      if (const auto tags = tags_.find(previousTagId); tags != tags_.end()) {
        tags_[TagId(*entry)] = std::move(tags->second);
        tags_.erase(tags);
        try {
          storage::SaveTags(layout_, tags_);
        } catch (const std::exception& error) {
          tags_ = previousTags;
          logger_.Error(L"Ошибка переноса тегов при переименовании: " + ibstart::utf::FromUtf8(error.what()));
          Message(window_, L"Не удалось сохранить теги после переименования базы.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
        }
      }
    }
  }
  SaveCatalog();
  RefreshTagFilter();
  PopulateTree();
  SelectTreeItem(edited->name);
}
void MainWindow::EditSelectedTags() {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (!entry || !entry->IsDatabase()) {
    Message(window_, L"Выберите информационную базу для изменения тегов.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  const auto edited = EditTagAssignment(window_, TagsFor(tags_, *entry), tags_, tag_styles_);
  if (!edited) return;

  const auto previous = tags_;
  const auto& values = *edited;
  if (values.empty()) tags_.erase(TagId(*entry));
  else tags_[TagId(*entry)] = values;
  try {
    storage::SaveTags(layout_, tags_);
  } catch (const std::exception& error) {
    tags_ = previous;
    logger_.Error(L"Ошибка сохранения тегов: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить теги базы.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  RefreshTagFilter();
  PopulateTree();
  SelectTreeItem(name);
  SetStatus(values.empty() ? L"Теги базы очищены." : L"Теги базы сохранены: " + TagsText(values));
}
void MainWindow::ConfigureTagColors() {
  const auto updated = EditTagManager(window_, tags_, tag_styles_);
  if (!updated) return;
  const auto previousTags = tags_;
  const auto previousStyles = tag_styles_;
  tags_ = updated->tags;
  tag_styles_ = updated->styles;
  try {
    storage::SaveTagsAndStyles(layout_, tags_, tag_styles_);
  } catch (const std::exception& error) {
    tags_ = previousTags;
    tag_styles_ = previousStyles;
    logger_.Error(L"Ошибка сохранения настроек тегов: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить настройки тегов.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  RefreshTagFilter();
  PopulateTree();
  SetStatus(L"Настройки тегов сохранены.");
}
void MainWindow::AddTagToSelected(std::wstring tag) {
  if (settings_.simple_mode || !catalog_) return;
  tag = TrimText(tag);
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (tag.empty() || !entry || !entry->IsDatabase()) return;
  const auto previousTags = tags_;
  auto values = TagsFor(tags_, *entry);
  if (ContainsTag(values, tag)) {
    SetStatus(L"У базы уже есть тег «" + tag + L"».");
    return;
  }
  values.push_back(std::move(tag));
  tags_[TagId(*entry)] = std::move(values);
  try {
    storage::SaveTags(layout_, tags_);
  } catch (const std::exception& error) {
    tags_ = previousTags;
    logger_.Error(L"Ошибка добавления тега: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось добавить тег базе.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  RefreshTagFilter();
  PopulateTree();
  SelectTreeItem(name);
  SetStatus(L"Тег добавлен: " + TagsText(tags_.at(TagId(*entry))));
}
void MainWindow::AddNewTagToSelected() {
  const auto entered = InputBox(window_, L"Новый тег", L"Название тега:", L"");
  if (!entered) return;
  const auto requested = TrimText(*entered);
  if (requested.empty()) return;
  const auto known = KnownTags(tags_, tag_styles_);
  const auto found = std::find_if(known.begin(), known.end(), [&](const auto& tag) { return EqualNoCase(tag, requested); });
  AddTagToSelected(found == known.end() ? requested : *found);
}
void MainWindow::DeleteSelected() {
  if (!catalog_) return;
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (!entry) return;
  if (settings_.simple_mode && !entry->IsDatabase()) return;
  const auto tagId = entry->IsDatabase() ? TagId(*entry) : std::wstring();
  const bool group = entry->IsGroup();
  const auto item = entry->IsDatabase() ? L"информационную базу" : L"группу";
  const auto message = L"Удалить " + std::wstring(item) + L" \"" + name + L"\" из списка.";
  if (MessageBoxW(window_, message.c_str(), L"ИБ Старт", MB_YESNO) != IDYES) return;
  if (!catalog_->Remove(name)) return;
  if (!tagId.empty() && tags_.contains(tagId)) {
    const auto previousTags = tags_;
    tags_.erase(tagId);
    try {
      storage::SaveTags(layout_, tags_);
    } catch (const std::exception& error) {
      tags_ = previousTags;
      logger_.Error(L"Ошибка удаления тегов: " + ibstart::utf::FromUtf8(error.what()));
      Message(window_, L"База удалена из списка, но её теги не удалось удалить.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    }
  }
  if (group && sort_settings_.folder_modes.contains(name)) {
    const auto previousSorting = sort_settings_;
    sort_settings_.folder_modes.erase(name);
    try {
      storage::SaveSortSettings(layout_, sort_settings_);
    } catch (const std::exception& error) {
      sort_settings_ = previousSorting;
      logger_.Error(L"Ошибка удаления сортировки папки: " + ibstart::utf::FromUtf8(error.what()));
      Message(window_, L"Папка удалена, но её настройку сортировки не удалось удалить.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    }
  }
  SaveCatalog();
  RefreshTagFilter();
  PopulateTree();
}
void MainWindow::MoveSelected(int offset) {
  if (!catalog_) return;
  const auto name = SelectedName();
  if (!settings_.simple_mode) {
    if (const auto* entry = catalog_->Find(name); entry && SortModeForFolder(catalog_->ParentOf(entry->name)) != storage::SortMode::catalog_order) {
      SetStatus(L"Перестановка доступна только при исходном порядке списка.");
      return;
    }
  }
  if (!catalog_->MoveBy(name, offset)) {
    SetStatus(offset < 0 ? L"Элемент уже находится первым в группе." : L"Элемент уже находится последним в группе.");
    return;
  }
  SaveCatalog(); PopulateTree(); SelectTreeItem(name);
}
void MainWindow::MoveSelectedToFolder() {
  if (!catalog_) return;
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (!entry || !entry->IsDatabase()) {
    Message(window_, L"Выберите информационную базу для перемещения.", L"Перемещение базы", MB_OK | MB_ICONWARNING);
    return;
  }
  const auto current = catalog_->ParentOf(entry->name);
  const auto target = SelectCatalogFolder(window_, SortedTree(), current);
  if (!target) return;
  if (EqualNoCase(*target, current)) {
    SetStatus(L"База уже находится в выбранной папке.");
    return;
  }
  if (!catalog_->Move(name, *target, std::numeric_limits<size_t>::max())) {
    Message(window_, L"Не удалось переместить базу в выбранную папку.", L"Перемещение базы", MB_OK | MB_ICONWARNING);
    return;
  }
  SaveCatalog();
  PopulateTree();
  SelectTreeItem(name);
  SetStatus(target->empty() ? L"База перемещена в корневой уровень." : L"База перемещена в папку: " + *target);
}
void MainWindow::ClearSelectedCache() {
  if (settings_.simple_mode || !catalog_) return;
  try {
    const auto database = catalog_->DatabaseFor(SelectedName());
    const auto candidates = cache::CandidatesFor(database);
    if (candidates.empty()) {
      Message(window_, L"Безопасных каталогов кэша для этой базы не найдено.");
      return;
    }

    uintmax_t totalBytes = 0;
    std::wstring list = L"Будут очищены только следующие каталоги кэша:\n";
    for (const auto& item : candidates) {
      totalBytes += item.bytes;
      list += item.path.wstring() + L" — " + cache::FormatSize(item.bytes) + L"\n";
    }
    list += L"\nПримерный объём для очистки: " + cache::FormatSize(totalBytes) + L".\n";
    if (cache::HasActiveOneCProcess()) list += L"\nОбнаружен активный процесс 1С. Закройте его перед очисткой.\n";
    if (MessageBoxW(window_, list.c_str(), L"Очистка кэша", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    const auto result = cache::Clear(candidates);
    const auto size = cache::FormatSize(result.bytes);
    logger_.Info(L"Очистка кэша: файлов=" + std::to_wstring(result.files) + L", байт=" + std::to_wstring(result.bytes) + L" (" + size + L")");
    Message(window_, L"Очищено файлов: " + std::to_wstring(result.files) + L"\nОсвобождено: " + size);
  } catch (...) {
    Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  }
}
void MainWindow::ClearRecentBases() {
  try {
    if (storage::LoadHistory(layout_).empty()) { SetStatus(L"Список недавних баз уже пуст."); return; }
    if (MessageBoxW(window_, L"Очистить список недавних баз?\n\nСами базы и избранное не будут затронуты.", L"Очистить недавние базы", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    storage::ClearHistory(layout_);
    logger_.Info(L"Очищен список недавних баз.");
    PopulateTree();
    SetStatus(L"Список недавних баз очищен.");
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка очистки недавних баз: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось очистить список недавних баз.", L"ИБ Старт", MB_OK | MB_ICONERROR);
  }
}
void MainWindow::CreateShortcut() { if (!catalog_) return; try { const auto database = catalog_->DatabaseFor(SelectedName()); shell::CreateDesktopShortcut(executable_, database.id, database.name); Message(window_, L"Ярлык создан на рабочем столе."); } catch (...) { Message(window_, L"Не удалось создать ярлык.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }
void MainWindow::RefreshFileMenu() {
  if (!file_menu_) return;
  while (GetMenuItemCount(file_menu_) > 0) {
    const HMENU submenu = GetSubMenu(file_menu_, 0);
    RemoveMenu(file_menu_, 0, MF_BYPOSITION);
    if (submenu) DestroyMenu(submenu);
  }
  for (const auto& item : file_menu_items_) if (item.icon) DestroyIcon(item.icon);
  file_menu_items_.clear();
  file_menu_items_.reserve(16);
  const auto append = [&](bool enabled, bool checked, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}) {
    ContextMenuItem visual{command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20), std::move(text), std::move(shortcut)};
    file_menu_items_.push_back(std::move(visual));
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW;
    item.wID = command;
    item.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
    item.dwItemData = reinterpret_cast<ULONG_PTR>(&file_menu_items_.back());
    InsertMenuItemW(file_menu_, static_cast<UINT>(GetMenuItemCount(file_menu_)), TRUE, &item);
  };
  const auto appendPopup = [&](HMENU submenu, UINT identity, int iconResource, std::wstring text) {
    ContextMenuItem visual{identity, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20), std::move(text), {}};
    file_menu_items_.push_back(std::move(visual));
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA | MIIM_SUBMENU;
    item.fType = MFT_OWNERDRAW;
    item.wID = identity;
    item.fState = MFS_ENABLED;
    item.hSubMenu = submenu;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(&file_menu_items_.back());
    InsertMenuItemW(file_menu_, static_cast<UINT>(GetMenuItemCount(file_menu_)), TRUE, &item);
  };
  append(true, false, kOpenList, IDI_TREE_FOLDER, L"Открыть список баз…", L"Ctrl+O");
  append(true, false, kOpenStandardList, IDI_TREE_FOLDER, L"Открыть стандартный список 1С");
  HMENU recent = CreatePopupMenu();
  if (recent) {
    size_t count = 0;
    for (const auto& path : settings_.recent_ibases) {
      if (count >= 9) break;
      const UINT command = kRecentList1 + static_cast<UINT>(count++);
      AppendMenuW(recent, MF_STRING, command, path.wstring().c_str());
    }
    if (count == 0) AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, L"Нет недавно открытых списков");
    appendPopup(recent, kRecentListsMenu, IDI_ACTION_REFRESH, L"Недавно открытые списки");
  }
  if (!settings_.simple_mode) {
    AppendMenuW(file_menu_, MF_SEPARATOR, 0, nullptr);
    append(true, false, kAddFile, IDI_ACTION_ADD, L"Добавить файловую базу…", L"Ctrl+Alt+F");
    append(true, false, kAddServer, IDI_ACTION_ADD, L"Добавить серверную базу…", L"Ctrl+Alt+S");
    append(true, false, kAddGroup, IDI_TREE_FOLDER, L"Добавить группу…", L"Ctrl+Alt+G");
    AppendMenuW(file_menu_, MF_SEPARATOR, 0, nullptr);
    append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список", L"F5");
  }
}
void MainWindow::RefreshMainMenuBar() {
  if (!menu_ || !file_menu_ || !view_menu_ || !help_menu_) return;
  const auto clearMenu = [](HMENU menu) {
    while (GetMenuItemCount(menu) > 0) RemoveMenu(menu, 0, MF_BYPOSITION);
  };
  clearMenu(view_menu_);
  clearMenu(help_menu_);
  for (const auto& item : main_menu_items_) if (item.icon) DestroyIcon(item.icon);
  main_menu_items_.clear();
  main_menu_items_.reserve(14);
  const auto append = [&](HMENU target, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}, bool checked = false) {
    ContextMenuItem visual{command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20), std::move(text), std::move(shortcut)};
    main_menu_items_.push_back(std::move(visual));
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
    item.fType = MFT_OWNERDRAW;
    item.wID = command;
    item.fState = MFS_ENABLED | (checked ? MFS_CHECKED : 0);
    item.dwItemData = reinterpret_cast<ULONG_PTR>(&main_menu_items_.back());
    InsertMenuItemW(target, static_cast<UINT>(GetMenuItemCount(target)), TRUE, &item);
  };
  if (settings_.simple_mode) {
    append(view_menu_, kSimpleMode, 0, L"Выйти из простого режима", L"Ctrl+Alt+M", true);
  } else {
    append(view_menu_, kToggleFavorite, IDI_ACTION_FAVORITE, L"Добавить/убрать из избранного", L"Ctrl+Alt+I");
    append(view_menu_, kEditTags, 0, L"Управление тегами выбранной базы…");
    append(view_menu_, kConfigureTagColors, 0, L"Настроить теги…");
    append(view_menu_, kShowTagsInList, 0, L"Показывать теги в списке баз", {}, settings_.show_tags_in_list);
    append(view_menu_, kClearRecent, IDI_ACTION_DELETE, L"Очистить недавние базы…");
    append(view_menu_, kSimpleMode, 0, L"Простой режим", L"Ctrl+Alt+M");
    append(help_menu_, kCheckForUpdates, IDI_ACTION_UPDATE, L"Проверить обновления…");
    AppendMenuW(help_menu_, MF_SEPARATOR, 0, nullptr);
    append(help_menu_, kAbout, IDI_IBSTART, L"О программе…", L"F1");
  }
  clearMenu(menu_);
  if (!settings_.simple_mode) AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu_), L"Файл");
  AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu_), settings_.simple_mode ? L"Режим" : L"Вид");
  if (!settings_.simple_mode) AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu_), L"Справка");
  SetMenu(window_, menu_);
  DrawMenuBar(window_);
}
void MainWindow::RememberRecentList(const std::filesystem::path& path) {
  if (path.empty()) return;
  for (auto it = settings_.recent_ibases.begin(); it != settings_.recent_ibases.end();) {
    if (EqualNoCase(it->wstring(), path.wstring())) it = settings_.recent_ibases.erase(it);
    else ++it;
  }
  settings_.recent_ibases.insert(settings_.recent_ibases.begin(), path);
  if (settings_.recent_ibases.size() > 9) settings_.recent_ibases.resize(9);
}
void MainWindow::OpenList() {
  if (settings_.simple_mode) return;
  wchar_t filename[MAX_PATH]{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window_;
  dialog.lpstrFilter = L"ibases.v8i\0ibases.v8i\0Все файлы\0*.*\0";
  dialog.lpstrFile = filename;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  settings_.active_ibases = filename;
  settings_.selected_entry.clear();
  TreeView_SelectItem(tree_, nullptr);
  RememberRecentList(settings_.active_ibases);
  storage::SaveSettings(layout_, settings_);
  RefreshFileMenu();
  DrawMenuBar(window_);
  LoadCatalog();
}
void MainWindow::OpenStandardList() {
  const auto standard = storage::FindStandardIbases();
  if (!standard) {
    Message(window_, L"Стандартный файл ibases.v8i не найден. Откройте список вручную.", L"Список баз", MB_OK | MB_ICONINFORMATION);
    return;
  }
  settings_.active_ibases = *standard;
  settings_.selected_entry.clear();
  TreeView_SelectItem(tree_, nullptr);
  RememberRecentList(*standard);
  storage::SaveSettings(layout_, settings_);
  RefreshFileMenu();
  DrawMenuBar(window_);
  LoadCatalog();
}
void MainWindow::OpenRecentList(size_t index) {
  if (index >= settings_.recent_ibases.size()) return;
  const auto path = settings_.recent_ibases[index];
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    settings_.recent_ibases.erase(settings_.recent_ibases.begin() + static_cast<std::ptrdiff_t>(index));
    storage::SaveSettings(layout_, settings_);
    RefreshFileMenu();
    DrawMenuBar(window_);
    Message(window_, L"Этот список больше не доступен. Он удалён из истории.", L"Список баз", MB_OK | MB_ICONWARNING);
    return;
  }
  settings_.active_ibases = path;
  settings_.selected_entry.clear();
  TreeView_SelectItem(tree_, nullptr);
  RememberRecentList(path);
  storage::SaveSettings(layout_, settings_);
  RefreshFileMenu();
  DrawMenuBar(window_);
  LoadCatalog();
}
void MainWindow::ToggleTagDisplay() {
  if (settings_.simple_mode) return;
  const bool previous = settings_.show_tags_in_list;
  settings_.show_tags_in_list = !settings_.show_tags_in_list;
  try {
    storage::SaveSettings(layout_, settings_);
  } catch (const std::exception& error) {
    settings_.show_tags_in_list = previous;
    logger_.Error(L"Ошибка сохранения отображения тегов: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить настройку отображения тегов.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  if (view_menu_) CheckMenuItem(view_menu_, kShowTagsInList, MF_BYCOMMAND | (settings_.show_tags_in_list ? MF_CHECKED : MF_UNCHECKED));
  RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
void MainWindow::UpdateConnection() {
  if (!connection_) return;
  if (!catalog_) {
    SetWindowTextW(connection_, L"");
    return;
  }
  const auto* entry = catalog_->Find(SelectedName());
  if (!entry || !entry->IsDatabase()) {
    SetWindowTextW(connection_, L"");
    return;
  }
  const std::wstring connect = entry->ValueOr(L"Connect");
  SetWindowTextW(connection_, connect.c_str());
}
void MainWindow::SetStatus(std::wstring text) { if (status_) SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); }
std::wstring MainWindow::CatalogStatistics() const { return L"Баз: " + std::to_wstring(catalog_ ? catalog_->Databases().size() : 0) + L" | Платформ: " + std::to_wstring(platforms_.size()); }
void MainWindow::SetSimpleMode(bool enabled) {
  const std::wstring selected = SelectedName();
  settings_.simple_mode = enabled;
  const int visible = enabled ? SW_HIDE : SW_SHOW;
  for (const HWND control : {tag_filter_label_, tag_filter_, sort_label_, sort_mode_, details_title_, details_subtitle_, details_, status_,
                              edit_, cache_, shortcut_, remove_}) {
    if (control) ShowWindow(control, visible);
  }
  if (connection_) ShowWindow(connection_, SW_SHOW);
  if (enterprise_) ShowWindow(enterprise_, SW_SHOW);
  if (designer_) ShowWindow(designer_, SW_SHOW);
  if (!enabled && window_) {
    RECT bounds{};
    const int minimumWidth = ScaleForDpi(kMinimumWindowWidth, GetDpiForWindow(window_));
    if (GetWindowRect(window_, &bounds) && bounds.right - bounds.left < minimumWidth) {
      SetWindowPos(window_, nullptr, 0, 0, minimumWidth, bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  RefreshFileMenu();
  RefreshMainMenuBar();
  PopulateTree();
  if (!selected.empty()) SelectTreeItem(selected);
  DisplaySelected();
  RECT client{};
  if (GetClientRect(window_, &client)) Layout(client.right - client.left, client.bottom - client.top);
  if (tree_) RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
void MainWindow::ToggleFavorite() { if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите базу для добавления в избранное."); return; } auto favorites = storage::LoadFavorites(layout_); const auto found = std::find(favorites.begin(), favorites.end(), name); if (found == favorites.end()) { favorites.insert(favorites.begin(), name); if (favorites.size() > 9) favorites.resize(9); SetStatus(L"Добавлено в избранное: " + name); } else { favorites.erase(found); SetStatus(L"Удалено из избранного: " + name); } storage::SaveFavorites(layout_, favorites); RefreshTagFilter(); PopulateTree(); }
void MainWindow::LaunchFavorite(size_t slot) { auto favorites = storage::LoadFavorites(layout_); if (slot >= favorites.size()) { Message(window_, L"Этот слот избранного пока не назначен."); return; } SetWindowTextW(search_, L""); PopulateTree(); if (SelectTreeItem(favorites[slot])) LaunchSelected(domain::LaunchMode::enterprise); }
void MainWindow::CheckForUpdates() {
  if (update_check_) {
    SetStatus(L"Проверка обновлений уже выполняется…");
    return;
  }
  auto state = std::make_shared<UpdateCheckState>();
  update_check_ = state;
  EnableMenuItem(help_menu_, kCheckForUpdates, MF_BYCOMMAND | MF_GRAYED);
  DrawMenuBar(window_);
  SetStatus(L"Проверяем наличие обновлений…");
  const HWND owner = window_;
  std::thread([state, owner] {
    std::optional<update::Release> release;
    std::wstring error;
    try {
      release = update::FetchLatestRelease();
    } catch (const std::exception& exception) {
      error = WideErrorText(exception.what());
    } catch (...) {
      error = L"Неизвестная ошибка проверки обновлений.";
    }
    {
      std::lock_guard lock(state->mutex);
      state->release = std::move(release);
      state->error = std::move(error);
      state->completed = true;
    }
    PostMessageW(owner, kUpdateCheckFinishedMessage, 0, 0);
  }).detach();
}
void MainWindow::CompleteUpdateCheck() {
  auto state = std::move(update_check_);
  if (!state) return;

  std::optional<update::Release> release;
  std::wstring error;
  {
    std::lock_guard lock(state->mutex);
    if (!state->completed) {
      update_check_ = std::move(state);
      return;
    }
    release = std::move(state->release);
    error = std::move(state->error);
  }
  EnableMenuItem(help_menu_, kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
  DrawMenuBar(window_);
  if (!error.empty()) {
    logger_.Error(L"Ошибка проверки обновлений: " + error);
    SetStatus(L"Не удалось проверить обновления.");
    ShowUpdateCheckError();
    return;
  }
  if (!release) {
    SetStatus(L"Опубликованной стабильной версии пока нет.");
    Message(window_, L"Для ИБ Старт пока нет опубликованной стабильной версии. Предварительные версии не устанавливаются автоматически.",
        L"Проверка обновлений", MB_OK | MB_ICONINFORMATION);
    return;
  }

  const int comparison = update::CompareVersions(version::value, release->version);
  if (comparison >= 0) {
    const std::wstring text = comparison == 0
        ? L"У вас установлена актуальная версия " + std::wstring(version::value) + L"."
        : L"У вас установлена версия " + std::wstring(version::value) + L", которая новее последней опубликованной стабильной версии " + release->version + L".";
    SetStatus(L"Проверка обновлений завершена.");
    Message(window_, text, L"Проверка обновлений", MB_OK | MB_ICONINFORMATION);
    return;
  }

  SetStatus(L"Доступно обновление: " + release->version);
  const std::wstring text = L"Доступна новая версия ИБ Старт " + release->version + L".\n\nУстановлена версия: " +
      std::wstring(version::value) + L".\n\nОткрыть страницу релиза в браузере?";
  if (MessageBoxW(window_, text.c_str(), L"Доступно обновление", MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", release->page_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    logger_.Error(L"Не удалось открыть страницу релиза: " + release->page_url);
    Message(window_, L"Не удалось открыть страницу релиза в браузере.", L"Проверка обновлений", MB_OK | MB_ICONWARNING);
  }
}
void MainWindow::ShowUpdateCheckError() {
  constexpr int kOpenLogButton = 1001;
  const TASKDIALOG_BUTTON buttons[] = {{kOpenLogButton, L"Открыть журнал"}};
  TASKDIALOGCONFIG dialog{sizeof(dialog)};
  dialog.hwndParent = window_;
  dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  dialog.pszWindowTitle = L"Проверка обновлений";
  dialog.pszMainInstruction = L"Не удалось проверить обновления";
  dialog.pszContent = L"Проверьте подключение к интернету и повторите попытку.\n\n"
      L"Подробности сохранены в текущем файле журнала.";
  dialog.pszMainIcon = TD_WARNING_ICON;
  dialog.cButtons = static_cast<UINT>(sizeof(buttons) / sizeof(*buttons));
  dialog.pButtons = buttons;

  int selected{};
  if (FAILED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr))) {
    Message(window_, L"Не удалось проверить обновления. Проверьте подключение к интернету и повторите попытку.\n\n"
        L"Подробности записаны в:\n" + logger_.path().wstring(), L"Проверка обновлений", MB_OK | MB_ICONWARNING);
    return;
  }
  if (selected != kOpenLogButton) return;

  const auto openResult = reinterpret_cast<INT_PTR>(
      ShellExecuteW(window_, L"open", logger_.path().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (openResult > 32) return;
  logger_.Error(L"Не удалось открыть файл журнала: " + logger_.path().wstring());
  Message(window_, L"Не удалось открыть текущий файл журнала. Он находится по адресу:\n" + logger_.path().wstring(),
      L"Проверка обновлений", MB_OK | MB_ICONWARNING);
}
void MainWindow::ShowAbout() const { const std::wstring text = L"ИБ Старт (IBStart)\nВерсия " + std::wstring(version::value) + L"\n\nЛёгкий менеджер запусков информационных баз 1С:Предприятие.\n\nЛицензия MIT. IBStart не является официальным продуктом фирмы «1С»."; MessageBoxW(window_, text.c_str(), L"О программе — ИБ Старт", MB_OK | MB_ICONINFORMATION); }
void MainWindow::ReportUnhandledError(std::string_view message) noexcept { try { const auto wide = utf::FromUtf8(message); logger_.Error(L"Необработанная ошибка UI: " + wide); const auto text = L"Произошла непредвиденная ошибка. Подробности записаны в:\n" + logger_.path().wstring(); MessageBoxW(window_, text.c_str(), L"ИБ Старт", MB_OK | MB_ICONERROR); } catch (...) { MessageBoxW(window_, L"Произошла непредвиденная ошибка.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }

}  // namespace ibstart::ui
