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
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ibstart::ui {
namespace {
constexpr wchar_t kClassName[] = L"IBStart.MainWindow";
constexpr wchar_t kInputBoxClass[] = L"IBStart.InputBox";
constexpr UINT kActivateMessage = WM_APP + 23;
constexpr ULONG_PTR kLaunchCopyData = 0x49425354;
enum Command : int { kEnterprise = 100, kDesigner, kEdit, kCache, kShortcut, kDelete, kAddFile, kAddServer, kAddGroup, kOpenList, kRefresh, kSimpleMode, kToggleFavorite, kFocusSearch, kAbout, kMoveUp, kMoveDown, kFavorite1 = 200 };
enum TreeImage : int { kDatabaseImage, kFolderImage, kFavoriteImage, kRecentImage };

void Message(HWND owner, std::wstring_view text, std::wstring_view title = L"ИБ Старт", UINT type = MB_OK | MB_ICONINFORMATION) { MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type); }
HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}
HFONT CreateUiFont(HWND window, int points, LONG weight) {
  return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(window)), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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
std::wstring FriendlyFieldName(std::wstring_view key) {
  struct Label { std::wstring_view key; std::wstring_view text; };
  constexpr Label labels[] = {{L"Connect", L"Подключение"}, {L"ID", L"Идентификатор"}, {L"Folder", L"Группа"},
      {L"OrderInList", L"Порядок"}, {L"Version", L"Версия платформы"}, {L"App", L"Приложение"},
      {L"DefaultApp", L"Приложение по умолчанию"}, {L"WA", L"Аутентификация ОС"}, {L"External", L"Внешняя"},
      {L"Locale", L"Локаль"}, {L"ClientConnectionSpeed", L"Скорость соединения"}, {L"AdditionalParameters", L"Доп. параметры"}};
  for (const auto& label : labels) if (CompareStringOrdinal(key.data(), static_cast<int>(key.size()), label.key.data(), static_cast<int>(label.key.size()), TRUE) == CSTR_EQUAL) return std::wstring(label.text);
  return std::wstring(key);
}
std::wstring ConnectionKind(std::wstring_view connect) {
  if (catalog::Catalog::IsWebConnection(connect)) return L"Веб-база";
  if (utf::FindNoCaseOrdinal(connect, L"File=") != std::wstring_view::npos) return L"Файловая информационная база";
  if (utf::FindNoCaseOrdinal(connect, L"Srvr=") != std::wstring_view::npos) return L"Серверная информационная база";
  return L"Информационная база";
}
std::wstring SingleLine(std::wstring value) {
  std::replace(value.begin(), value.end(), L'\r', L' '); std::replace(value.begin(), value.end(), L'\n', L' '); std::replace(value.begin(), value.end(), L'\t', L' '); return value;
}

