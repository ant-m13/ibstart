#include "ui/main_window.hpp"
#include "ui/dialog_support.hpp"
#include "ui/folder_picker.hpp"
#include "ui/input_box.hpp"
#include "ui/tree_presentation.hpp"

#include "app/instance_activation.hpp"
#include "app/resource.h"
#include "core/cache/cache_service.hpp"
#include "core/connection/connection_string.hpp"
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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace ibstart::ui {
using dialog::CreateUiFont;
using dialog::CloseModalDialog;
using dialog::DialogControlColor;
using dialog::DialogOuterSize;
using dialog::DisableModalOwner;
using dialog::InputBox;
using dialog::PositionDialogNearOwner;
using dialog::RestoreModalOwner;
using dialog::ScaleForDpi;
using dialog::SetControlFont;
using presentation::ContainsTag;
using presentation::EraseTagStyle;
using presentation::KnownTags;
using presentation::ParseTags;
using presentation::TagId;
using presentation::TagStyleFor;
using presentation::TagsFor;
using presentation::TagsText;
namespace {
constexpr wchar_t kClassName[] = L"IBStart.MainWindow";
constexpr wchar_t kDatabaseEditorClass[] = L"IBStart.DatabaseEditor";
constexpr wchar_t kAdvancedDatabaseOptionsClass[] = L"IBStart.AdvancedDatabaseOptions";
constexpr wchar_t kTagManagerClass[] = L"IBStart.TagManager";
constexpr wchar_t kTagAssignmentClass[] = L"IBStart.TagAssignment";
constexpr UINT kActivateMessage = WM_APP + 23;
constexpr UINT kUpdateCheckFinishedMessage = WM_APP + 24;
constexpr UINT kFocusShortcutSelectionMessage = WM_APP + 25;
constexpr UINT kCacheOperationFinishedMessage = WM_APP + 26;
constexpr int kMinimumWindowWidth = 940;
constexpr int kMinimumSimpleWindowWidth = 520;
constexpr int kMinimumWindowHeight = 460;
enum Command : int { kEnterprise = 100, kDesigner, kEdit, kCache, kShortcut, kDelete, kAddDatabase, kAddGroup, kOpenList, kRefresh, kSimpleMode, kToggleFavorite, kFocusSearch, kCheckForUpdates, kAbout, kMoveUp, kMoveDown, kOpenFolder, kClearRecent, kCopyDetailValue, kCopyDetailPair, kEditTags, kConfigureTagColors, kSortAscending, kSortDescending, kToggleFoldersFirstWhenSorting, kMoveToFolder, kOpenStandardList, kShowTagsInList, kNewTagForSelected, kFavorite1 = 200 };
constexpr UINT kRecentList1 = 300;
constexpr UINT kQuickTag1 = 400;
constexpr UINT kTagsContextMenu = 250;
constexpr UINT kRecentListsMenu = 299;
enum TreeImage : int { kFileDatabaseImage, kServerDatabaseImage, kWebDatabaseImage, kFolderImage, kFavoriteImage, kRecentImage };
constexpr LPARAM kRecentRootItemData = 1;
constexpr LPARAM kFavoritesRootItemData = 2;
constexpr LPARAM kCatalogRootItemData = 3;
constexpr wchar_t kCatalogRootName[] = L"Информационные базы";

using TreeExpansionStates = std::map<std::wstring, bool>;

TreeExpansionStates CaptureTreeExpansionStates(HWND tree) {
  TreeExpansionStates result;
  const auto collect = [&](const auto& self, HTREEITEM item) -> void {
    for (auto current = item; current; current = TreeView_GetNextSibling(tree, current)) {
      const auto child = TreeView_GetChild(tree, current);
      if (child) {
        wchar_t name[512]{};
        TVITEMW row{};
        row.mask = TVIF_TEXT;
        row.hItem = current;
        row.pszText = name;
        row.cchTextMax = static_cast<int>(std::size(name));
        if (TreeView_GetItem(tree, &row)) {
          result.emplace(name, (TreeView_GetItemState(tree, current, TVIS_EXPANDED) & TVIS_EXPANDED) != 0);
        }
        self(self, child);
      }
    }
  };
  if (tree) collect(collect, TreeView_GetRoot(tree));
  return result;
}

void RestoreTreeExpansionStates(HWND tree, const TreeExpansionStates& states) {
  const auto restore = [&](const auto& self, HTREEITEM item) -> void {
    for (auto current = item; current; current = TreeView_GetNextSibling(tree, current)) {
      const auto child = TreeView_GetChild(tree, current);
      if (!child) continue;
      wchar_t name[512]{};
      TVITEMW row{};
      row.mask = TVIF_TEXT;
      row.hItem = current;
      row.pszText = name;
      row.cchTextMax = static_cast<int>(std::size(name));
      if (TreeView_GetItem(tree, &row)) {
        if (const auto found = states.find(name); found != states.end()) {
          TreeView_Expand(tree, current, found->second ? TVE_EXPAND : TVE_COLLAPSE);
        }
      }
      self(self, child);
    }
  };
  if (tree) restore(restore, TreeView_GetRoot(tree));
}

OwnerDrawMenuIcon MenuIconForCommand(UINT command) {
  switch (command) {
    case kMoveUp: return OwnerDrawMenuIcon::move_up;
    case kMoveDown: return OwnerDrawMenuIcon::move_down;
    case kSimpleMode: return OwnerDrawMenuIcon::compact_mode;
    case kTagsContextMenu:
    case kEditTags:
    case kConfigureTagColors:
    case kShowTagsInList: return OwnerDrawMenuIcon::tag;
    case kSortAscending: return OwnerDrawMenuIcon::sort_ascending;
    case kSortDescending: return OwnerDrawMenuIcon::sort_descending;
    default: return OwnerDrawMenuIcon::standard;
  }
}

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
  if (connection::Value(connect, L"File")) return L"Файловая информационная база";
  if (connection::Value(connect, L"Srvr")) return L"Серверная информационная база";
  return L"Информационная база";
}
int DatabaseTreeImage(const domain::Entry* entry) {
  if (!entry) return kServerDatabaseImage;
  const auto connect = entry->ValueOr(L"Connect");
  if (catalog::Catalog::IsWebConnection(connect)) return kWebDatabaseImage;
  if (connection::Value(connect, L"File")) return kFileDatabaseImage;
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
  for (auto current = item; current; current = TreeView_GetParent(tree, current)) {
    const LPARAM data = TreeItemData(tree, current);
    if (data == kRecentRootItemData || data == kFavoritesRootItemData) return true;
  }
  return false;
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
  result.directory = connection::ValueOrEmpty(connect, L"File");
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
  if (!connection::ValueOrEmpty(connect, L"File").empty()) return DatabaseConnectionKind::file;
  return DatabaseConnectionKind::server;
}
std::wstring BuildConnection(DatabaseConnectionKind kind, std::wstring_view original, std::wstring_view file,
    std::wstring_view web, std::wstring_view server, std::wstring_view reference) {
  std::wstring result;
  const auto append = [&result](std::wstring_view value) {
    if (value.empty()) return;
    if (!result.empty()) result.push_back(L';');
    result += value;
  };
  if (kind == DatabaseConnectionKind::file) append(L"File=" + connection::QuoteValue(std::wstring(file)));
  else if (kind == DatabaseConnectionKind::web) append(L"WS=" + connection::QuoteValue(std::wstring(web)));
  else {
    append(L"Srvr=" + connection::QuoteValue(std::wstring(server)));
    append(L"Ref=" + connection::QuoteValue(std::wstring(reference)));
  }
  bool firstFragment = true;
  for (const auto& part : connection::Split(original)) {
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
    if (EqualNoCase(connection::Trim(key), L"File") || EqualNoCase(connection::Trim(key), L"WS") ||
        EqualNoCase(connection::Trim(key), L"Srvr") || EqualNoCase(connection::Trim(key), L"Ref")) continue;
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
      return 0;
    }
    if (command == IDCANCEL) { state->done = true; return 0; }
  }
  if (message == WM_CLOSE && state) { state->done = true; return 0; }
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
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 660, 510, style, extendedStyle);
  HWND dialog = CreateWindowExW(extendedStyle, kAdvancedDatabaseOptionsClass, L"Дополнительные настройки базы", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) return std::nullopt;
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  CreateAdvancedDatabaseOptionsControls(dialog, state);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.default_app);
  MSG message{};
  int pumpResult = 1;
  while (!state.done && (pumpResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  CloseModalDialog(dialog, owner);
  if (pumpResult == 0) PostQuitMessage(static_cast<int>(message.wParam));
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
  state.file = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"File"), WS_TABSTOP | ES_AUTOHSCROLL, 48, 142, 498, 25, kFilePath, textFont);
  state.file_browse = create(0, L"BUTTON", L"Обзор…", WS_TABSTOP, 556, 142, 78, 25, kBrowseFilePath, buttonFont);
  state.web_radio = create(0, L"BUTTON", L"Веб-база", WS_TABSTOP | BS_AUTORADIOBUTTON, 28, 178, 180, 20, kConnectionWeb, textFont);
  state.web_label = create(0, L"STATIC", L"Адрес веб-сервера:", 0, 48, 202, 190, 20, 0, textFont);
  const auto web = catalog::Catalog::WebUrl(state.initial.connect);
  state.web = create(WS_EX_CLIENTEDGE, L"EDIT", web ? *web : L"", WS_TABSTOP | ES_AUTOHSCROLL, 48, 222, 586, 25, kWebAddress, textFont);
  state.server_radio = create(0, L"BUTTON", L"Серверная база 1С:Предприятия", WS_TABSTOP | BS_AUTORADIOBUTTON, 28, 258, 270, 20, kConnectionServer, textFont);
  state.server_label = create(0, L"STATIC", L"Кластер серверов:", 0, 48, 282, 150, 20, 0, textFont);
  state.server = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"Srvr"), WS_TABSTOP | ES_AUTOHSCROLL, 205, 278, 429, 25, kServerCluster, textFont);
  state.reference_label = create(0, L"STATIC", L"Имя информационной базы:", 0, 48, 310, 170, 20, 0, textFont);
  state.reference = create(WS_EX_CLIENTEDGE, L"EDIT", connection::ValueOrEmpty(state.initial.connect, L"Ref"), WS_TABSTOP | ES_AUTOHSCROLL, 220, 306, 414, 25, kServerReference, textFont);

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
      return 0;
    }
    if (command == IDCANCEL) { state->done = true; return 0; }
  }
  if (message == WM_CLOSE && state) { state->done = true; return 0; }
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
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outerSize = DialogOuterSize(owner, 660, 570, style, extendedStyle);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDatabaseEditorClass, std::wstring(title).c_str(),
      style, CW_USEDEFAULT, CW_USEDEFAULT, outerSize.cx, outerSize.cy,
      owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) return std::nullopt;
  state.font = CreateUiFont(dialog, 9, FW_NORMAL);
  state.button_font = CreateUiFont(dialog, 9, FW_NORMAL);
  CreateDatabaseEditorControls(dialog, state, platforms);
  PositionDialogNearOwner(dialog, owner);
  ShowWindow(dialog, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.name);
  MSG message{};
  int pumpResult = 1;
  while (!state.done && (pumpResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  CloseModalDialog(dialog, owner);
  if (pumpResult == 0) PostQuitMessage(static_cast<int>(message.wParam));
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

}  // namespace

struct MainWindow::UpdateCheckState {
  std::mutex mutex;
  std::optional<update::Release> release;
  std::wstring error;
  bool completed{false};
};

struct MainWindow::CacheOperationState {
  enum class Stage { finding, clearing };

  std::mutex mutex;
  Stage stage{Stage::finding};
  std::vector<cache::CacheItem> candidates;
  cache::ClearResult result;
  std::wstring error;
  bool completed{false};
};

MainWindow::MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
    storage::Settings settings, std::optional<std::wstring> launch_id)
    : instance_(instance), executable_(std::move(executable)), layout_(std::move(layout)), settings_(std::move(settings)),
      catalog_state_(layout_), logger_(layout_.root / L"logs"), initial_launch_id_(std::move(launch_id)) {}
