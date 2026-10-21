#include "ui/main_window.hpp"
#include "ui/application_settings_dialog.hpp"
#include "ui/command_ids.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/update/github_release_client.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
enum class Scenario { disabled, newer, current, older, missing, error, malformed, close_pending, manual };
Scenario scenario{};
int failures{}, phase{}, ticks{};
std::atomic<int> requests{};
std::atomic<bool> cancelled{};
std::mutex response_mutex;
std::condition_variable_any response_ready;
bool release_response{};
std::filesystem::path captures;
bool settings_initial{}, settings_accept{};

void Check(bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAILED " << description << " (scenario=" << static_cast<int>(scenario)
        << ", phase=" << phase << ", ticks=" << ticks << ", requests=" << requests << ")\n";
    ++failures;
  }
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
std::wstring MenuText(HMENU menu, int position) {
  wchar_t text[256]{}; GetMenuStringW(menu, static_cast<UINT>(position), text, 256, MF_BYPOSITION);
  return text;
}
bool Busy(HWND main) {
  const auto menu = GetMenu(main);
  // The ordinary main menu always ends with Help unless an update is available.
  const auto help = GetSubMenu(menu, 4);
  return (GetMenuState(help, ibstart::ui::commands::kCheckForUpdates, MF_BYCOMMAND) & MF_GRAYED) != 0;
}
bool HasUpdate(HWND main) {
  return GetMenuState(GetMenu(main), ibstart::ui::commands::kOpenAvailableUpdate, MF_BYCOMMAND) != static_cast<UINT>(-1);
}
void Capture(HWND window, const wchar_t* name) {
  if (captures.empty()) return;
  RECT rect{}; GetWindowRect(window, &rect);
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  const HDC screen = GetDC(window), dc = CreateCompatibleDC(screen);
  BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  void* pixels{};
  const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
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
    Check(stream.good(), "Write window evidence");
    SelectObject(dc, previous); DeleteObject(bitmap);
  } else Check(false, "Allocate screenshot");
  DeleteDC(dc); ReleaseDC(window, screen);
}
void ReleaseResponse() {
  { std::lock_guard lock(response_mutex); release_response = true; }
  response_ready.notify_all();
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
  if (++ticks > 300) { Check(false, "Update scenario timeout"); PostQuitMessage(1); return; }
  const auto main = Window(L"IBStart.MainWindow");
  if (!main) return;
  using namespace ibstart::ui::commands;
  const auto dialog = Window(L"#32770");
  if (dialog) {
    Check(scenario == Scenario::manual && phase == 2, "Only a manual check opens a result dialog");
    phase = 3;
    SendMessageW(dialog, WM_COMMAND, IDNO, 0);
    return;
  }
  if (phase == 0 && (scenario == Scenario::disabled || scenario == Scenario::manual)) {
    if (ticks < 3) return;
    Check(requests == 0, "Disabled startup makes no transport call");
    Check(!HasUpdate(main), "No indicator without a successful check");
    if (scenario == Scenario::disabled) {
      phase = 4; SendMessageW(main, WM_CLOSE, 0, 0); return;
    }
    phase = 1; SendMessageW(main, WM_COMMAND, kCheckForUpdates, 0); return;
  }
  if ((phase == 0 || phase == 1) && requests == 1) {
    Check(Busy(main), "Manual check disabled during an active request");
    if (scenario == Scenario::close_pending) {
      phase = 4; SendMessageW(main, WM_CLOSE, 0, 0); return;
    }
    SendMessageW(main, WM_COMMAND, kSimpleMode, 0);
    Check(GetMenuItemCount(GetMenu(main)) == 1, "Simple mode has no pending indicator");
    SendMessageW(main, WM_COMMAND, kSimpleMode, 0);
    Check(Busy(main), "Menu rebuild preserves the active-request lock");
    if (scenario == Scenario::manual) SendMessageW(main, WM_COMMAND, kCheckForUpdates, 0);
    phase = 2; ReleaseResponse(); return;
  }
  if (phase == 2 && !Busy(main) && scenario != Scenario::manual) {
    Check(requests == 1, "Exactly one startup transport call");
    Check(HasUpdate(main) == (scenario == Scenario::newer), "Only a newer release creates the indicator");
    if (scenario == Scenario::newer) {
      Check(MenuText(GetMenu(main), 4) == L"Справка", "Indicator follows Help");
      Check(MenuText(GetMenu(main), 5) == L"Доступно обновление 99999.0.0", "Indicator shows the release version");
      Capture(main, L"update-main.bmp");
      SendMessageW(main, WM_COMMAND, kSimpleMode, 0);
      Check(MenuText(GetMenu(main), 0) == L"Режим", "Simple mode menu label");
      Check(MenuText(GetMenu(main), 1) == L"Доступно обновление 99999.0.0", "Simple mode preserves the update");
      Capture(main, L"update-simple.bmp");
      SendMessageW(main, WM_COMMAND, kSimpleMode, 0);
      Check(HasUpdate(main), "Returning to ordinary mode preserves the update");
    }
    phase = 4; SendMessageW(main, WM_CLOSE, 0, 0); return;
  }
  if (phase == 3) {
    Check(HasUpdate(main), "Declining the manual prompt preserves the update indicator");
    Check(requests == 1, "Manual check remains available when startup is disabled without duplicate requests");
    phase = 4; SendMessageW(main, WM_CLOSE, 0, 0);
  }
}
void CALLBACK DriveSettings(HWND, UINT, UINT_PTR, DWORD) {
  if (++ticks > 100) { Check(false, "Settings scenario timeout"); PostQuitMessage(1); return; }
  const auto window = Window(L"IBStart.ApplicationSettings");
  if (!window || phase) return;
  phase = 1;
  Check(GetWindowTextLengthW(window) > 0, "Settings window retains its native caption");
  const auto tabs = GetDlgItem(window, 1830);
  TabCtrl_SetCurSel(tabs, 1);
  NMHDR changed{tabs, 1830, TCN_SELCHANGE};
  SendMessageW(window, WM_NOTIFY, 1830, reinterpret_cast<LPARAM>(&changed));
  const auto checkbox = GetDlgItem(window, 1833);
  Check(IsWindowVisible(checkbox) != FALSE, "Startup update checkbox appears on Launch tab");
  Check((SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED) == settings_initial,
      "Checkbox reflects the stored value");
  if (settings_initial) Capture(window, L"update-settings.bmp");
  SendMessageW(checkbox, BM_CLICK, 0, 0);
  SendMessageW(window, WM_COMMAND, settings_accept ? IDOK : IDCANCEL, 0);
}
}  // namespace

