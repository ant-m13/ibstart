#include "ui/main_window.hpp"
#include "ui/command_ids.hpp"
#include "ui/database_editor_dialog.hpp"
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
#include "core/launcher/process_launcher.hpp"
#include "core/shell/shortcut.hpp"
#include "core/update/update_service.hpp"

#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ibstart::ui {
using namespace commands;
using dialog::CreateUiFont;
using dialog::DialogControlColor;
using dialog::InputBox;
using dialog::ScaleForDpi;
using presentation::ContainsTag;
using presentation::KnownTags;
using presentation::TagId;
using presentation::TagsFor;
using presentation::TagsText;
namespace {
constexpr wchar_t kClassName[] = L"IBStart.MainWindow";
constexpr UINT kActivateMessage = WM_APP + 23;
constexpr UINT kUpdateCheckFinishedMessage = WM_APP + 24;
constexpr UINT kFocusShortcutSelectionMessage = WM_APP + 25;
constexpr UINT kCacheOperationFinishedMessage = WM_APP + 26;
constexpr UINT_PTR kBackgroundPollTimer = 1;
constexpr UINT_PTR kSearchRefreshTimer = 2;
constexpr UINT kBackgroundPollIntervalMilliseconds = 100;
constexpr UINT kSearchRefreshDelayMilliseconds = 180;
constexpr int kMinimumWindowWidth = 940;
constexpr int kMinimumSimpleWindowWidth = 520;
constexpr int kMinimumWindowHeight = 460;
void Message(HWND owner, std::wstring_view text, std::wstring_view title = L"ИБ Старт", UINT type = MB_OK | MB_ICONINFORMATION) { MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type); }
std::wstring WideErrorText(std::string_view message) noexcept {
  try { return utf::FromUtf8(message); }
  catch (...) { return std::wstring(message.begin(), message.end()); }
}
HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
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
domain::ClientType ClientTypeFromApplication(std::wstring_view value) {
  if (EqualNoCase(value, L"ThinClient")) return domain::ClientType::thin;
  if (EqualNoCase(value, L"ThickClient")) return domain::ClientType::thick;
  if (EqualNoCase(value, L"WebClient")) return domain::ClientType::web;
  return domain::ClientType::automatic;
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

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
    storage::Settings settings, std::optional<std::wstring> launch_id)
    : instance_(instance), executable_(std::move(executable)), layout_(std::move(layout)), settings_(std::move(settings)),
      catalog_state_(layout_), logger_(layout_.root / L"logs"), tag_manager_(catalog_state_, logger_),
      initial_launch_id_(std::move(launch_id)) {
  RegisterCommandHandlers();
}

