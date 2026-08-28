#include "core/shell/shortcut.hpp"

#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/windows_path.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <stdexcept>

namespace ibstart::shell {
namespace {
std::wstring SafeShortcutName(std::wstring_view displayName) {
  std::wstring result(displayName);
  constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
  for (auto& character : result) if (invalid.find(character) != std::wstring_view::npos || character < 0x20) character = L'_';
  while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) result.pop_back();
  if (result.empty()) result = L"Информационная база";
  return result;
}
}

void CreateDesktopShortcut(const std::filesystem::path& executable, std::wstring_view database_id, std::wstring_view display_name) {
  if (!windows_path::IsWithinLimit(executable)) {
    throw std::runtime_error("IBStart executable path is too long: " +
        utf::ToUtf8(windows_path::LengthError(executable)));
  }
  PWSTR desktop{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) throw std::runtime_error("Cannot find Desktop folder.");
  const std::filesystem::path target = std::filesystem::path(desktop) / (SafeShortcutName(display_name) + L" — IBStart.lnk");
  CoTaskMemFree(desktop);
  if (!windows_path::IsWithinLimit(target)) {
    throw std::runtime_error("Desktop shortcut path is too long: " +
        utf::ToUtf8(windows_path::LengthError(target)));
  }
  IShellLinkW* link{};
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) throw std::runtime_error("Cannot create Windows shortcut.");
  const std::wstring arguments = L"--launch-id " + launcher::QuoteWindowsArgument(database_id);
  HRESULT setup = link->SetPath(executable.c_str());
  if (SUCCEEDED(setup)) setup = link->SetArguments(arguments.c_str());
  if (SUCCEEDED(setup)) setup = link->SetDescription(L"Запуск базы через ИБ Старт");
  if (SUCCEEDED(setup)) setup = link->SetWorkingDirectory(executable.parent_path().c_str());
  if (FAILED(setup)) { link->Release(); throw std::runtime_error("Cannot configure Windows shortcut."); }
  IPersistFile* file{};
  const HRESULT query = link->QueryInterface(IID_PPV_ARGS(&file));
  if (SUCCEEDED(query)) { const HRESULT save = file->Save(target.c_str(), TRUE); file->Release(); link->Release(); if (FAILED(save)) throw std::runtime_error("Cannot save desktop shortcut."); }
  else { link->Release(); throw std::runtime_error("Cannot create shortcut persistence interface."); }
}
}  // namespace ibstart::shell