// This test executable substitutes only the network boundary. The application's
// worker, release parser, version comparison, window messages and menus are real.
namespace ibstart::update::transport {
std::optional<std::string> FetchLatestVersionAsset(std::stop_token stop) {
  ++requests;
  std::unique_lock lock(response_mutex);
  response_ready.wait(lock, stop, [] { return release_response; });
  if (stop.stop_requested()) { cancelled = true; return std::nullopt; }
  switch (scenario) {
    case Scenario::current: return utf::ToUtf8(std::wstring(version::value));
    case Scenario::older: return "0.0.0";
    case Scenario::missing: return std::nullopt;
    case Scenario::error: throw std::runtime_error("Synthetic offline transport");
    case Scenario::malformed: return "invalid-version";
    default: return "99999.0.0";
  }
}
}  // namespace ibstart::update::transport

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--capture") {
    captures = argv[2]; std::filesystem::create_directories(captures);
  }
  const auto previous = GetThreadDesktop(GetCurrentThreadId());
  const auto name = L"IBStart.UpdateTests." + std::to_wstring(GetCurrentProcessId());
  const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
      DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) return 1;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES};
  InitCommonControlsEx(&controls);
  const auto root = std::filesystem::temp_directory_path() / (L"ibstart-updates-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(root);

  for (const auto test : {Scenario::newer, Scenario::disabled, Scenario::current, Scenario::older,
      Scenario::missing, Scenario::error, Scenario::malformed, Scenario::close_pending, Scenario::manual}) {
    scenario = test; phase = ticks = 0; requests = 0; cancelled = false;
    std::cout << "Running scenario " << static_cast<int>(test) << std::endl;
    { std::lock_guard lock(response_mutex); release_response = false; }
    ibstart::storage::Settings settings;
    settings.open_last_list_on_startup = false;
    settings.check_updates_on_startup = test != Scenario::disabled && test != Scenario::manual;
    settings.show_cache_size_in_list = false;
    const ibstart::storage::StorageLayout layout{root / std::to_wstring(static_cast<int>(test)), true};
    ibstart::storage::EnsureWritable(layout);
    ibstart::storage::SaveSettings(layout, settings);
    const auto timer = SetTimer(nullptr, 0, 20, Drive);
    {
      ibstart::ui::MainWindow application(GetModuleHandleW(nullptr), std::filesystem::path(argv[0]), layout);
      Check(application.Show(SW_SHOWNOACTIVATE) == 0, "Main window exits normally");
    }
    KillTimer(nullptr, timer);
    Check(phase == 4, "Application update scenario completed");
    if (test == Scenario::close_pending) Check(cancelled, "Closing cancels the pending request");
    if (failures) {
      std::cerr << "Preserving failed profile: " << ibstart::utf::ToUtf8(root.wstring()) << '\n';
      return 1;
    }
  }
  for (const auto initial : {true, false}) {
    for (const auto accept : {true, false}) {
      settings_initial = initial; settings_accept = accept; phase = ticks = 0;
      ibstart::storage::Settings settings; settings.check_updates_on_startup = initial;
      const auto timer = SetTimer(nullptr, 0, 20, DriveSettings);
      const auto result = ibstart::ui::dialog::EditApplicationSettings(nullptr, settings, root, true, {}, {}, {}, {}, {});
      KillTimer(nullptr, timer);
      Check(phase == 1, "Settings scenario completed");
      Check(result.has_value() == accept, "Cancel discards the settings draft");
      if (result) Check(result->settings.check_updates_on_startup == !initial, "OK collects the toggled checkbox");
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
  SetThreadDesktop(previous); CloseDesktop(desktop);
  std::error_code error; std::filesystem::remove_all(root, error);
  if (failures) return 1;
  std::cout << "All startup update contracts passed (isolated desktop/profile, substituted transport)\n";
  return 0;
}
