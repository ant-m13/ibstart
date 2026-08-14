#include "core/domain/utf.hpp"
#include "core/storage/storage.hpp"
#include "ui/main_window.hpp"

#include <Windows.h>
#include <CommCtrl.h>

#include <filesystem>
#include <optional>
#include <string>

namespace {
std::filesystem::path ExecutablePath() {
  std::wstring path(32768, L'\0'); const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())); path.resize(length); return path;
}
std::optional<std::wstring> ArgumentValue() {
  int count{}; LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count); std::optional<std::wstring> result;
  for (int index = 1; values && index + 1 < count; ++index) if (_wcsicmp(values[index], L"--launch-id") == 0) { result = values[index + 1]; break; }
  if (values) LocalFree(values); return result;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\IBStart.SingleInstance.0.1");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { if (const HWND existing = FindWindowW(L"IBStart.MainWindow", nullptr)) PostMessageW(existing, WM_APP + 23, 0, 0); if (mutex) CloseHandle(mutex); return 0; }
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    const auto executable = ExecutablePath(); const auto layout = ibstart::storage::ResolveLayout(executable); ibstart::storage::EnsureWritable(layout); const auto settings = ibstart::storage::LoadSettings(layout);
    ibstart::ui::MainWindow window(instance, executable, layout, settings, ArgumentValue()); const int result = window.Show(show_command); if (SUCCEEDED(apartment)) CoUninitialize(); if (mutex) CloseHandle(mutex); return result;
  } catch (const std::exception& error) { MessageBoxW(nullptr, (L"Не удалось запустить ИБ Старт.\n" + ibstart::utf::FromUtf8(error.what())).c_str(), L"ИБ Старт", MB_OK | MB_ICONERROR); if (SUCCEEDED(apartment)) CoUninitialize(); if (mutex) CloseHandle(mutex); return 1; }
}