struct InputState { HWND edit{}; std::optional<std::wstring> result; bool done{false}; };
LRESULT CALLBACK InputWindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* state = reinterpret_cast<InputState*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) { SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams)); return TRUE; }
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
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kInputBoxClass, std::wstring(title).c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
      CW_USEDEFAULT, CW_USEDEFAULT, 470, 150, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    EnableWindow(owner, TRUE);
    return std::nullopt;
  }
  CreateWindowW(L"STATIC", std::wstring(caption).c_str(), WS_CHILD | WS_VISIBLE, 14, 14, 430, 20, dialog, nullptr, nullptr, nullptr);
  state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::wstring(initial).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 14, 38, 430, 24, dialog, nullptr, nullptr, nullptr);
  CreateWindowW(L"BUTTON", L"ОК", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 275, 78, 80, 25, dialog, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
  CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 364, 78, 80, 25, dialog, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
  ShowWindow(dialog, SW_SHOW); SetFocus(state.edit);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) { if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  EnableWindow(owner, TRUE); SetForegroundWindow(owner); return state.result;
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
  if (windowX != CW_USEDEFAULT && windowY != CW_USEDEFAULT) {
    const RECT saved{windowX, windowY, windowX + settings_.window_width, windowY + settings_.window_height};
    if (!MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) { windowX = CW_USEDEFAULT; windowY = CW_USEDEFAULT; }
  }
  window_ = CreateWindowExW(0, kClassName, L"ИБ Старт — IBStart", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      windowX, windowY, settings_.window_width, settings_.window_height, nullptr, nullptr, instance_, this);
  if (!window_) return 1;
  ShowWindow(window_, show_command); UpdateWindow(window_);
  ACCEL accelerators[] = {{FVIRTKEY, VK_F3, kEnterprise}, {FVIRTKEY, VK_F4, kDesigner}, {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'F', kFocusSearch},
      {static_cast<BYTE>(FVIRTKEY | FALT), '1', kFavorite1}, {static_cast<BYTE>(FVIRTKEY | FALT), '2', kFavorite1 + 1}, {static_cast<BYTE>(FVIRTKEY | FALT), '3', kFavorite1 + 2},
      {static_cast<BYTE>(FVIRTKEY | FALT), '4', kFavorite1 + 3}, {static_cast<BYTE>(FVIRTKEY | FALT), '5', kFavorite1 + 4}, {static_cast<BYTE>(FVIRTKEY | FALT), '6', kFavorite1 + 5},
      {static_cast<BYTE>(FVIRTKEY | FALT), '7', kFavorite1 + 6}, {static_cast<BYTE>(FVIRTKEY | FALT), '8', kFavorite1 + 7}, {static_cast<BYTE>(FVIRTKEY | FALT), '9', kFavorite1 + 8}};
  HACCEL accelerator = CreateAcceleratorTableW(accelerators, 12);
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
      break;
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
  CreateWindowW(L"STATIC", L"Поиск:", WS_CHILD | WS_VISIBLE, 8, 10, 50, 20, window_, nullptr, instance_, nullptr);
  search_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 58, 7, 600, 25, window_, nullptr, instance_, nullptr);
  tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, 8, 42, 360, 420, window_, nullptr, instance_, nullptr);
  TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  details_title_ = CreateWindowW(L"STATIC", L"Выберите базу или группу", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 49, 460, 26, window_, nullptr, instance_, nullptr);
  details_subtitle_ = CreateWindowW(L"STATIC", L"Сведения появятся здесь", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
      390, 76, 460, 20, window_, nullptr, instance_, nullptr);
  details_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 380, 100, 480, 182, window_, nullptr, instance_, nullptr);
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

  tree_images_ = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 4, 1);
  if (tree_images_) {
    constexpr int resources[] = {IDI_TREE_DATABASE, IDI_TREE_FOLDER, IDI_ACTION_FAVORITE, IDI_ACTION_REFRESH};
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
  AttachButtonIcon(enterprise_, instance_, IDI_ACTION_ENTERPRISE, button_images_);
  AttachButtonIcon(designer_, instance_, IDI_ACTION_DESIGNER, button_images_);
  AttachButtonIcon(edit_, instance_, IDI_ACTION_EDIT, button_images_);
  AttachButtonIcon(cache_, instance_, IDI_ACTION_CACHE, button_images_);
  AttachButtonIcon(shortcut_, instance_, IDI_ACTION_SHORTCUT, button_images_);
  AttachButtonIcon(remove_, instance_, IDI_ACTION_DELETE, button_images_);
  status_ = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kOpenList, L"Открыть список баз…"); AppendMenuW(file, MF_STRING, kAddFile, L"Добавить файловую базу…"); AppendMenuW(file, MF_STRING, kAddServer, L"Добавить серверную базу…"); AppendMenuW(file, MF_STRING, kAddGroup, L"Добавить группу…"); AppendMenuW(file, MF_STRING, kRefresh, L"Обновить список");
  AppendMenuW(view, MF_STRING, kToggleFavorite, L"Добавить/убрать из избранного\tAlt+1…Alt+9");
  AppendMenuW(view, MF_STRING, kSimpleMode, L"Простой режим"); AppendMenuW(help, MF_STRING, kAbout, L"О программе…"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"Файл"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"Вид"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Справка"); SetMenu(window_, menu);
  SetSimpleMode(settings_.simple_mode);
  DisplaySelected();
}

