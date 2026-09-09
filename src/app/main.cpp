#include "app/instance_activation.hpp"

#include "core/domain/utf.hpp"
#include "core/storage/storage.hpp"
#include "ui/main_window.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::filesystem::path ExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || static_cast<size_t>(length) >= path.size()) throw std::runtime_error("Cannot determine IBStart executable path.");
  path.resize(length);
  return path;
}
std::optional<std::wstring> ArgumentValue() {
  int count{}; LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count); std::optional<std::wstring> result;
  for (int index = 1; values && index + 1 < count; ++index) if (_wcsicmp(values[index], L"--launch-id") == 0) { result = values[index + 1]; break; }
  if (values) LocalFree(values); return result;
}
std::wstring ErrorText(const std::exception& error) noexcept {
  try { return ibstart::utf::FromUtf8(error.what()); }
  catch (...) {
    const std::string_view bytes(error.what());
    return std::wstring(bytes.begin(), bytes.end());
  }
}

HWND FindProfileWindow(std::uint64_t identity) {
  struct Search {
    std::uint64_t identity{};
    HWND window{};
  } search{identity};
  EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(parameter);
    wchar_t class_name[64]{};
    if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) == 0 ||
        _wcsicmp(class_name, L"IBStart.MainWindow") != 0) {
      return TRUE;
    }
    const auto value = reinterpret_cast<ULONG_PTR>(GetPropW(window, ibstart::app::kProfileIdentityProperty));
    if (value != static_cast<ULONG_PTR>(search.identity)) return TRUE;
    search.window = window;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.window;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
  if (!InitCommonControlsEx(&controls)) {
    MessageBoxW(nullptr, L"Не удалось инициализировать системные элементы управления.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return 1;
  }
  std::filesystem::path executable;
  ibstart::storage::StorageLayout layout;
  try {
    executable = ExecutablePath();
    layout = ibstart::storage::ResolveLayout(executable);
  } catch (const std::exception& error) {
    MessageBoxW(nullptr, (L"Не удалось определить профиль ИБ Старт.\n" + ErrorText(error)).c_str(),
        L"ИБ Старт", MB_OK | MB_ICONERROR);
    return 1;
  }

  const auto mutex_name = ibstart::storage::InstanceMutexName(layout);
  SetLastError(ERROR_SUCCESS);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (!mutex) {
    MessageBoxW(nullptr, L"Не удалось включить защиту от запуска нескольких экземпляров.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    return 1;
  }
  const bool alreadyExists = GetLastError() == ERROR_ALREADY_EXISTS;
  const auto launchId = ArgumentValue();
  if (launchId && !ibstart::app::IsValidLaunchIdLength(launchId->size())) {
    MessageBoxW(nullptr, L"Идентификатор базы в параметре --launch-id слишком длинный.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    CloseHandle(mutex);
    return 1;
  }
  if (alreadyExists) {
    HWND existing = nullptr;
    for (unsigned attempt = 0; attempt != 150 && !existing; ++attempt) {
      existing = FindProfileWindow(ibstart::storage::InstanceIdentity(layout));
      if (!existing) Sleep(10);
    }
    if (existing) {
      if (launchId) {
        COPYDATASTRUCT data{};
        data.dwData = ibstart::app::kLaunchCopyData;
        data.cbData = static_cast<DWORD>((launchId->size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(launchId->c_str());
        DWORD_PTR ignored{};
        SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 1000, &ignored);
      }
      PostMessageW(existing, WM_APP + 23, 0, 0);
    }
    CloseHandle(mutex);
    return existing ? 0 : 1;
  }
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
    MessageBoxW(nullptr, L"Не удалось инициализировать COM.", L"ИБ Старт", MB_OK | MB_ICONERROR);
    if (mutex) CloseHandle(mutex);
    return 1;
  }
  try {
    ibstart::storage::EnsureWritable(layout);
    ibstart::ui::MainWindow window(instance, executable, layout, launchId);
    const int result = window.Show(show_command);
    if (SUCCEEDED(apartment)) CoUninitialize();
    CloseHandle(mutex);
    return result;
  } catch (const std::exception& error) { MessageBoxW(nullptr, (L"Не удалось запустить ИБ Старт.\n" + ErrorText(error)).c_str(), L"ИБ Старт", MB_OK | MB_ICONERROR); if (SUCCEEDED(apartment)) CoUninitialize(); CloseHandle(mutex); return 1; }
  catch (...) { MessageBoxW(nullptr, L"Не удалось запустить ИБ Старт из-за неизвестной ошибки.", L"ИБ Старт", MB_OK | MB_ICONERROR); if (SUCCEEDED(apartment)) CoUninitialize(); CloseHandle(mutex); return 1; }
}