MainWindow::~MainWindow() {
  CancelTreeDrag();
  if (window_ && IsWindow(window_)) {
    settings_.selected_entry = catalog_ && catalog_->Find(SelectedName()) ? SelectedName() : std::wstring();
    DestroyWindow(window_);
  }
  context_menu_items_.Clear();
  main_menu_items_.Clear();
  file_menu_items_.Clear();
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
      {control, 'F', kFocusSearch}, {control, 'O', kOpenList}, {controlAlt, 'F', kAddDatabase}, {controlAlt, 'G', kAddGroup},
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
      switch (LOWORD(wparam)) {
        case kEnterprise: LaunchSelected(domain::LaunchMode::enterprise); break; case kDesigner: LaunchSelected(domain::LaunchMode::designer); break;
        case kAddDatabase: AddDatabase(); break; case kAddGroup: AddGroup(); break; case kOpenList: OpenList(); break; case kOpenStandardList: OpenStandardList(); break;
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
        case kToggleFoldersFirstWhenSorting: ToggleFoldersFirstWhenSorting(); break;
        default:
          if (LOWORD(wparam) >= kFavorite1 && LOWORD(wparam) < kFavorite1 + 9) LaunchFavorite(LOWORD(wparam) - kFavorite1);
          else if (LOWORD(wparam) >= kRecentList1 && LOWORD(wparam) < kRecentList1 + 10) OpenRecentList(LOWORD(wparam) - kRecentList1);
          break;
      } return 0;
    case WM_NOTIFY:
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == tree_) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) return DrawTreeSearchMatches(reinterpret_cast<NMTVCUSTOMDRAW*>(lparam));
        if (notification->code == TVN_ITEMEXPANDINGW) {
          const auto* expanding = reinterpret_cast<NMTREEVIEWW*>(lparam);
          if (expanding && TreeItemData(tree_, expanding->itemNew.hItem) == kCatalogRootItemData && expanding->action == TVE_COLLAPSE) return TRUE;
        }
        if (notification->code == TVN_SELCHANGEDW) { DisplaySelected(); return 0; }
        if (notification->code == TVN_GETINFOTIPW) {
          const auto* hint = reinterpret_cast<NMTVGETINFOTIPW*>(lparam);
          if (!settings_.simple_mode && hint && hint->pszText && hint->cchTextMax > 0 && catalog_ && TreeItemData(tree_, hint->hItem) == 0) {
            if (const auto* entry = catalog_->Find(TreeItemName(tree_, hint->hItem)); entry && entry->IsDatabase()) {
              const auto& tags = TagsFor(catalog_state_.Read().tags, *entry);
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
      if (!app::IsValidLaunchCopyData(data)) return FALSE;
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
    case kCacheOperationFinishedMessage: CompleteCacheOperation(); return 0;
    case kFocusShortcutSelectionMessage:
      if (tree_) SetFocus(tree_);
      return 0;
    case WM_CLOSE:
      if (IsClearingCache()) {
        Message(window_, L"Дождитесь завершения очистки кэша перед закрытием приложения.", L"Очистка кэша", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      settings_.selected_entry = catalog_ && catalog_->Find(SelectedName()) ? SelectedName() : std::wstring();
      DestroyWindow(window);
      return 0;
    case WM_DESTROY: {
      WINDOWPLACEMENT placement{sizeof(placement)};
      if (GetWindowPlacement(window, &placement)) { const RECT& rect = placement.rcNormalPosition; settings_.window_x = rect.left; settings_.window_y = rect.top; settings_.window_width = rect.right - rect.left; settings_.window_height = rect.bottom - rect.top; }
      if (tree_ && IsWindow(tree_)) settings_.selected_entry = catalog_ && catalog_->Find(SelectedName()) ? SelectedName() : std::wstring();
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
    for (const HWND control : {searchLabel, search_, tag_filter_label_, tag_filter_, tree_, details_, connection_}) {
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
  // The tree starts below the tag filter, while the details panel can use the
  // same vertical band as that filter.  Keeping these anchors separate avoids
  // an unused gap above the selected database information.
  constexpr int treeTop = 74;
  constexpr int detailsTop = 39;
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
  const int buttonsY = std::max(treeTop + 100, height - bottom - buttonsHeight);
  const int connectionY = detailsTop + 58;
  const int detailsY = connectionY + 28;
  const int detailsHeight = std::max(42, buttonsY - detailsY - 10);
  const int keyWidth = std::clamp(rightWidth * 35 / 100, 80, 190);

  HDWP positions = BeginDeferWindowPos(13);
  const auto defer = [&positions](HWND control, int x, int y, int controlWidth, int controlHeight) {
    if (!positions || !control) return;
    positions = DeferWindowPos(positions, control, nullptr, x, y, std::max(1, controlWidth), std::max(1, controlHeight),
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  };
  defer(search_, 58, 7, width - 66, 25);
  defer(tag_filter_, 116, 39, 258, 25);
  defer(tree_, 8, treeTop, leftWidth, height - treeTop - bottom);
  defer(details_title_, rightX + 10, detailsTop + 7, rightWidth - 20, 26);
  defer(details_subtitle_, rightX + 10, detailsTop + 34, rightWidth - 20, 20);
  defer(details_, rightX, detailsY, rightWidth, detailsHeight);
  defer(connection_, rightX, connectionY, rightWidth, 22);
  for (const auto& button : buttons) defer(button.window, rightX + button.x, buttonsY + button.y, button.width, buttonHeight);
  const bool positioned = positions && EndDeferWindowPos(positions) != FALSE;
  if (!positioned) {
    // DeferWindowPos can fail only under severe resource pressure.  Keep a
    // complete fallback layout instead of leaving controls at old positions.
    MoveWindow(search_, 58, 7, std::max(1, width - 66), 25, TRUE);
    MoveWindow(tag_filter_, 116, 39, 258, 25, TRUE);
    MoveWindow(tree_, 8, treeTop, leftWidth, std::max(1, height - treeTop - bottom), TRUE);
    MoveWindow(details_title_, rightX + 10, detailsTop + 7, std::max(1, rightWidth - 20), 26, TRUE);
    MoveWindow(details_subtitle_, rightX + 10, detailsTop + 34, std::max(1, rightWidth - 20), 20, TRUE);
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
    catalog_state_.Reload();
    RefreshTagFilter();
    PopulateTree();
    if (!hasInitialLaunch) {
      const std::wstring& restore = selected.empty() ? settings_.selected_entry : selected;
      if (!restore.empty()) SelectTreeItem(restore);
    }
  } catch (const std::exception& error) { logger_.Error(L"Ошибка загрузки: " + ibstart::utf::FromUtf8(error.what())); if (report_error) Message(window_, L"Не удалось загрузить список баз. Проверьте путь и кодировку UTF-8.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

bool MainWindow::SaveCatalog(catalog::Catalog candidate) {
  auto target = store_ ? store_->path() : settings_.active_ibases;
  if (!store_ && target.empty()) {
    wchar_t filename[MAX_PATH] = L"ibases.v8i";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"ibases.v8i\0ibases.v8i\0Все файлы\0*.*\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"v8i";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return false;
    target = filename;
  }

  auto updatedSettings = settings_;
  updatedSettings.active_ibases = target;
  RememberRecentList(updatedSettings, target);
  std::optional<v8i::V8iFileStore> createdStore;
  auto& writer = store_ ? *store_ : createdStore.emplace(target);
  const auto logMaintenanceWarnings = [&writer, this] {
    for (const auto& warning : writer.maintenance_warnings()) {
      logger_.Error(L"Обслуживание резервных копий: " + WideErrorText(warning));
    }
  };
  try {
    writer.Save(candidate.document());
    logMaintenanceWarnings();
  } catch (const v8i::ExternalModificationError&) {
    logMaintenanceWarnings();
    const int answer = MessageBoxW(window_, L"Файл ibases.v8i был изменён другой программой. Перечитать его?", L"ИБ Старт", MB_YESNO | MB_ICONWARNING);
    if (answer == IDYES) {
      if (store_) LoadCatalog();
      else static_cast<void>(ActivateCatalog(target));
    }
    return false;
  } catch (const std::exception& error) {
    logMaintenanceWarnings();
    logger_.Error(L"Ошибка записи: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить ibases.v8i. Исходный файл не изменён.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return false;
  }

  if (createdStore) store_ = std::move(createdStore);
  catalog_ = std::move(candidate);
  settings_ = std::move(updatedSettings);
  bool settingsSaved = true;
  try {
    storage::SaveSettings(layout_, settings_);
  } catch (const std::exception& error) {
    settingsSaved = false;
    logger_.Error(L"Список баз сохранён, но настройки приложения записать не удалось: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Список баз сохранён, но историю открытых списков и настройки приложения записать не удалось.",
        L"ИБ Старт", MB_OK | MB_ICONWARNING);
  }
  RefreshFileMenu();
  DrawMenuBar(window_);
  SetStatus((settingsSaved ? L"Сохранено: " : L"Список сохранён; настройки приложения не сохранены: ") +
      settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
  return true;
}

void MainWindow::RefreshTagFilter() {
  int selection = tag_filter_ ? static_cast<int>(SendMessageW(tag_filter_, CB_GETCURSEL, 0, 0)) : 0;
  const bool favoritesSelected = selection == 1;
  std::wstring selectedTag;
  if (selection >= 2 && static_cast<size_t>(selection - 2) < filter_tags_.size()) selectedTag = filter_tags_[selection - 2];

  filter_favorites_ = catalog_state_.Read().favorites;
  filter_tags_ = catalog_ ? presentation::CollectFilterTags(*catalog_, catalog_state_.Read().tags) : std::vector<std::wstring>{};
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
presentation::TreeTagFilter MainWindow::CurrentTagFilter() const {
  presentation::TreeTagFilter filter;
  if (settings_.simple_mode || !tag_filter_) return filter;
  const int selection = static_cast<int>(SendMessageW(tag_filter_, CB_GETCURSEL, 0, 0));
  if (selection == 1) filter.kind = presentation::TreeTagFilterKind::favorites;
  else if (selection >= 2 && static_cast<size_t>(selection - 2) < filter_tags_.size()) {
    filter.kind = presentation::TreeTagFilterKind::tag;
    filter.tag = filter_tags_[selection - 2];
  }
  return filter;
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
void MainWindow::SortFolder(std::wstring_view folder, catalog::SortDirection direction) {
  if (!catalog_) return;
  auto candidate = *catalog_;
  if (!candidate.SortChildrenByName(folder, direction, settings_.folders_first_when_sorting)) return;
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTreeWithoutFlicker(folder, folder.empty());
  if (folder.empty()) {
    SetStatus(direction == catalog::SortDirection::ascending ? L"Корень списка отсортирован по возрастанию и сохранён в ibases.v8i." :
        L"Корень списка отсортирован по убыванию и сохранён в ibases.v8i.");
  } else {
    SetStatus(direction == catalog::SortDirection::ascending ? L"Папка отсортирована по возрастанию и сохранена в ibases.v8i." :
        L"Папка отсортирована по убыванию и сохранена в ibases.v8i.");
  }
}
void MainWindow::ToggleFoldersFirstWhenSorting() {
  const bool previous = settings_.folders_first_when_sorting;
  settings_.folders_first_when_sorting = !previous;
  try {
    storage::SaveSettings(layout_, settings_);
    RefreshMainMenuBar();
    SetStatus(settings_.folders_first_when_sorting ? L"При сортировке папки будут размещаться сверху." : L"При сортировке папки будут участвовать в общем порядке по имени.");
  } catch (const std::exception& error) {
    settings_.folders_first_when_sorting = previous;
    logger_.Error(L"Ошибка сохранения настройки сортировки: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить настройку сортировки.", L"ИБ Старт", MB_OK | MB_ICONERROR);
  }
}
void MainWindow::AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter,
    const presentation::TreeTagFilter& tag_filter) {
  if (!catalog_) return;
  const auto& catalog_state = catalog_state_.Read();
  for (const auto& item : items) {
    if (!presentation::MatchesSearchFilter(*catalog_, item, filter, catalog_state.tags) ||
        !presentation::MatchesTagFilter(*catalog_, item, tag_filter, catalog_state.tags, filter_favorites_)) continue;
    TVINSERTSTRUCTW row{}; row.hParent = parent; row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    row.item.pszText = const_cast<wchar_t*>(item.name.c_str());
    const auto* entry = catalog_ ? catalog_->Find(item.name) : nullptr;
    row.item.iImage = row.item.iSelectedImage = item.database ? DatabaseTreeImage(entry) : kFolderImage;
    const HTREEITEM handle = TreeView_InsertItem(tree_, &row);
    if (!item.database) {
      AddTreeItems(item.children, handle, filter, tag_filter);
      if (!filter.empty()) TreeView_Expand(tree_, handle, TVE_EXPAND);
    }
  }
}
void MainWindow::PopulateTree() {
  if (!tree_) return;
  wchar_t text[512]{}; GetWindowTextW(search_, text, 512); search_filter_ = text; const std::wstring_view filter = search_filter_; TreeView_DeleteAllItems(tree_);
  if (catalog_) {
    const auto tag_filter = CurrentTagFilter();
    const auto& catalog_state = catalog_state_.Read();
    TVINSERTSTRUCTW catalogRoot{};
    catalogRoot.hParent = TVI_ROOT;
    catalogRoot.hInsertAfter = TVI_LAST;
    catalogRoot.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_STATE;
    catalogRoot.item.pszText = const_cast<wchar_t*>(kCatalogRootName);
    catalogRoot.item.iImage = catalogRoot.item.iSelectedImage = kFolderImage;
    catalogRoot.item.lParam = kCatalogRootItemData;
    catalogRoot.item.stateMask = TVIS_EXPANDED;
    catalogRoot.item.state = TVIS_EXPANDED;
    const HTREEITEM catalogRootHandle = TreeView_InsertItem(tree_, &catalogRoot);
    AddTreeItems(catalog_->Tree(), catalogRootHandle, filter, tag_filter);
    if (catalogRootHandle) TreeView_Expand(tree_, catalogRootHandle, TVE_EXPAND);

    const auto addSpecialRoot = [&](std::wstring_view rootName, const std::vector<std::wstring>& names, int image, LPARAM itemData = 0) {
      TVINSERTSTRUCTW root{}; root.hParent = TVI_ROOT; root.hInsertAfter = TVI_LAST;
      root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM; root.item.pszText = const_cast<wchar_t*>(rootName.data()); root.item.lParam = itemData;
      root.item.iImage = root.item.iSelectedImage = image;
      const HTREEITEM rootHandle = TreeView_InsertItem(tree_, &root);
      bool any = false;
      for (const auto& name : names) {
        const auto* entry = catalog_->Find(name);
        const catalog::TreeItem item{name, true, {}, {}};
        if (!entry || !entry->IsDatabase() || !presentation::MatchesSearchFilter(*catalog_, item, filter, catalog_state.tags) ||
            !presentation::MatchesTagFilter(*catalog_, item, tag_filter, catalog_state.tags, filter_favorites_)) continue;
        TVINSERTSTRUCTW row{}; row.hParent = rootHandle; row.hInsertAfter = TVI_LAST;
        row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE; row.item.pszText = const_cast<wchar_t*>(entry->name.c_str());
        row.item.iImage = row.item.iSelectedImage = DatabaseTreeImage(entry); TreeView_InsertItem(tree_, &row); any = true;
      }
      if (any) TreeView_Expand(tree_, rootHandle, TVE_EXPAND); else TreeView_DeleteItem(tree_, rootHandle);
    };
    if (!settings_.simple_mode) {
      addSpecialRoot(L"Избранное", catalog_state.favorites, kFavoriteImage, kFavoritesRootItemData);
      std::vector<std::wstring> recent;
      for (const auto& history : catalog_state.history) for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == history.database_id) { recent.push_back(entry->name); break; }
      addSpecialRoot(L"Недавние", recent, kRecentImage, kRecentRootItemData);
    }
  }
  if (initial_launch_id_ && catalog_) {
    auto wanted = *initial_launch_id_; initial_launch_id_.reset();
    for (const auto* entry : catalog_->Databases()) if (entry->ValueOr(L"ID", entry->name) == wanted) { wanted = entry->name; break; }
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
void MainWindow::PopulateTreeWithoutFlicker(std::wstring_view selected, bool select_catalog_root) {
  const bool canSuspendDrawing = tree_ && IsWindow(tree_);
  const auto expansionStates = CaptureTreeExpansionStates(tree_);
  if (canSuspendDrawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resumeDrawing = [&] {
    if (!canSuspendDrawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };
  try {
    PopulateTree();
    RestoreTreeExpansionStates(tree_, expansionStates);
    if (select_catalog_root) SelectCatalogRoot();
    else if (!selected.empty()) SelectTreeItem(selected);
  } catch (...) {
    resumeDrawing();
    throw;
  }
  resumeDrawing();
}
LRESULT MainWindow::DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const {
  return presentation::DrawTreeSearchMatches(tree_, draw, catalog_ ? &*catalog_ : nullptr, settings_,
      catalog_state_.Read().tags, catalog_state_.Read().tag_styles, search_filter_, controls_font_);
}
LRESULT MainWindow::DrawDetailsList(NMLVCUSTOMDRAW* draw) const {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
  if (draw->nmcd.dwDrawStage != (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) return CDRF_DODEFAULT;
  const auto row = static_cast<int>(draw->nmcd.dwItemSpec);
  draw->clrTextBk = row % 2 == 0 ? RGB(242, 248, 249) : RGB(250, 252, 253);
  if (draw->iSubItem == 1 && details_ && EqualNoCase(ListViewText(details_, row, 0), L"Тег")) {
    if (const auto* style = TagStyleFor(catalog_state_.Read().tag_styles, ListViewText(details_, row, 1))) {
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
  const auto* item = FindMenuItem(measure->itemData);
  return item && OwnerDrawMenu::Measure(window_, controls_font_, *item, measure);
}

bool MainWindow::DrawContextMenuItem(const DRAWITEMSTRUCT* draw) const {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const auto* item = FindMenuItem(draw->itemData);
  return item && OwnerDrawMenu::Draw(controls_font_, *item, draw);
}

const OwnerDrawMenuItem* MainWindow::FindMenuItem(ULONG_PTR item_data) const noexcept {
  if (const auto* item = context_menu_items_.Find(item_data)) return item;
  if (const auto* item = main_menu_items_.Find(item_data)) return item;
  return file_menu_items_.Find(item_data);
}

std::wstring MainWindow::SelectedName() const {
  const HTREEITEM selected = TreeView_GetSelection(tree_);
  return TreeItemData(tree_, selected) == 0 ? TreeItemName(tree_, selected) : L"";
}
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

  const auto* dragged = catalog_->Find(dragging_name_);
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
      toRoot = true;
    } else if (TreeItemData(tree_, hit.hItem) == kCatalogRootItemData) {
      toRoot = true;
      targetItem = hit.hItem;
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
        if (cycle) targetName.clear();
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

  const HTREEITEM dropTarget = (targetIsGroup || (toRoot && targetItem)) ? targetItem : nullptr;
  // A drag image saves the pixels underneath itself.  Hide it before changing
  // the tree's drop feedback, otherwise its later restore can put those stale
  // pixels back over the freshly painted selection or insert mark.
  if (drag_image_) {
    ImageList_DragMove(treePoint.x, treePoint.y);
    ImageList_DragShowNolock(FALSE);
  }
  TreeView_SelectDropTarget(tree_, dropTarget);
  TreeView_SetInsertMark(tree_, !targetName.empty() && !targetIsGroup ? targetItem : nullptr, insertAfter);
  if (drag_image_) ImageList_DragShowNolock(TRUE);
  drag_target_name_ = std::move(targetName);
  drag_insert_after_ = insertAfter;
  drag_to_root_ = toRoot;
  if (drag_to_root_) SetStatus(L"Отпустите мышь, чтобы переместить в корень списка.");
  else if (!drag_target_name_.empty() && targetIsGroup) SetStatus(L"Отпустите мышь, чтобы переместить в группу: " + drag_target_name_);
  else if (!drag_target_name_.empty()) SetStatus(L"Отпустите мышь, чтобы вставить " + std::wstring(drag_insert_after_ ? L"после: " : L"перед: ") + drag_target_name_);
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
  if (position != std::numeric_limits<size_t>::max() && EqualNoCase(sourceParent, targetParent)) {
    if (const auto sourcePosition = CatalogPosition(draggedName, sourceParent); sourcePosition && *sourcePosition < position) --position;
  }
  auto candidate = *catalog_;
  if (!candidate.Move(draggedName, targetParent, position)) { SetStatus(L"Перемещение невозможно: нельзя поместить группу внутрь самой себя."); return; }
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTreeWithoutFlicker(draggedName);
  SetStatus(targetParent.empty() ? L"Элемент перемещён в корень списка." : L"Элемент перемещён: " + draggedName);
}
void MainWindow::CancelTreeDrag() {
  if (drag_image_) {
    if (tree_ && IsWindow(tree_)) ImageList_DragLeave(tree_);
    ImageList_EndDrag();
    ImageList_Destroy(drag_image_);
    drag_image_ = nullptr;
  }
  if (tree_ && IsWindow(tree_)) {
    TreeView_SelectDropTarget(tree_, nullptr);
    TreeView_SetInsertMark(tree_, nullptr, FALSE);
    // Repaint once after the drag image has restored its saved background.
    // This also clears any residue left by a previous native drag repaint.
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
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
      if (TreeItemData(tree_, item) == 0 && TreeView_GetItem(tree_, &row) && name == text) return item;
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
bool MainWindow::SelectCatalogRoot() {
  if (!tree_) return false;
  for (HTREEITEM item = TreeView_GetRoot(tree_); item; item = TreeView_GetNextSibling(tree_, item)) {
    if (TreeItemData(tree_, item) != kCatalogRootItemData) continue;
    TreeView_SelectItem(tree_, item);
    TreeView_EnsureVisible(tree_, item);
    return true;
  }
  return false;
}

void MainWindow::ShowTreeContextMenu(POINT screen) {
  if (!tree_ || !catalog_) return;
  context_menu_items_.Clear();
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
  const LPARAM selectedData = TreeItemData(tree_, selectedItem);
  const bool catalogRoot = selectedData == kCatalogRootItemData;
  const bool specialRoot = selectedData != 0 && !catalogRoot;
  const bool recentRoot = SelectedItemIsRecentRoot();
  const auto* entry = specialRoot ? nullptr : catalog_->Find(name);
  const bool database = entry && entry->IsDatabase();
  const bool web = database && catalog::Catalog::IsWebConnection(entry->ValueOr(L"Connect"));
  const bool launch_available = database && !cache_operation_;
  const bool group = entry && entry->IsGroup();
  const bool editable = entry && !settings_.simple_mode;
  const bool file = database && !connection::ValueOrEmpty(entry->ValueOr(L"Connect"), L"File").empty();
  const std::wstring addParent = group ? entry->name : entry ? catalog_->ParentOf(entry->name) : std::wstring();
  const bool sortTarget = catalogRoot || group;
  const std::wstring sortParent = catalogRoot ? std::wstring() : group ? entry->name : std::wstring();
  const auto& favorites = catalog_state_.Read().favorites;
  const bool favorite = std::find(favorites.begin(), favorites.end(), name) != favorites.end();

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  std::vector<std::wstring> quick_tags;
  const auto appendTo = [&](HMENU target, bool enabled, bool checked, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}) {
    context_menu_items_.Append(target, command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), enabled, checked);
  };
  const auto append = [&](bool enabled, bool checked, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}) {
    appendTo(menu, enabled, checked, command, iconResource, std::move(text), std::move(shortcut));
  };
  const auto appendPopup = [&](HMENU submenu, UINT identity, std::wstring text) {
    context_menu_items_.Append(menu, identity, nullptr, std::move(text), {}, MenuIconForCommand(identity), true, false, submenu);
  };
  const auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
  if (settings_.simple_mode) {
    if (sortTarget) {
      append(true, false, kSortAscending, 0, L"Сортировать по возрастанию");
      append(true, false, kSortDescending, 0, L"Сортировать по убыванию");
      SetForegroundWindow(window_);
      const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
      DestroyMenu(menu);
      context_menu_items_.Clear();
      if (command == kSortAscending) SortFolder(sortParent, catalog::SortDirection::ascending);
      else if (command == kSortDescending) SortFolder(sortParent, catalog::SortDirection::descending);
      return;
    }
    if (database) {
      append(launch_available, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
      append(launch_available && !web, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
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
      context_menu_items_.Clear();
      return;
    }
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
    DestroyMenu(menu);
    context_menu_items_.Clear();
    if (command) SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    return;
  }
  if (!catalogRoot) {
    append(launch_available, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
    append(launch_available && !web, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
    separator();
    append(database, favorite, kToggleFavorite, IDI_ACTION_FAVORITE, favorite ? L"Убрать из избранного" : L"Добавить в избранное", L"Ctrl+Alt+I");
    if (database) {
      HMENU tagMenu = CreatePopupMenu();
      if (tagMenu) {
        AppendMenuW(tagMenu, MF_STRING, kEditTags, L"Управление тегами…");
        AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
        const auto& assigned = TagsFor(catalog_state_.Read().tags, *entry);
        bool hasAvailableTags = false;
        for (const auto& tag : KnownTags(catalog_state_.Read().tags, catalog_state_.Read().tag_styles)) {
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
    append(editable, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
    append(editable, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
    append(editable, false, kMoveToFolder, IDI_TREE_FOLDER, L"Переместить в папку…");
    append(editable, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
  }
  if (sortTarget) {
    if (!catalogRoot) separator();
    append(true, false, kSortAscending, 0, L"Сортировать по возрастанию");
    append(true, false, kSortDescending, 0, L"Сортировать по убыванию");
  }
  separator();
  append(!settings_.simple_mode, false, kAddDatabase, IDI_ACTION_ADD, group ? L"Добавить базу в группу…" : L"Добавить базу…", L"Ctrl+Alt+F");
  append(!settings_.simple_mode, false, kAddGroup, IDI_TREE_FOLDER, group ? L"Добавить вложенную группу…" : L"Добавить группу…", L"Ctrl+Alt+G");
  separator();
  append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список", L"F5");

  SetForegroundWindow(window_);
  const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, window_, nullptr);
  DestroyMenu(menu);
  context_menu_items_.Clear();
  if (!command) return;
  const size_t quickTagIndex = command >= kQuickTag1 ? static_cast<size_t>(command - kQuickTag1) : quick_tags.size();
  if (quickTagIndex < quick_tags.size()) AddTagToSelected(quick_tags[quickTagIndex]);
  else if (command == kAddDatabase) AddDatabase(addParent);
  else if (command == kAddGroup) AddGroup(addParent);
  else if (sortTarget && command == kSortAscending) SortFolder(sortParent, catalog::SortDirection::ascending);
  else if (sortTarget && command == kSortDescending) SortFolder(sortParent, catalog::SortDirection::descending);
  else SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MainWindow::DisplaySelected() {
  if (!details_) {
    UpdateConnection();
    return;
  }
  ListView_DeleteAllItems(details_);
  const auto name = SelectedName();
  const LPARAM selectedData = TreeItemData(tree_, TreeView_GetSelection(tree_));
  const auto* entry = catalog_ && selectedData == 0 ? catalog_->Find(name) : nullptr;
  if (!entry) {
    SetWindowTextW(details_title_, selectedData == kCatalogRootItemData ? kCatalogRootName : name.empty() ? L"Выберите базу или группу" : name.c_str());
    SetWindowTextW(details_subtitle_, selectedData == kCatalogRootItemData ? L"Корневой уровень списка баз" : name.empty() ? L"Сведения появятся здесь" : L"Служебный раздел списка");
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
    const auto& tags = TagsFor(catalog_state_.Read().tags, *entry);
    for (const auto& tag : tags) addRow(L"Тег", tag);
    const auto connect = entry->ValueOr(L"Connect");
    if (!connection::ValueOrEmpty(connect, L"File").empty()) {
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
      const auto server = connection::ValueOrEmpty(connect, L"Srvr");
      const auto reference = connection::ValueOrEmpty(connect, L"Ref");
      if (!server.empty() || !reference.empty()) {
        addDivider();
        addRow(L"Сервер 1С", server);
        addRow(L"Имя базы", reference);
      }
    }
  }
  const bool database = entry->IsDatabase();
  const bool web = database && catalog::Catalog::IsWebConnection(entry->ValueOr(L"Connect"));
  const bool launch_available = database && !cache_operation_;
  EnableWindow(enterprise_, launch_available); EnableWindow(designer_, launch_available && !web);
  EnableWindow(edit_, !settings_.simple_mode); EnableWindow(remove_, !settings_.simple_mode);
  EnableWindow(cache_, database && !settings_.simple_mode && !cache_operation_); EnableWindow(shortcut_, database && !settings_.simple_mode);
  InvalidateRect(details_, nullptr, TRUE);
  UpdateConnection();
}

void MainWindow::LaunchSelected(domain::LaunchMode mode) {
  if (cache_operation_) {
    SetStatus(L"Запуск базы недоступен до завершения операции с кэшем.");
    return;
  }
  if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите информационную базу."); return; }
  bool launchSucceeded = false;
  try {
    const auto database = catalog_->DatabaseFor(name);
    const auto webUrl = catalog::Catalog::WebUrl(database.connect);
    if (webUrl && mode == domain::LaunchMode::designer) {
      Message(window_, L"Конфигуратор недоступен для веб-базы. Запустите её в режиме Предприятие тонким клиентом или в браузере.", L"ИБ Старт", MB_OK | MB_ICONINFORMATION);
      return;
    }
    const auto rememberLaunch = [&](domain::LaunchMode launchedMode) {
      const auto timestamp = std::chrono::system_clock::now();
      catalog_state_.RecordLaunch({database.id, timestamp, launchedMode});
      PopulateTreeWithoutFlicker(name);
    };
    domain::LaunchOptions options;
    options.mode = mode;
    if (mode == domain::LaunchMode::designer) {
      // The Configurator is provided only by the full (thick-client) platform.
      // App/DefaultApp describe the Enterprise launch and must not redirect F4
      // to a standalone thin client.
      options.client_type = domain::ClientType::thick;
    } else if (webUrl) {
      options.client_type = domain::ClientType::thin;
    } else {
      options.client_type = ClientTypeFromApplication(database.app);
      if (options.client_type == domain::ClientType::automatic) options.client_type = ClientTypeFromApplication(database.default_app);
    }
    if (const auto fromParameters = launcher::AppArchitectureFromParameters(database.additional_parameters)) options.architecture = *fromParameters;
    else if (const auto fromDatabase = launcher::ParseAppArchitecture(database.app_arch)) options.architecture = *fromDatabase;
    const auto& selectedVersion = database.version.empty() ? database.default_version : database.version;
    if (selectedVersion != L"" && selectedVersion != L"Авто") options.version = selectedVersion;
    const bool hasThinClient = std::any_of(platforms_.begin(), platforms_.end(), [](const auto& platform) { return platform.has_thin_client; });
    if (webUrl && !hasThinClient) {
      const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", webUrl->c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      if (result <= 32) {
        logger_.Error(L"Не удалось открыть веб-базу в браузере: " + database.name);
        Message(window_, L"Не удалось открыть веб-базу в браузере.", L"ИБ Старт", MB_OK | MB_ICONERROR);
        return;
      }
      launchSucceeded = true;
      logger_.Info(L"Открыта веб-база в браузере: " + database.name);
      SetStatus(L"Открыта база в браузере: " + database.name);
      rememberLaunch(domain::LaunchMode::web_client);
      return;
    }
    if (options.client_type == domain::ClientType::web) { Message(window_, L"Веб-клиент можно использовать только для веб-базы с адресом http:// или https://.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; }
    auto selected = launcher::SelectPlatform(platforms_, options);
    bool usedNewestThinClient = false;
    if (!selected && webUrl && !selectedVersion.empty() && selectedVersion != L"Авто") {
      auto newestThinOptions = options;
      newestThinOptions.version = L"Авто";
      selected = launcher::SelectPlatform(platforms_, newestThinOptions);
      usedNewestThinClient = selected.has_value();
    }
    if (!selected) {
      Message(window_, L"Подходящая платформа 1С не найдена. Проверьте установку и настройки поиска.", L"ИБ Старт", MB_OK | MB_ICONERROR);
      return;
    }
    const auto parameters = database.additional_parameters; if (utf::FindNoCaseOrdinal(parameters, L"/p") != std::wstring_view::npos && MessageBoxW(window_, L"В дополнительных параметрах обнаружен /P. Пароль может храниться в открытом виде в ibases.v8i. Продолжить?", L"Предупреждение", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    const auto command = launcher::BuildCommand(database, *selected, options);
    if (usedNewestThinClient) logger_.Info(L"Требуемая версия " + selectedVersion + L" не найдена; для веб-базы выбран тонкий клиент " + selected->version + L".");
    launcher::Launch(command);
    launchSucceeded = true;
    logger_.Info(L"Запуск: " + logging::RedactedCommandLine(command));
    SetStatus(L"Запущена база: " + database.name);
    rememberLaunch(mode);
  } catch (const std::exception& error) {
    if (launchSucceeded) {
      logger_.Error(L"Ошибка после успешного запуска: " + ibstart::utf::FromUtf8(error.what()));
      SetStatus(L"База запущена, но историю запуска или список обновить не удалось.");
      Message(window_, L"База запущена, но сохранить историю запуска или обновить список не удалось.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    } else {
      logger_.Error(L"Ошибка запуска: " + ibstart::utf::FromUtf8(error.what()));
      Message(window_, L"Не удалось запустить базу. Подробности — в последнем логе.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    }
  }
}

std::wstring MainWindow::NextName(std::wstring_view stem) const { for (unsigned number = 1;; ++number) { const auto candidate = std::wstring(stem) + L" " + std::to_wstring(number); if (!catalog_ || !catalog_->Find(candidate)) return candidate; } }
void MainWindow::OpenSelectedFolder() {
  if (!catalog_) return; const auto name = SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) return;
  const auto folder = connection::ValueOrEmpty(entry->ValueOr(L"Connect"), L"File");
  if (folder.empty()) return;
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error) || error) { Message(window_, L"Каталог файловой базы не найден.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) Message(window_, L"Не удалось открыть каталог базы.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
}
void MainWindow::AddDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  DatabaseEditorData initial;
  initial.name = NextName(L"Файловая база");
  initial.kind = DatabaseConnectionKind::file;
  auto entered = EditDatabase(window_, L"Добавление информационной базы", std::move(initial), platforms_);
  if (!entered) return;
  auto candidate = *catalog_;
  bool added = false;
  if (entered->kind == DatabaseConnectionKind::file) {
    added = candidate.AddFileDatabase(entered->name, std::filesystem::path(connection::ValueOrEmpty(entered->connect, L"File")), parent);
  } else {
    added = candidate.AddServerDatabase(entered->name, entered->connect, parent);
  }
  if (!added) {
    const std::wstring message = entered->kind == DatabaseConnectionKind::file
        ? L"Не удалось добавить базу. Укажите уникальное имя, существующую группу и каталог, содержащий 1Cv8.1CD."
        : L"Не удалось добавить базу. Укажите уникальное имя, корректное подключение и существующую группу.";
    Message(window_, message, L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  if (auto* entry = candidate.Find(entered->name)) {
    entered->id = entry->ValueOr(L"ID");
    entered->folder = entry->ValueOr(L"Folder");
    if (entered->order_in_list.empty()) entered->order_in_list = entry->ValueOr(L"OrderInList");
    if (entered->order_in_tree.empty()) entered->order_in_tree = entry->ValueOr(L"OrderInTree");
    ApplyDatabaseEditorData(*entry, *entered);
  }
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTree();
  SelectTreeItem(entered->name);
}
void MainWindow::AddGroup(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = InputBox(window_, L"Добавить группу", L"Название группы:", NextName(L"Новая группа"));
  if (!name) return;
  auto candidate = *catalog_;
  if (!candidate.AddGroup(*name, parent)) {
    Message(window_, L"Имя группы уже используется или родительская группа недоступна.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  } else {
    if (!SaveCatalog(std::move(candidate))) return;
    PopulateTree();
    SelectTreeItem(*name);
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
    auto candidate = *catalog_;
    if (!candidate.RenameGroup(selected, TrimText(*changed))) {
      Message(window_, L"Имя группы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
      return;
    }
    const auto renamed = TrimText(*changed);
    if (!SaveCatalog(std::move(candidate))) return;
    PopulateTree();
    SelectTreeItem(renamed);
    return;
  }
  const auto previousTagId = TagId(*entry);
  const auto edited = EditDatabase(window_, L"Редактирование информационной базы", DatabaseEditorDataFromEntry(*entry), platforms_);
  if (!edited) return;
  auto candidate = *catalog_;
  if (selected != edited->name && !candidate.RenameDatabase(selected, edited->name)) {
    Message(window_, L"Имя базы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  entry = candidate.Find(edited->name);
  if (!entry) return;
  ApplyDatabaseEditorData(*entry, *edited);
  const auto updatedTagId = TagId(*entry);
  if (!SaveCatalog(std::move(candidate))) return;
  if (selected != edited->name || previousTagId != updatedTagId) {
    try {
      catalog_state_.RenameDatabaseMetadata(selected, edited->name, previousTagId, updatedTagId);
    } catch (const std::exception& error) {
      logger_.Error(L"Ошибка обновления метаданных после переименования: " + ibstart::utf::FromUtf8(error.what()));
      Message(window_, L"База переименована, но её избранное или теги не удалось сохранить.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    }
  }
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
  const auto& metadata = catalog_state_.Read();
  const auto edited = EditTagAssignment(window_, TagsFor(metadata.tags, *entry), metadata.tags, metadata.tag_styles);
  if (!edited) return;

  const auto& values = *edited;
  try {
    catalog_state_.SetTags(TagId(*entry), values);
  } catch (const std::exception& error) {
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
  const auto& metadata = catalog_state_.Read();
  const auto updated = EditTagManager(window_, metadata.tags, metadata.tag_styles);
  if (!updated) return;
  try {
    catalog_state_.ReplaceTagConfiguration(updated->tags, updated->styles);
  } catch (const std::exception& error) {
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
  try {
    if (!catalog_state_.AddTag(TagId(*entry), tag)) {
      SetStatus(L"У базы уже есть тег «" + tag + L"».");
      return;
    }
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка добавления тега: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось добавить тег базе.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return;
  }
  RefreshTagFilter();
  PopulateTree();
  SelectTreeItem(name);
  SetStatus(L"Тег добавлен: " + TagsText(TagsFor(catalog_state_.Read().tags, *entry)));
}
void MainWindow::AddNewTagToSelected() {
  const auto entered = InputBox(window_, L"Новый тег", L"Название тега:", L"");
  if (!entered) return;
  const auto requested = TrimText(*entered);
  if (requested.empty()) return;
  const auto& metadata = catalog_state_.Read();
  const auto known = KnownTags(metadata.tags, metadata.tag_styles);
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
  const auto item = entry->IsDatabase() ? L"информационную базу" : L"группу";
  const auto message = L"Удалить " + std::wstring(item) + L" \"" + name + L"\" из списка.";
  if (MessageBoxW(window_, message.c_str(), L"ИБ Старт", MB_YESNO) != IDYES) return;
  auto candidate = *catalog_;
  if (!candidate.Remove(name)) return;
  if (!SaveCatalog(std::move(candidate))) return;
  if (!tagId.empty()) {
    try {
      catalog_state_.RemoveTags(tagId);
    } catch (const std::exception& error) {
      logger_.Error(L"Ошибка удаления тегов: " + ibstart::utf::FromUtf8(error.what()));
      Message(window_, L"База удалена из списка, но её теги не удалось удалить.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    }
  }
  RefreshTagFilter();
  PopulateTree();
}
void MainWindow::MoveSelected(int offset) {
  if (!catalog_) return;
  const auto name = SelectedName();
  auto candidate = *catalog_;
  if (!candidate.MoveBy(name, offset)) {
    SetStatus(offset < 0 ? L"Элемент уже находится первым в группе." : L"Элемент уже находится последним в группе.");
    return;
  }
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTreeWithoutFlicker(name);
}
void MainWindow::MoveSelectedToFolder() {
  if (!catalog_) return;
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (!entry) {
    Message(window_, L"Выберите базу или группу для перемещения.", L"Перемещение в папку", MB_OK | MB_ICONWARNING);
    return;
  }
  const bool group = entry->IsGroup();
  const auto current = catalog_->ParentOf(entry->name);
  const auto target = dialog::SelectCatalogFolder(window_, catalog_->Tree(), current,
      group ? std::wstring_view(entry->name) : std::wstring_view{});
  if (!target) return;
  if (EqualNoCase(*target, current)) {
    SetStatus(group ? L"Группа уже находится в выбранной папке." : L"База уже находится в выбранной папке.");
    return;
  }
  auto candidate = *catalog_;
  if (!candidate.Move(name, *target, std::numeric_limits<size_t>::max())) {
    Message(window_, L"Не удалось переместить выбранный элемент в папку.", L"Перемещение в папку", MB_OK | MB_ICONWARNING);
    return;
  }
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTreeWithoutFlicker(name);
  const std::wstring item = group ? L"Группа" : L"База";
  SetStatus(target->empty() ? item + L" перемещена в корневой уровень." : item + L" перемещена в папку: " + *target);
}
void MainWindow::ClearSelectedCache() {
  if (settings_.simple_mode || !catalog_) return;
  if (cache_operation_) {
    SetStatus(L"Операция с кэшем уже выполняется…");
    return;
  }
  try {
    const auto database = catalog_->DatabaseFor(SelectedName());
    const auto state = std::make_shared<CacheOperationState>();
    cache_operation_ = state;
    DisplaySelected();
    SetStatus(L"Анализируем размер кэша…");
    const HWND owner = window_;
    std::thread([state, owner, database] {
      std::vector<cache::CacheItem> candidates;
      std::wstring error;
      try {
        candidates = cache::CandidatesFor(database);
      } catch (const std::exception& exception) {
        error = WideErrorText(exception.what());
      } catch (...) {
        error = L"Неизвестная ошибка анализа кэша.";
      }
      {
        std::lock_guard lock(state->mutex);
        state->candidates = std::move(candidates);
        state->error = std::move(error);
        state->completed = true;
      }
      PostMessageW(owner, kCacheOperationFinishedMessage, 0, 0);
    }).detach();
  } catch (const std::exception& error) {
    cache_operation_.reset();
    DisplaySelected();
    logger_.Error(L"Ошибка подготовки очистки кэша: " + WideErrorText(error.what()));
    Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  } catch (...) {
    cache_operation_.reset();
    DisplaySelected();
    Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  }
}
bool MainWindow::IsClearingCache() const {
  const auto state = cache_operation_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->stage == CacheOperationState::Stage::clearing;
}
void MainWindow::ClearRecentBases() {
  try {
    if (catalog_state_.Read().history.empty()) { SetStatus(L"Список недавних баз уже пуст."); return; }
    if (MessageBoxW(window_, L"Очистить список недавних баз?\n\nСами базы и избранное не будут затронуты.", L"Очистить недавние базы", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    catalog_state_.ClearHistory();
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
  file_menu_items_.Clear();
  const auto append = [&](bool enabled, bool checked, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}) {
    file_menu_items_.Append(file_menu_, command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), enabled, checked);
  };
  const auto appendPopup = [&](HMENU submenu, UINT identity, int iconResource, std::wstring text) {
    file_menu_items_.Append(file_menu_, identity, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20),
        std::move(text), {}, MenuIconForCommand(identity), true, false, submenu);
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
    append(true, false, kAddDatabase, IDI_ACTION_ADD, L"Добавить базу…", L"Ctrl+Alt+F");
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
  main_menu_items_.Clear();
  const auto append = [&](HMENU target, UINT command, int iconResource, std::wstring text, std::wstring shortcut = {}, bool checked = false) {
    main_menu_items_.Append(target, command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), true, checked, nullptr,
        command == kToggleFoldersFirstWhenSorting);
  };
  if (settings_.simple_mode) {
    append(view_menu_, kSimpleMode, 0, L"Выйти из простого режима", L"Ctrl+Alt+M", true);
  } else {
    append(view_menu_, kToggleFavorite, IDI_ACTION_FAVORITE, L"Добавить/убрать из избранного", L"Ctrl+Alt+I");
    append(view_menu_, kToggleFoldersFirstWhenSorting, IDI_TREE_FOLDER, L"Папки всегда сверху при сортировке", {}, settings_.folders_first_when_sorting);
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
void MainWindow::RememberRecentList(storage::Settings& settings, const std::filesystem::path& path) {
  if (path.empty()) return;
  for (auto it = settings.recent_ibases.begin(); it != settings.recent_ibases.end();) {
    if (EqualNoCase(it->wstring(), path.wstring())) it = settings.recent_ibases.erase(it);
    else ++it;
  }
  settings.recent_ibases.insert(settings.recent_ibases.begin(), path);
  if (settings.recent_ibases.size() > 9) settings.recent_ibases.resize(9);
}
bool MainWindow::ActivateCatalog(const std::filesystem::path& path) {
  std::optional<v8i::V8iFileStore> loadedStore;
  std::optional<catalog::Catalog> loadedCatalog;
  std::vector<domain::PlatformInstallation> loadedPlatforms;
  auto loadedSettings = settings_;
  try {
    loadedStore.emplace(path);
    loadedCatalog.emplace(loadedStore->Read());
    loadedPlatforms = platform::Discover(loadedSettings.platform_search_paths);
    loadedSettings.active_ibases = path;
    loadedSettings.selected_entry.clear();
    RememberRecentList(loadedSettings, path);
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка загрузки списка " + path.wstring() + L": " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось открыть выбранный список баз. Текущий список и активный путь не изменены. Проверьте формат и кодировку UTF-8.",
        L"ИБ Старт", MB_OK | MB_ICONERROR);
    return false;
  }

  store_ = std::move(loadedStore);
  catalog_ = std::move(loadedCatalog);
  platforms_ = std::move(loadedPlatforms);
  settings_ = std::move(loadedSettings);
  TreeView_SelectItem(tree_, nullptr);

  try {
    storage::SaveSettings(layout_, settings_);
  } catch (const std::exception& error) {
    logger_.Error(L"Список открыт, но активный путь не сохранён в настройках: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Список открыт, но сохранить его как активный не удалось. После перезапуска может открыться предыдущий список.",
        L"ИБ Старт", MB_OK | MB_ICONWARNING);
  }
  try {
    static_cast<void>(catalog_state_.Reload());
  } catch (const std::exception& error) {
    logger_.Error(L"Список открыт, но локальные теги и история не перечитаны: " + ibstart::utf::FromUtf8(error.what()));
  }
  RefreshTagFilter();
  PopulateTree();
  RefreshFileMenu();
  DrawMenuBar(window_);
  SetStatus(path.wstring() + L" | " + CatalogStatistics());
  logger_.Info(L"Загружен список баз: " + path.wstring() + L" | " + CatalogStatistics());
  return true;
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
  static_cast<void>(ActivateCatalog(filename));
}
void MainWindow::OpenStandardList() {
  const auto standard = storage::FindStandardIbases();
  if (!standard) {
    Message(window_, L"Стандартный файл ibases.v8i не найден. Откройте список вручную.", L"Список баз", MB_OK | MB_ICONINFORMATION);
    return;
  }
  static_cast<void>(ActivateCatalog(*standard));
}
void MainWindow::OpenRecentList(size_t index) {
  if (index >= settings_.recent_ibases.size()) return;
  const auto path = settings_.recent_ibases[index];
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    auto updatedSettings = settings_;
    updatedSettings.recent_ibases.erase(updatedSettings.recent_ibases.begin() + static_cast<std::ptrdiff_t>(index));
    try {
      storage::SaveSettings(layout_, updatedSettings);
    } catch (const std::exception& exception) {
      logger_.Error(L"Недоступный список не удалось удалить из истории: " + ibstart::utf::FromUtf8(exception.what()));
      Message(window_, L"Список больше не доступен, но удалить его из истории не удалось.", L"Список баз", MB_OK | MB_ICONWARNING);
      return;
    }
    settings_ = std::move(updatedSettings);
    RefreshFileMenu();
    DrawMenuBar(window_);
    Message(window_, L"Этот список больше не доступен. Он удалён из истории.", L"Список баз", MB_OK | MB_ICONWARNING);
    return;
  }
  static_cast<void>(ActivateCatalog(path));
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
  for (const HWND control : {tag_filter_label_, tag_filter_, details_title_, details_subtitle_, details_, status_,
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
void MainWindow::ToggleFavorite() {
  if (!catalog_) return;
  const auto name = SelectedName();
  const auto* entry = catalog_->Find(name);
  if (!entry || !entry->IsDatabase()) {
    Message(window_, L"Выберите базу для добавления в избранное.");
    return;
  }

  try {
    const bool added = catalog_state_.ToggleFavorite(name);
    SetStatus((added ? L"Добавлено в избранное: " : L"Удалено из избранного: ") + name);
    RefreshTagFilter();
    PopulateTree();
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка сохранения избранного: " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось сохранить избранное.", L"ИБ Старт", MB_OK | MB_ICONERROR);
  }
}

void MainWindow::LaunchFavorite(size_t slot) {
  const auto& favorites = catalog_state_.Read().favorites;
  if (slot >= favorites.size()) {
    Message(window_, L"Этот слот избранного пока не назначен.");
    return;
  }
  const auto name = favorites[slot];
  SetWindowTextW(search_, L"");
  PopulateTree();
  if (SelectTreeItem(name)) LaunchSelected(domain::LaunchMode::enterprise);
}
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
  try {
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
  } catch (const std::exception& error) {
    update_check_.reset();
    EnableMenuItem(help_menu_, kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
    DrawMenuBar(window_);
    logger_.Error(L"Не удалось запустить проверку обновлений: " + WideErrorText(error.what()));
    SetStatus(L"Не удалось запустить проверку обновлений.");
    Message(window_, L"Не удалось запустить фоновую проверку обновлений.", L"Проверка обновлений", MB_OK | MB_ICONERROR);
  } catch (...) {
    update_check_.reset();
    EnableMenuItem(help_menu_, kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
    DrawMenuBar(window_);
    SetStatus(L"Не удалось запустить проверку обновлений.");
    Message(window_, L"Не удалось запустить фоновую проверку обновлений.", L"Проверка обновлений", MB_OK | MB_ICONERROR);
  }
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
void MainWindow::CompleteCacheOperation() {
  auto state = cache_operation_;
  if (!state) return;

  CacheOperationState::Stage stage;
  std::vector<cache::CacheItem> candidates;
  cache::ClearResult result;
  std::wstring error;
  {
    std::lock_guard lock(state->mutex);
    if (!state->completed) return;
    stage = state->stage;
    error = std::move(state->error);
    if (stage == CacheOperationState::Stage::finding) candidates = std::move(state->candidates);
    else result = std::move(state->result);
    state->completed = false;
  }

  if (stage == CacheOperationState::Stage::finding) {
    if (!error.empty()) {
      cache_operation_.reset();
      DisplaySelected();
      logger_.Error(L"Ошибка анализа кэша: " + error);
      SetStatus(L"Не удалось проанализировать кэш.");
      Message(window_, L"Не удалось проанализировать кэш. Подробности — в журнале.", L"Очистка кэша", MB_OK | MB_ICONERROR);
      return;
    }
    if (candidates.empty()) {
      cache_operation_.reset();
      DisplaySelected();
      SetStatus(L"Безопасных каталогов кэша для этой базы не найдено.");
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
    if (MessageBoxW(window_, list.c_str(), L"Очистка кэша", MB_YESNO | MB_ICONWARNING) != IDYES) {
      cache_operation_.reset();
      DisplaySelected();
      SetStatus(L"Очистка кэша отменена.");
      return;
    }

    {
      std::lock_guard lock(state->mutex);
      state->stage = CacheOperationState::Stage::clearing;
    }
    SetStatus(L"Очищаем кэш…");
    const HWND owner = window_;
    try {
      std::thread([state, owner, candidates = std::move(candidates)] {
        cache::ClearResult result;
        std::wstring error;
        try {
          result = cache::Clear(candidates);
        } catch (const std::exception& exception) {
          error = WideErrorText(exception.what());
        } catch (...) {
          error = L"Неизвестная ошибка очистки кэша.";
        }
        {
          std::lock_guard lock(state->mutex);
          state->result = std::move(result);
          state->error = std::move(error);
          state->completed = true;
        }
        PostMessageW(owner, kCacheOperationFinishedMessage, 0, 0);
      }).detach();
    } catch (const std::exception& exception) {
      cache_operation_.reset();
      DisplaySelected();
      logger_.Error(L"Не удалось запустить очистку кэша: " + WideErrorText(exception.what()));
      SetStatus(L"Не удалось запустить очистку кэша.");
      Message(window_, L"Не удалось запустить фоновую очистку кэша.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    } catch (...) {
      cache_operation_.reset();
      DisplaySelected();
      SetStatus(L"Не удалось запустить очистку кэша.");
      Message(window_, L"Не удалось запустить фоновую очистку кэша.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    }
    return;
  }

  cache_operation_.reset();
  DisplaySelected();
  if (!error.empty()) {
    logger_.Error(L"Ошибка очистки кэша: " + error);
    SetStatus(L"Не удалось очистить кэш.");
    Message(window_, L"Не удалось очистить кэш. Подробности — в журнале.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    return;
  }
  const auto size = cache::FormatSize(result.bytes);
  logger_.Info(L"Очистка кэша: файлов=" + std::to_wstring(result.files) + L", байт=" + std::to_wstring(result.bytes) + L" (" + size + L")");
  for (const auto& item : result.errors) logger_.Error(L"Ошибка очистки кэша: " + item);
  SetStatus(result.errors.empty() ? L"Кэш очищен." : L"Кэш очищен с ошибками.");
  const std::wstring text = L"Очищено файлов: " + std::to_wstring(result.files) + L"\nОсвобождено: " + size +
      (result.errors.empty() ? L"" : L"\n\nНе удалось очистить некоторые каталоги. Подробности — в журнале.");
  Message(window_, text, L"Очистка кэша", result.errors.empty() ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
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
void MainWindow::ShowAbout() const { const std::wstring text = L"ИБ Старт (IBStart)\nВерсия " + std::wstring(version::value) + L"\n\nМенеджер запуска 1С.\n\nЛицензия MIT. IBStart не является официальным продуктом фирмы «1С»."; MessageBoxW(window_, text.c_str(), L"О программе — ИБ Старт", MB_OK | MB_ICONINFORMATION); }
void MainWindow::ReportUnhandledError(std::string_view message) noexcept { try { const auto wide = utf::FromUtf8(message); logger_.Error(L"Необработанная ошибка UI: " + wide); const auto text = L"Произошла непредвиденная ошибка. Подробности записаны в:\n" + logger_.path().wstring(); MessageBoxW(window_, text.c_str(), L"ИБ Старт", MB_OK | MB_ICONERROR); } catch (...) { MessageBoxW(window_, L"Произошла непредвиденная ошибка.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }

}  // namespace ibstart::ui
