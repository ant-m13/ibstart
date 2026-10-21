#include "ui/context_menu_controller.hpp"
#include "ui/command_ids.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
using namespace ibstart::ui;
using namespace ibstart::ui::commands;
ContextMenuController controller;
int failures{}, inspected{}, ticks{};
std::filesystem::path captures;
HFONT font{};

void Check(bool condition, const char* message) {
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void Inspect(HMENU menu) {
  for (int i = 0; i < GetMenuItemCount(menu); ++i) {
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_DATA | MIIM_FTYPE;
    if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) || !(info.fType & MFT_OWNERDRAW)) continue;
    const auto* item = reinterpret_cast<const OwnerDrawMenuItem*>(info.dwItemData);
    if (!item) continue;
    if (item->command == kTagsContextMenu) {
      Check(item->icon_kind == OwnerDrawMenuIcon::tag, "Tags submenu uses the tag icon across compiled modules");
      Check(item->icon_kind != OwnerDrawMenuIcon::credentials, "Tags never uses the credentials icon");
      ++inspected;
    } else if (item->command == kLaunchWithParameters) {
      Check(item->icon_kind == OwnerDrawMenuIcon::launch_parameters, "Launch parameters icon"); ++inspected;
    } else if (item->command == kMoveUp || item->command == kMoveDown) {
      Check(item->icon_kind == (item->command == kMoveUp ? OwnerDrawMenuIcon::move_up : OwnerDrawMenuIcon::move_down),
          "Move command icon"); ++inspected;
    }
  }
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_INITMENUPOPUP) Inspect(reinterpret_cast<HMENU>(wparam));
  if (message == WM_MEASUREITEM && controller.Measure(window, font, reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) return TRUE;
  if (message == WM_DRAWITEM && controller.Draw(font, reinterpret_cast<DRAWITEMSTRUCT*>(lparam))) return TRUE;
  return DefWindowProcW(window, message, wparam, lparam);
}

void Capture(HWND window) {
  if (captures.empty()) return;
  RECT rect{}; GetWindowRect(window, &rect);
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  const HDC screen = GetDC(window), dc = CreateCompatibleDC(screen);
  BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  void* pixels{};
  const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  Check(bitmap != nullptr, "Allocate popup capture");
  if (bitmap) {
    const auto previous = SelectObject(dc, bitmap);
    Check(PrintWindow(window, dc, 0) != FALSE, "Capture real context menu"); GdiFlush();
    const DWORD bytes = static_cast<DWORD>(width * height * 4);
    const BITMAPFILEHEADER header{0x4d42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bytes,
        0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    std::ofstream output(captures / L"tags-context-menu.bmp", std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), bytes);
    Check(output.good(), "Save popup capture");
    SelectObject(dc, previous); DeleteObject(bitmap);
  }
  DeleteDC(dc); ReleaseDC(window, screen);
}

void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
  if (++ticks > 50) { Check(false, "Context menu timeout"); EndMenu(); return; }
  EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (std::wstring_view(name) != L"#32768") return TRUE;
    Capture(window); EndMenu(); return FALSE;
  }, 0);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--capture") {
    captures = argv[2]; std::filesystem::create_directories(captures);
  }
  const auto previous = GetThreadDesktop(GetCurrentThreadId());
  const auto name = L"IBStart.ContextMenuTests." + std::to_wstring(GetCurrentProcessId());
  const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
      DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) return 1;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
  WNDCLASSW klass{}; klass.lpfnWndProc = Proc; klass.hInstance = GetModuleHandleW(nullptr);
  klass.lpszClassName = L"IBStart.ContextMenuIconTests";
  RegisterClassW(&klass);
  font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  const HWND window = CreateWindowW(klass.lpszClassName, L"Context menu", WS_OVERLAPPEDWINDOW,
      10, 10, 500, 500, nullptr, nullptr, klass.hInstance, nullptr);
  Check(window != nullptr, "Create menu owner");
  if (window) {
    ShowWindow(window, SW_SHOWNOACTIVATE); controller.Create(klass.hInstance);
    TreeContextMenuState state; state.database = state.editable = state.launch_available = true;
    state.cache_operation_available = state.file = true;
    const auto timer = SetTimer(nullptr, 0, 100, Drive);
    static_cast<void>(controller.ShowTree(window, {30, 30}, state));
    KillTimer(nullptr, timer);
    Check(inspected == 4, "Inspected tags, launch parameters and both move commands");
    DestroyWindow(window);
  }
  controller.Clear(); SetThreadDesktop(previous); CloseDesktop(desktop);
  if (failures) return 1;
  std::cout << "Context menu icon contracts passed (isolated desktop, real native popup)\n";
  return 0;
}
