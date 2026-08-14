#include "core/shell/shortcut.hpp"

#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <stdexcept>

namespace ibstart::shell {
void CreateDesktopShortcut(const std::filesystem::path& executable, std::wstring_view database_id, std::wstring_view display_name) {
  PWSTR desktop{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) throw std::runtime_error("Cannot find Desktop folder.");
  const std::filesystem::path target = std::filesystem::path(desktop) / (std::wstring(display_name) + L" — IBStart.lnk");
  CoTaskMemFree(desktop);
  IShellLinkW* link{};
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) throw std::runtime_error("Cannot create Windows shortcut.");
  link->SetPath(executable.c_str());
  const std::wstring arguments = L"--launch-id " + launcher::QuoteWindowsArgument(database_id);
  link->SetArguments(arguments.c_str());
  link->SetDescription(L"Запуск базы через ИБ Старт");
  IPersistFile* file{};
  const HRESULT query = link->QueryInterface(IID_PPV_ARGS(&file));
  if (SUCCEEDED(query)) { const HRESULT save = file->Save(target.c_str(), TRUE); file->Release(); link->Release(); if (FAILED(save)) throw std::runtime_error("Cannot save desktop shortcut."); }
  else { link->Release(); throw std::runtime_error("Cannot create shortcut persistence interface."); }
}
}  // namespace ibstart::shell