void MainWindow::RegisterCommandHandlers() {
  command_dispatcher_.Register(kEnterprise, [this] { LaunchSelected(domain::LaunchMode::enterprise); });
  command_dispatcher_.Register(kDesigner, [this] { LaunchSelected(domain::LaunchMode::designer); });
  command_dispatcher_.Register(kAddDatabase, [this] { AddDatabase(); });
  command_dispatcher_.Register(kAddGroup, [this] { AddGroup(); });
  command_dispatcher_.Register(kOpenList, [this] { OpenList(); });
  command_dispatcher_.Register(kOpenStandardList, [this] { OpenStandardList(); });
  command_dispatcher_.Register(kRefresh, [this] {
    const std::wstring selected = tree_view_.SelectedName();
    LoadCatalog();
    if (!selected.empty()) static_cast<void>(tree_view_.SelectItem(selected));
  });
  command_dispatcher_.Register(kEdit, [this] { EditSelected(); });
  command_dispatcher_.Register(kCache, [this] { ClearSelectedCache(); });
  command_dispatcher_.Register(kClearRecent, [this] { ClearRecentBases(); });
  command_dispatcher_.Register(kShortcut, [this] { CreateShortcut(); });
  command_dispatcher_.Register(kOpenFolder, [this] { OpenSelectedFolder(); });
  command_dispatcher_.Register(kDelete, [this] { DeleteSelected(); });
  command_dispatcher_.Register(kCopyDetailValue, [this] { CopySelectedDetail(false); });
  command_dispatcher_.Register(kCopyDetailPair, [this] { CopySelectedDetail(true); });
  command_dispatcher_.Register(kEditTags, [this] { EditSelectedTags(); });
  command_dispatcher_.Register(kConfigureTagColors, [this] { ConfigureTagColors(); });
  command_dispatcher_.Register(kSimpleMode, [this] { SetSimpleMode(!settings_.simple_mode); });
  command_dispatcher_.Register(kToggleFavorite, [this] { ToggleFavorite(); });
  command_dispatcher_.Register(kShowTagsInList, [this] { ToggleTagDisplay(); });
  command_dispatcher_.Register(kFocusSearch, [this] { SetFocus(search_); });
  command_dispatcher_.Register(kCheckForUpdates, [this] { CheckForUpdates(); });
  command_dispatcher_.Register(kAbout, [this] { ShowAbout(); });
  command_dispatcher_.Register(kExit, [this] { SendMessageW(window_, WM_CLOSE, 0, 0); });
  command_dispatcher_.Register(kMoveUp, [this] { MoveSelected(-1); });
  command_dispatcher_.Register(kMoveDown, [this] { MoveSelected(1); });
  command_dispatcher_.Register(kMoveToFolder, [this] { MoveSelectedToFolder(); });
  command_dispatcher_.Register(kNewTagForSelected, [this] { AddNewTagToSelected(); });
  command_dispatcher_.Register(kToggleFoldersFirstWhenSorting, [this] { ToggleFoldersFirstWhenSorting(); });
  command_dispatcher_.RegisterRange(kFavorite1, 9, [this](std::size_t slot) { LaunchFavorite(slot); });
  command_dispatcher_.RegisterRange(kRecentList1, 10, [this](std::size_t index) { OpenRecentList(index); });
}
MainWindow::~MainWindow() {
  StopAndJoinBackgroundThreads();
  CancelTreeDrag();
  if (window_ && IsWindow(window_)) {
    const auto selected = tree_view_.SelectedName();
    settings_.selected_entry = catalog_ && catalog_->Find(selected) ? selected : std::wstring();
    DestroyWindow(window_);
  }
  for (const auto images : button_images_) if (images) ImageList_Destroy(images);
  if (controls_font_) DeleteObject(controls_font_);
  if (controls_bold_font_) DeleteObject(controls_bold_font_);
  if (button_font_) DeleteObject(button_font_);
  if (details_title_font_) DeleteObject(details_title_font_);
  if (details_subtitle_font_) DeleteObject(details_subtitle_font_);
  if (details_key_font_) DeleteObject(details_key_font_);
}

void MainWindow::StopAndJoinBackgroundThreads() noexcept {
  update_check_.StopAndJoin();
  cache_operation_.StopAndJoin();
}

bool MainWindow::RefreshBackgroundPolling() {
  if (!window_ || !IsWindow(window_)) return false;
  if (update_check_.active() || cache_operation_.active()) {
    return SetTimer(window_, kBackgroundPollTimer, kBackgroundPollIntervalMilliseconds, nullptr) != 0;
  }
  KillTimer(window_, kBackgroundPollTimer);
  return true;
}

void MainWindow::PollBackgroundOperations() {
  if (update_check_.completed()) CompleteUpdateCheck();
  if (!window_) return;
  if (cache_operation_.completed()) CompleteCacheOperation();
}

void MainWindow::BeginClose() {
  if (closing_) return;
  closing_ = true;
  update_check_.RequestStop();
  cache_operation_.RequestStop();
  if (update_check_.active() || cache_operation_.active()) {
    ShowWindow(window_, SW_HIDE);
    if (!RefreshBackgroundPolling()) {
      StopAndJoinBackgroundThreads();
    }
  }
  TryFinishClose();
}