void MainWindow::Layout(int width, int height) {
  const int statusHeight = 22, top = 42, bottom = statusHeight + 10, leftWidth = std::max(260, width * 42 / 100), rightX = leftWidth + 18, rightWidth = std::max(250, width - rightX - 10);
  MoveWindow(search_, 58, 7, std::max(100, width - 66), 25, TRUE); MoveWindow(tree_, 8, top, leftWidth, std::max(100, height - top - bottom), TRUE);
  const int cardHeight = std::max(100, height - top - 165);
  MoveWindow(details_title_, rightX + 10, top + 7, std::max(80, rightWidth - 20), 26, TRUE);
  MoveWindow(details_subtitle_, rightX + 10, top + 34, std::max(80, rightWidth - 20), 20, TRUE);
  MoveWindow(details_, rightX, top + 58, rightWidth, std::max(42, cardHeight - 58), TRUE);
  const int keyWidth = std::clamp(rightWidth * 35 / 100, 120, 190);
  ListView_SetColumnWidth(details_, 0, keyWidth);
  ListView_SetColumnWidth(details_, 1, std::max(80, rightWidth - keyWidth - 5));
  const int y = height - bottom - 82;
  MoveWindow(enterprise_, rightX, y, 150, 30, TRUE); MoveWindow(designer_, rightX + 159, y, 150, 30, TRUE); MoveWindow(edit_, rightX + 318, y, 105, 30, TRUE);
  MoveWindow(cache_, rightX, y + 36, 115, 30, TRUE); MoveWindow(shortcut_, rightX + 124, y + 36, 130, 30, TRUE); MoveWindow(remove_, rightX + 263, y + 36, 100, 30, TRUE);
  SendMessageW(status_, WM_SIZE, 0, 0);
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
    row.item.iImage = row.item.iSelectedImage = item.database ? kDatabaseImage : kFolderImage;
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
        row.item.iImage = row.item.iSelectedImage = kDatabaseImage; TreeView_InsertItem(tree_, &row); any = true;
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
  const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  const auto previous = SelectObject(context, font);
  SIZE size{};
  GetTextExtentPoint32W(context, item->text.c_str(), static_cast<int>(item->text.size()), &size);
  SelectObject(context, previous);
  ReleaseDC(window_, context);
  measure->itemHeight = 28;
  measure->itemWidth = std::max<UINT>(138, static_cast<UINT>(size.cx + 44));
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
  const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SelectObject(draw->hDC, font);
  SetBkMode(draw->hDC, TRANSPARENT);
  SetTextColor(draw->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : (disabled ? COLOR_GRAYTEXT : COLOR_MENUTEXT)));
  RECT textRect = draw->rcItem;
  textRect.left += 35;
  DrawTextW(draw->hDC, item->text.c_str(), static_cast<int>(item->text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
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
  const auto append = [&](bool enabled, bool checked, UINT command, int iconResource, std::wstring text) {
    ContextMenuItem visual{command, iconResource == 0 ? nullptr : LoadResourceIcon(instance_, iconResource, 20), std::move(text)};
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
  append(database, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие\tF3");
  append(database, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор\tF4");
  separator();
  append(database, favorite, kToggleFavorite, IDI_ACTION_FAVORITE, favorite ? L"Убрать из избранного" : L"Добавить в избранное");
  append(editable, false, kEdit, IDI_ACTION_EDIT, L"Изменить…");
  append(database && !settings_.simple_mode, false, kCache, IDI_ACTION_CACHE, L"Очистить кэш…");
  append(database && !settings_.simple_mode, false, kShortcut, IDI_ACTION_SHORTCUT, L"Создать ярлык");
  separator();
  append(editable, false, kMoveUp, 0, L"Переместить вверх");
  append(editable, false, kMoveDown, 0, L"Переместить вниз");
  append(editable, false, kDelete, IDI_ACTION_DELETE, L"Удалить…");
  separator();
  append(!settings_.simple_mode, false, kAddFile, IDI_ACTION_ADD, group ? L"Добавить файловую базу в группу…" : L"Добавить файловую базу…");
  append(!settings_.simple_mode, false, kAddServer, IDI_ACTION_ADD, group ? L"Добавить серверную базу в группу…" : L"Добавить серверную базу…");
  append(!settings_.simple_mode, false, kAddGroup, IDI_TREE_FOLDER, group ? L"Добавить вложенную группу…" : L"Добавить группу…");
  separator();
  append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список");

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
    domain::LaunchOptions options; options.mode = mode; if (database.version != L"" && database.version != L"Авто") options.version = database.version;
    const auto selected = launcher::SelectPlatform(platforms_, options); if (!selected) { Message(window_, L"Подходящая платформа 1С не найдена. Проверьте установку и настройки поиска.", L"ИБ Старт", MB_OK | MB_ICONERROR); return; }
    const auto parameters = database.additional_parameters; if (utf::FindNoCaseOrdinal(parameters, L"/p") != std::wstring_view::npos && MessageBoxW(window_, L"В дополнительных параметрах обнаружен /P. Пароль может храниться в открытом виде в ibases.v8i. Продолжить?", L"Предупреждение", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    const auto command = launcher::BuildCommand(database, *selected, options); logger_.Info(L"Запуск: " + command.CommandLine()); launcher::Launch(command); storage::AppendHistory(layout_, {database.id, std::chrono::system_clock::now(), mode}); SetStatus(L"Запущена база: " + database.name);
  } catch (const std::exception& error) { logger_.Error(L"Ошибка запуска: " + ibstart::utf::FromUtf8(error.what())); Message(window_, L"Не удалось запустить базу. Подробности — в последнем логе.", L"ИБ Старт", MB_OK | MB_ICONERROR); }
}

std::wstring MainWindow::NextName(std::wstring_view stem) const { for (unsigned number = 1;; ++number) { const auto candidate = std::wstring(stem) + L" " + std::to_wstring(number); if (!catalog_ || !catalog_->Find(candidate)) return candidate; } }
void MainWindow::AddFileDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  BROWSEINFOW info{}; info.hwndOwner = window_; info.lpszTitle = L"Выберите каталог файловой базы (с 1Cv8.1CD)";
  PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
  if (!id) return;
  wchar_t path[MAX_PATH]{}; const bool valid = SHGetPathFromIDListW(id, path); CoTaskMemFree(id);
  if (!valid) return;
  const std::filesystem::path directory(path);
  auto proposed = directory.filename().wstring();
  if (const auto entered = InputBox(window_, L"Добавить базу", L"Имя базы:", proposed)) proposed = *entered; else return;
  if (!catalog_->AddFileDatabase(proposed, directory, parent)) {
    Message(window_, L"Не удалось добавить базу. Каталог должен содержать 1Cv8.1CD, имя должно быть уникальным, а выбранная группа — существовать.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
    return;
  }
  SaveCatalog(); PopulateTree(); SelectTreeItem(proposed);
}
void MainWindow::AddServerDatabase(std::wstring parent) {
  if (settings_.simple_mode || !catalog_) return;
  const auto name = InputBox(window_, L"Серверная база", L"Имя базы:", NextName(L"Серверная база"));
  if (!name) return;
  const auto connect = InputBox(window_, L"Серверная база", L"Connect (например Srvr=\"server\";Ref=\"base\"):", L"");
  if (!connect) return;
  if (!catalog_->AddServerDatabase(*name, *connect, parent)) {
    Message(window_, L"Нужно уникальное имя, непустая строка Connect и существующая группа.", L"ИБ Старт", MB_OK | MB_ICONWARNING);
  } else {
    SaveCatalog(); PopulateTree(); SelectTreeItem(*name);
  }
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
void MainWindow::EditSelected() { if (settings_.simple_mode || !catalog_) return; const auto selected = SelectedName(); auto* entry = catalog_->Find(selected); if (!entry) return; const auto value = entry->IsDatabase() ? entry->ValueOr(L"Connect") : entry->name; const auto changed = InputBox(window_, L"Изменить", entry->IsDatabase() ? L"Строка Connect:" : L"Название группы:", value); if (!changed || changed->empty()) return; if (entry->IsDatabase()) entry->Set(L"Connect", *changed); else if (!catalog_->RenameGroup(selected, *changed)) { Message(window_, L"Имя уже используется.", L"ИБ Старт", MB_OK | MB_ICONWARNING); return; } SaveCatalog(); PopulateTree(); }
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