void MainWindow::TryFinishClose() {
  if (!closing_ || update_check_.active() || cache_operation_.active()) return;
  if (!window_ || !IsWindow(window_)) return;
  KillTimer(window_, kBackgroundPollTimer);
  const auto selected = tree_view_.SelectedName();
  settings_.selected_entry = catalog_ && catalog_->Find(selected) ? selected : std::wstring();
  DestroyWindow(window_);
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
    case WM_TIMER:
      if (wparam == kBackgroundPollTimer) {
        PollBackgroundOperations();
        return 0;
      }
      if (wparam == kSearchRefreshTimer) {
        KillTimer(window, kSearchRefreshTimer);
        if (!closing_ && !suppress_search_refresh_) PopulateTree();
        return 0;
      }
      break;
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
      if (closing_) return 0;
      if (reinterpret_cast<HWND>(lparam) == search_ && HIWORD(wparam) == EN_CHANGE) {
        if (!suppress_search_refresh_) {
          KillTimer(window, kSearchRefreshTimer);
          SetTimer(window, kSearchRefreshTimer, kSearchRefreshDelayMilliseconds, nullptr);
        }
        return 0;
      }
      if (reinterpret_cast<HWND>(lparam) == connection_ && HIWORD(wparam) == EN_SETFOCUS) { SendMessageW(connection_, EM_SETSEL, 0, -1); return 0; }
      if (reinterpret_cast<HWND>(lparam) == tag_filter_ && HIWORD(wparam) == CBN_SELCHANGE) { PopulateTree(); return 0; }
      static_cast<void>(command_dispatcher_.Dispatch(LOWORD(wparam)));
      return 0;
    case WM_NOTIFY:
      if (lparam && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == tree_) {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->code == NM_CUSTOMDRAW) return DrawTreeSearchMatches(reinterpret_cast<NMTVCUSTOMDRAW*>(lparam));
        if (notification->code == TVN_ITEMEXPANDINGW) {
          const auto* expanding = reinterpret_cast<NMTREEVIEWW*>(lparam);
          if (expanding && tree_view_.ItemData(expanding->itemNew.hItem) ==
              TreeViewController::kCatalogRootItemData && expanding->action == TVE_COLLAPSE) return TRUE;
        }
        if (notification->code == TVN_SELCHANGEDW) { DisplaySelected(); return 0; }
        if (notification->code == TVN_GETINFOTIPW) {
          const auto* hint = reinterpret_cast<NMTVGETINFOTIPW*>(lparam);
          if (!settings_.simple_mode && hint && hint->pszText && hint->cchTextMax > 0 && catalog_ &&
              tree_view_.ItemData(hint->hItem) == 0) {
            if (const auto* entry = catalog_->Find(tree_view_.ItemName(hint->hItem)); entry && entry->IsDatabase()) {
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
        if (notification->code == NM_CUSTOMDRAW) {
          return details_view_.Draw(
              reinterpret_cast<NMLVCUSTOMDRAW*>(lparam), catalog_state_.Read().tag_styles);
        }
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
      if (closing_) return FALSE;
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
    case kActivateMessage:
      if (!closing_) Activate();
      return 0;
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
      BeginClose();
      return 0;
    case WM_DESTROY: {
      KillTimer(window, kBackgroundPollTimer);
      KillTimer(window, kSearchRefreshTimer);
      WINDOWPLACEMENT placement{sizeof(placement)};
      if (GetWindowPlacement(window, &placement)) { const RECT& rect = placement.rcNormalPosition; settings_.window_x = rect.left; settings_.window_y = rect.top; settings_.window_width = rect.right - rect.left; settings_.window_height = rect.bottom - rect.top; }
      if (tree_ && IsWindow(tree_)) {
        const auto selected = tree_view_.SelectedName();
        settings_.selected_entry = catalog_ && catalog_->Find(selected) ? selected : std::wstring();
      }
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
  tree_view_.Attach(tree_, instance_);
  details_title_ = CreateWindowW(L"STATIC", L"Выберите базу или группу", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 49, 460, 26, window_, nullptr, instance_, nullptr);
  details_subtitle_ = CreateWindowW(L"STATIC", L"Сведения появятся здесь", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 76, 460, 20, window_, nullptr, instance_, nullptr);
  details_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 380, 100, 480, 182, window_, nullptr, instance_, nullptr);
  connection_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
      8, 0, 720, 22, window_, nullptr, instance_, nullptr);
  controls_font_ = CreateUiFont(window_, 9, FW_NORMAL);
  controls_bold_font_ = CreateUiFont(window_, 9, FW_BOLD);
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
  details_view_.Attach({details_title_, details_subtitle_, details_, connection_, enterprise_, designer_,
      edit_, cache_, shortcut_, remove_}, details_key_font_);
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
  context_menus_.Create(instance_);
  menus_.Create(window_, instance_);
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
  const std::wstring selected = tree_view_.SelectedName();
  const bool hasInitialLaunch = initial_launch_id_.has_value();
  try {
    if (settings_.active_ibases.empty()) { if (const auto standard = storage::FindStandardIbases()) settings_.active_ibases = *standard; }
    auto session = catalog::LoadSession(settings_.active_ibases, settings_.platform_search_paths);
    if (!session.loaded) {
      catalog_ = std::move(session.catalog);
      store_.reset();
      platforms_ = std::move(session.platforms);
      SetStatus(L"Список ibases.v8i не найден — выберите файл или добавьте базу. | " + CatalogStatistics());
    } else {
      store_ = std::move(session.store);
      catalog_ = std::move(session.catalog);
      platforms_ = std::move(session.platforms);
      SetStatus(settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
      logger_.Info(L"Загружен список баз: " + settings_.active_ibases.wstring() + L" | " + CatalogStatistics());
    }
    static_cast<void>(catalog_state_.Reload());
    RefreshTagFilter();
    PopulateTree();
    if (!hasInitialLaunch) {
      const std::wstring& restore = selected.empty() ? settings_.selected_entry : selected;
      if (!restore.empty()) static_cast<void>(tree_view_.SelectItem(restore));
    }
  } catch (const std::exception& error) { logger_.Error(L"Ошибка загрузки: " + ibstart::utf::FromUtf8(error.what())); if (report_error) Message(window_, L"Не удалось загрузить список баз. Проверьте путь и кодировку UTF-8.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

bool MainWindow::SaveCatalog(catalog::Catalog candidate) {
  auto target = store_ ? store_->path() : settings_.active_ibases;
  bool overwriteConfirmed = false;
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
    std::error_code error;
    overwriteConfirmed = std::filesystem::is_regular_file(target, error) && !error;
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
    if (overwriteConfirmed) writer.AcceptCurrentContentsForOverwrite();
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
void MainWindow::PopulateTree() {
  if (!tree_) return;
  if (window_) KillTimer(window_, kSearchRefreshTimer);
  wchar_t text[512]{};
  GetWindowTextW(search_, text, static_cast<int>(std::size(text)));
  search_filter_ = text;
  if (catalog_) {
    tree_view_.Populate(*catalog_, catalog_state_.Read(), filter_favorites_, search_filter_,
        CurrentTagFilter(), settings_.simple_mode);
  } else {
    tree_view_.Clear();
  }
  if (initial_launch_id_ && catalog_) {
    auto wanted = *initial_launch_id_; initial_launch_id_.reset();
    if (const auto* entry = catalog_->FindById(wanted)) wanted = entry->name;
    if (tree_view_.SelectItem(wanted)) {
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
  PopulateTree();
  if (select_catalog_root) static_cast<void>(tree_view_.SelectCatalogRoot());
  else if (!selected.empty()) static_cast<void>(tree_view_.SelectItem(selected));
}
void MainWindow::RefreshRecentTreeBranch(std::wstring_view selected_recent) {
  if (!tree_ || !catalog_ || settings_.simple_mode) return;
  tree_view_.RefreshRecentBranch(*catalog_, catalog_state_.Read(), filter_favorites_, search_filter_,
      CurrentTagFilter(), selected_recent);
}
LRESULT MainWindow::DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const {
  return presentation::DrawTreeSearchMatches(tree_, draw, catalog_ ? &*catalog_ : nullptr, settings_,
      catalog_state_.Read().tags, catalog_state_.Read().tag_styles, search_filter_, controls_font_, controls_bold_font_);
}
bool MainWindow::MeasureContextMenuItem(MEASUREITEMSTRUCT* measure) const {
  return context_menus_.Measure(window_, controls_font_, measure) ||
      menus_.Measure(window_, controls_font_, measure);
}

bool MainWindow::DrawContextMenuItem(const DRAWITEMSTRUCT* draw) const {
  return context_menus_.Draw(controls_font_, draw) || menus_.Draw(controls_font_, draw);
}

void MainWindow::BeginTreeDrag(HTREEITEM item, POINT treePoint) {
  if (!tree_ || !catalog_ || !item || tree_view_.ItemData(item) != 0) return;
  TreeView_SelectItem(tree_, item);
  dragging_name_ = tree_view_.SelectedName();
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
    } else if (tree_view_.ItemData(hit.hItem) == TreeViewController::kCatalogRootItemData) {
      toRoot = true;
      targetItem = hit.hItem;
    } else if (!tree_view_.IsVirtualBranch(hit.hItem)) {
      targetName = tree_view_.ItemName(hit.hItem);
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
  const auto name = details_view_.Text(row, 0);
  const auto value = details_view_.Text(row, 1);
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

  const UINT command = context_menus_.ShowDetails(window_, screen);
  if (command == kCopyDetailValue) CopySelectedDetail(false);
  else if (command == kCopyDetailPair) CopySelectedDetail(true);
}
void MainWindow::ShowTreeContextMenu(POINT screen) {
  if (!tree_ || !catalog_) return;
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

  const auto name = tree_view_.SelectedName();
  const auto selectedItem = TreeView_GetSelection(tree_);
  const LPARAM selectedData = tree_view_.ItemData(selectedItem);
  const bool catalogRoot = selectedData == TreeViewController::kCatalogRootItemData;
  const bool specialRoot = selectedData != 0 && !catalogRoot;
  const bool recentRoot = tree_view_.SelectedItemIsRecentRoot();
  const auto* entry = specialRoot ? nullptr : catalog_->Find(name);
  const bool database = entry && entry->IsDatabase();
  const bool web = database && catalog::Catalog::IsWebConnection(entry->ValueOr(L"Connect"));
  const bool launch_available = database && !cache_operation_.active();
  const bool group = entry && entry->IsGroup();
  const bool editable = entry && !settings_.simple_mode;
  const bool file = database && !connection::ValueOrEmpty(entry->ValueOr(L"Connect"), L"File").empty();
  const std::wstring addParent = group ? entry->name : entry ? catalog_->ParentOf(entry->name) : std::wstring();
  const bool sortTarget = catalogRoot || group;
  const std::wstring sortParent = catalogRoot ? std::wstring() : group ? entry->name : std::wstring();
  const auto& favorites = catalog_state_.Read().favorites;
  const bool favorite = std::find(favorites.begin(), favorites.end(), name) != favorites.end();
  std::vector<std::wstring> quick_tags;
  if (database && !settings_.simple_mode) {
    const auto& assigned = TagsFor(catalog_state_.Read().tags, *entry);
    for (const auto& tag : KnownTags(catalog_state_.Read().tags, catalog_state_.Read().tag_styles)) {
      if (!ContainsTag(assigned, tag)) quick_tags.push_back(tag);
    }
  }
  const TreeContextMenuState state{
      settings_.simple_mode, sortTarget, catalogRoot, database, web, launch_available, group,
      editable, file, recentRoot, favorite, addParent, sortParent, quick_tags};
  const UINT command = context_menus_.ShowTree(window_, screen, state);
  if (!command) return;
  if (settings_.simple_mode) {
    if (sortTarget && command == kSortAscending) SortFolder(sortParent, catalog::SortDirection::ascending);
    else if (sortTarget && command == kSortDescending) SortFolder(sortParent, catalog::SortDirection::descending);
    else if (database) SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    return;
  }
  const size_t quickTagIndex = command >= kQuickTag1 ? static_cast<size_t>(command - kQuickTag1) : quick_tags.size();
  if (quickTagIndex < quick_tags.size()) AddTagToSelected(quick_tags[quickTagIndex]);
  else if (command == kAddDatabase) AddDatabase(addParent);
  else if (command == kAddGroup) AddGroup(addParent);
  else if (sortTarget && command == kSortAscending) SortFolder(sortParent, catalog::SortDirection::ascending);
  else if (sortTarget && command == kSortDescending) SortFolder(sortParent, catalog::SortDirection::descending);
  else SendMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void MainWindow::DisplaySelected() {
  const LPARAM selected_data = tree_view_.ItemData(TreeView_GetSelection(tree_));
  details_view_.Display(catalog_ ? &*catalog_ : nullptr, &catalog_state_, tree_view_.SelectedName(),
      selected_data == TreeViewController::kCatalogRootItemData, settings_.simple_mode,
      cache_operation_.active());
}

void MainWindow::LaunchSelected(domain::LaunchMode mode) {
  if (cache_operation_.active()) {
    SetStatus(L"Запуск базы недоступен до завершения операции с кэшем.");
    return;
  }
  if (!catalog_) return; const auto name = tree_view_.SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) { Message(window_, L"Выберите информационную базу."); return; }
  const bool selectedFromRecent = tree_view_.BranchData(TreeView_GetSelection(tree_)) ==
      TreeViewController::kRecentRootItemData;
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
      RefreshRecentTreeBranch(selectedFromRecent ? std::wstring_view(name) : std::wstring_view{});
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
      if (webUrl) {
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
      Message(window_, L"Подходящая платформа 1С не найдена. Проверьте установку и настройки поиска.", L"ИБ Старт", MB_OK | MB_ICONERROR);
      return;
    }
    const auto command = launcher::BuildCommand(database, *selected, options);
    if (logging::ContainsSecretArguments(command) &&
        MessageBoxW(window_, L"В параметрах запуска обнаружен пароль или токен. Значение будет видно в ibases.v8i и интерфейсе, "
                             L"а в журналах и автоматически создаваемых диагностических сообщениях будет замаскировано. Продолжить?",
            L"Предупреждение", MB_YESNO | MB_ICONWARNING) != IDYES) return;
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
  if (!catalog_) return; const auto name = tree_view_.SelectedName(); const auto* entry = catalog_->Find(name); if (!entry || !entry->IsDatabase()) return;
  const auto folder = connection::ValueOrEmpty(entry->ValueOr(L"Connect"), L"File");
  if (folder.empty()) return;
  std::error_code error;
  if (!std::filesystem::is_directory(folder, error) || error) { Message(window_, L"Каталог файловой базы не найден.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; }
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) Message(window_, L"Не удалось открыть каталог базы.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
}
void MainWindow::AddDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  dialog::DatabaseEditorData initial;
  initial.name = NextName(L"Файловая база");
  initial.kind = dialog::DatabaseConnectionKind::file;
  auto entered = dialog::EditDatabase(window_, L"Добавление информационной базы", std::move(initial), platforms_);
  if (!entered) return;
  auto candidate = *catalog_;
  bool added = false;
  if (entered->kind == dialog::DatabaseConnectionKind::file) {
    added = candidate.AddFileDatabase(entered->name, std::filesystem::path(connection::ValueOrEmpty(entered->connect, L"File")), parent);
  } else {
    added = candidate.AddServerDatabase(entered->name, entered->connect, parent);
  }
  if (!added) {
    const std::wstring message = entered->kind == dialog::DatabaseConnectionKind::file
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
    dialog::ApplyDatabaseEditorData(*entry, *entered);
  }
  if (!SaveCatalog(std::move(candidate))) return;
  PopulateTree();
  static_cast<void>(tree_view_.SelectItem(entered->name));
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
    static_cast<void>(tree_view_.SelectItem(*name));
  }
}
void MainWindow::EditSelected() {
  if (!catalog_) return;
  const auto selected = tree_view_.SelectedName();
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
    static_cast<void>(tree_view_.SelectItem(renamed));
    return;
  }
  const auto previousTagId = TagId(*entry);
  const auto edited = dialog::EditDatabase(window_, L"Редактирование информационной базы",
      dialog::DatabaseEditorDataFromEntry(*entry), platforms_);
  if (!edited) return;
  auto candidate = *catalog_;
  if (selected != edited->name && !candidate.RenameDatabase(selected, edited->name)) {
    Message(window_, L"Имя базы уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  entry = candidate.Find(edited->name);
  if (!entry) return;
  dialog::ApplyDatabaseEditorData(*entry, *edited);
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
  static_cast<void>(tree_view_.SelectItem(edited->name));
}
void MainWindow::EditSelectedTags() {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = tree_view_.SelectedName();
  const auto* entry = catalog_->Find(name);
  ApplyTagResult(tag_manager_.EditAssignment(window_, entry), name);
}
void MainWindow::ConfigureTagColors() {
  ApplyTagResult(tag_manager_.Configure(window_));
}
void MainWindow::AddTagToSelected(std::wstring tag) {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = tree_view_.SelectedName();
  const auto* entry = catalog_->Find(name);
  ApplyTagResult(tag_manager_.AddTag(window_, entry, std::move(tag)), name);
}
void MainWindow::AddNewTagToSelected() {
  const auto name = tree_view_.SelectedName();
  const auto* entry = !settings_.simple_mode && catalog_ ? catalog_->Find(name) : nullptr;
  ApplyTagResult(tag_manager_.AddNewTag(window_, entry), name);
}
void MainWindow::ApplyTagResult(TagManager::Result result, std::wstring_view selected) {
  if (result.changed) {
    RefreshTagFilter();
    PopulateTree();
    if (!selected.empty()) static_cast<void>(tree_view_.SelectItem(selected));
  }
  if (!result.status.empty()) SetStatus(std::move(result.status));
}
void MainWindow::DeleteSelected() {
  if (!catalog_) return;
  const auto name = tree_view_.SelectedName();
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
      static_cast<void>(catalog_state_.RemoveTags(tagId));
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
  const auto name = tree_view_.SelectedName();
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
  const auto name = tree_view_.SelectedName();
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
  if (cache_operation_.active()) {
    SetStatus(L"Операция с кэшем уже выполняется…");
    return;
  }
  try {
    const auto database = catalog_->DatabaseFor(tree_view_.SelectedName());
    cache_operation_.StartFinding(database, window_, kCacheOperationFinishedMessage);
    DisplaySelected();
    SetStatus(L"Анализируем размер кэша…");
    static_cast<void>(RefreshBackgroundPolling());
  } catch (const std::exception& error) {
    DisplaySelected();
    logger_.Error(L"Ошибка подготовки очистки кэша: " + WideErrorText(error.what()));
    Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  } catch (...) {
    DisplaySelected();
    Message(window_, L"Выберите базу для очистки кэша.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  }
}
bool MainWindow::IsClearingCache() const {
  return cache_operation_.clearing();
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
void MainWindow::CreateShortcut() { if (!catalog_) return; try { const auto database = catalog_->DatabaseFor(tree_view_.SelectedName()); shell::CreateDesktopShortcut(executable_, database.id, database.name); Message(window_, L"Ярлык создан на рабочем столе."); } catch (...) { Message(window_, L"Не удалось создать ярлык.", L"ИБ Старт", MB_OK | MB_ICONERROR); } }
void MainWindow::RefreshFileMenu() {
  menus_.RefreshFile(settings_);
}
void MainWindow::RefreshMainMenuBar() {
  menus_.RefreshMain(settings_);
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
  auto loadedSettings = settings_;
  std::optional<catalog::CatalogSession> loadedSession;
  try {
    loadedSession.emplace(catalog::LoadSession(path, loadedSettings.platform_search_paths));
    if (!loadedSession->loaded) throw std::runtime_error("The selected ibases.v8i file is no longer available.");
    loadedSettings.active_ibases = path;
    loadedSettings.selected_entry.clear();
    RememberRecentList(loadedSettings, path);
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка загрузки списка " + path.wstring() + L": " + ibstart::utf::FromUtf8(error.what()));
    Message(window_, L"Не удалось открыть выбранный список баз. Текущий список и активный путь не изменены. Проверьте формат и кодировку UTF-8.",
        L"ИБ Старт", MB_OK | MB_ICONERROR);
    return false;
  }

  store_ = std::move(loadedSession->store);
  catalog_ = std::move(loadedSession->catalog);
  platforms_ = std::move(loadedSession->platforms);
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
  if (menus_.view_menu()) CheckMenuItem(menus_.view_menu(), kShowTagsInList,
      MF_BYCOMMAND | (settings_.show_tags_in_list ? MF_CHECKED : MF_UNCHECKED));
  RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
void MainWindow::SetStatus(std::wstring text) { if (status_) SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); }
std::wstring MainWindow::CatalogStatistics() const { return L"Баз: " + std::to_wstring(catalog_ ? catalog_->Databases().size() : 0) + L" | Платформ: " + std::to_wstring(platforms_.size()); }
void MainWindow::SetSimpleMode(bool enabled) {
  const std::wstring selected = tree_view_.SelectedName();
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
  if (!selected.empty()) static_cast<void>(tree_view_.SelectItem(selected));
  DisplaySelected();
  RECT client{};
  if (GetClientRect(window_, &client)) Layout(client.right - client.left, client.bottom - client.top);
  if (tree_) RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
void MainWindow::ToggleFavorite() {
  if (!catalog_) return;
  const auto name = tree_view_.SelectedName();
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
  if (tree_view_.SelectItem(name)) LaunchSelected(domain::LaunchMode::enterprise);
}
void MainWindow::CheckForUpdates() {
  if (update_check_.active()) {
    SetStatus(L"Проверка обновлений уже выполняется…");
    return;
  }
  EnableMenuItem(menus_.help_menu(), kCheckForUpdates, MF_BYCOMMAND | MF_GRAYED);
  DrawMenuBar(window_);
  SetStatus(L"Проверяем наличие обновлений…");
  try {
    update_check_.Start(window_, kUpdateCheckFinishedMessage);
    static_cast<void>(RefreshBackgroundPolling());
  } catch (const std::exception& error) {
    EnableMenuItem(menus_.help_menu(), kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
    DrawMenuBar(window_);
    logger_.Error(L"Не удалось запустить проверку обновлений: " + WideErrorText(error.what()));
    SetStatus(L"Не удалось запустить проверку обновлений.");
    Message(window_, L"Не удалось запустить фоновую проверку обновлений.", L"Проверка обновлений", MB_OK | MB_ICONERROR);
  } catch (...) {
    EnableMenuItem(menus_.help_menu(), kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
    DrawMenuBar(window_);
    SetStatus(L"Не удалось запустить проверку обновлений.");
    Message(window_, L"Не удалось запустить фоновую проверку обновлений.", L"Проверка обновлений", MB_OK | MB_ICONERROR);
  }
}
void MainWindow::CompleteUpdateCheck() {
  auto completed = update_check_.TakeResult();
  if (!completed) return;
  auto release = std::move(completed->release);
  auto error = std::move(completed->error);
  const bool cancelled = completed->cancelled;
  static_cast<void>(RefreshBackgroundPolling());
  if (closing_) {
    TryFinishClose();
    return;
  }
  EnableMenuItem(menus_.help_menu(), kCheckForUpdates, MF_BYCOMMAND | MF_ENABLED);
  DrawMenuBar(window_);
  if (cancelled) {
    SetStatus(L"Проверка обновлений отменена.");
    return;
  }
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
  const auto open_result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(window_, L"open", release->page_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (open_result <= 32) {
    logger_.Error(L"Не удалось открыть страницу релиза: " + release->page_url);
    Message(window_, L"Не удалось открыть страницу релиза в браузере.", L"Проверка обновлений", MB_OK | MB_ICONWARNING);
  }
}
void MainWindow::CompleteCacheOperation() {
  auto completed = cache_operation_.TakeResult();
  if (!completed) return;

  static_cast<void>(RefreshBackgroundPolling());
  if (closing_) {
    TryFinishClose();
    return;
  }
  if (completed->cancelled) {
    DisplaySelected();
    SetStatus(L"Анализ кэша отменён.");
    return;
  }

  if (completed->stage == background::CacheClearOperation::Stage::finding) {
    if (!completed->error.empty()) {
      DisplaySelected();
      logger_.Error(L"Ошибка анализа кэша: " + completed->error);
      SetStatus(L"Не удалось проанализировать кэш.");
      Message(window_, L"Не удалось проанализировать кэш. Подробности — в журнале.", L"Очистка кэша", MB_OK | MB_ICONERROR);
      return;
    }
    if (completed->candidates.empty()) {
      DisplaySelected();
      SetStatus(L"Безопасных каталогов кэша для этой базы не найдено.");
      Message(window_, L"Безопасных каталогов кэша для этой базы не найдено.");
      return;
    }

    uintmax_t totalBytes = 0;
    std::wstring list = L"Будут очищены только следующие каталоги кэша:\n";
    for (const auto& item : completed->candidates) {
      totalBytes += item.bytes;
      list += item.path.wstring() + L" — " + cache::FormatSize(item.bytes) + L"\n";
    }
    list += L"\nПримерный объём для очистки: " + cache::FormatSize(totalBytes) + L".\n";
    if (cache::HasActiveOneCProcess()) list += L"\nОбнаружен активный процесс 1С. Закройте его перед очисткой.\n";
    if (MessageBoxW(window_, list.c_str(), L"Очистка кэша", MB_YESNO | MB_ICONWARNING) != IDYES) {
      DisplaySelected();
      SetStatus(L"Очистка кэша отменена.");
      return;
    }

    SetStatus(L"Очищаем кэш…");
    try {
      cache_operation_.StartClearing(
          std::move(completed->candidates), window_, kCacheOperationFinishedMessage);
      static_cast<void>(RefreshBackgroundPolling());
    } catch (const std::exception& exception) {
      DisplaySelected();
      logger_.Error(L"Не удалось запустить очистку кэша: " + WideErrorText(exception.what()));
      SetStatus(L"Не удалось запустить очистку кэша.");
      Message(window_, L"Не удалось запустить фоновую очистку кэша.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    } catch (...) {
      DisplaySelected();
      SetStatus(L"Не удалось запустить очистку кэша.");
      Message(window_, L"Не удалось запустить фоновую очистку кэша.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    }
    return;
  }

  DisplaySelected();
  if (!completed->error.empty()) {
    logger_.Error(L"Ошибка очистки кэша: " + completed->error);
    SetStatus(L"Не удалось очистить кэш.");
    Message(window_, L"Не удалось очистить кэш. Подробности — в журнале.", L"Очистка кэша", MB_OK | MB_ICONERROR);
    return;
  }
  const auto size = cache::FormatSize(completed->clear_result.bytes);
  logger_.Info(L"Очистка кэша: файлов=" + std::to_wstring(completed->clear_result.files) + L", байт=" +
      std::to_wstring(completed->clear_result.bytes) + L" (" + size + L")");
  for (const auto& item : completed->clear_result.errors) logger_.Error(L"Ошибка очистки кэша: " + item);
  SetStatus(completed->clear_result.errors.empty() ? L"Кэш очищен." : L"Кэш очищен с ошибками.");
  const std::wstring text = L"Очищено файлов: " + std::to_wstring(completed->clear_result.files) +
      L"\nОсвобождено: " + size + (completed->clear_result.errors.empty()
          ? L"" : L"\n\nНе удалось очистить некоторые каталоги. Подробности — в журнале.");
  Message(window_, text, L"Очистка кэша", completed->clear_result.errors.empty()
      ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
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
