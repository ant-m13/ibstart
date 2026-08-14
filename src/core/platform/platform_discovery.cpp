#include "core/platform/platform_discovery.hpp"

#include <Windows.h>

#include <algorithm>
#include <set>

namespace ibstart::platform {
namespace {

std::wstring Environment(std::wstring_view name) {
  const DWORD required = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
  if (required == 0) return {};
  std::wstring value(required, L'\0');
  GetEnvironmentVariableW(std::wstring(name).c_str(), value.data(), required);
  value.resize(required - 1);
  return value;
}

void AddExe(const std::filesystem::path& executable, std::vector<domain::PlatformInstallation>& output, std::set<std::wstring>& known) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(executable, error)) return;
  const auto canonical = std::filesystem::weakly_canonical(executable, error);
  const auto key = (error ? executable : canonical).wstring();
  if (!known.insert(key).second) return;
  const auto bin = executable.parent_path();
  const auto install = bin.parent_path();
  const auto version = install.filename().wstring();
  const bool x86 = install.wstring().find(L"(x86)") != std::wstring::npos || executable.wstring().find(L"(x86)") != std::wstring::npos;
  output.push_back({executable, version, x86 ? domain::ClientBitness::x86 : domain::ClientBitness::x64,
      std::filesystem::exists(bin / L"1cv8c.exe", error)});
}

void ScanRoot(const std::filesystem::path& root, std::vector<domain::PlatformInstallation>& output, std::set<std::wstring>& known) {
  std::error_code error;
  if (!std::filesystem::exists(root, error)) return;
  if (root.filename() == L"bin") AddExe(root / L"1cv8.exe", output, known);
  for (const auto& first : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error)) {
    if (error || !first.is_directory(error)) continue;
    AddExe(first.path() / L"bin" / L"1cv8.exe", output, known);
    for (const auto& second : std::filesystem::directory_iterator(first.path(), std::filesystem::directory_options::skip_permission_denied, error)) {
      if (error || !second.is_directory(error)) continue;
      AddExe(second.path() / L"bin" / L"1cv8.exe", output, known);
    }
  }
}

void ScanRegistry(HKEY hive, REGSAM view, std::vector<domain::PlatformInstallation>& output, std::set<std::wstring>& known) {
  HKEY root{};
  if (RegOpenKeyExW(hive, L"SOFTWARE\\1C\\1Cv8", 0, KEY_READ | view, &root) != ERROR_SUCCESS) return;
  for (DWORD firstIndex = 0;; ++firstIndex) {
    wchar_t firstName[256]; DWORD firstLength = 256;
    if (RegEnumKeyExW(root, firstIndex, firstName, &firstLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY version{};
    if (RegOpenKeyExW(root, firstName, 0, KEY_READ | view, &version) != ERROR_SUCCESS) continue;
    for (DWORD secondIndex = 0;; ++secondIndex) {
      wchar_t secondName[256]; DWORD secondLength = 256;
      if (RegEnumKeyExW(version, secondIndex, secondName, &secondLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      HKEY install{};
      if (RegOpenKeyExW(version, secondName, 0, KEY_READ | view, &install) != ERROR_SUCCESS) continue;
      wchar_t location[MAX_PATH * 4]; DWORD bytes = sizeof(location); DWORD type{};
      if (RegQueryValueExW(install, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(location), &bytes) == ERROR_SUCCESS &&
          (type == REG_SZ || type == REG_EXPAND_SZ)) {
        AddExe(std::filesystem::path(location) / L"bin" / L"1cv8.exe", output, known);
      }
      RegCloseKey(install);
    }
    RegCloseKey(version);
  }
  RegCloseKey(root);
}

}  // namespace

std::vector<std::filesystem::path> StandardSearchRoots() {
  std::vector<std::filesystem::path> roots;
  const auto pf = Environment(L"ProgramFiles");
  const auto pfx86 = Environment(L"ProgramFiles(x86)");
  const auto local = Environment(L"LOCALAPPDATA");
  if (!pf.empty()) { roots.emplace_back(std::filesystem::path(pf) / L"1cv8"); roots.emplace_back(std::filesystem::path(pf) / L"1C"); }
  if (!pfx86.empty()) { roots.emplace_back(std::filesystem::path(pfx86) / L"1cv8"); roots.emplace_back(std::filesystem::path(pfx86) / L"1C"); }
  if (!local.empty()) roots.emplace_back(std::filesystem::path(local) / L"Programs" / L"1C");
  return roots;
}

std::vector<domain::PlatformInstallation> Discover(const std::vector<std::filesystem::path>& user_roots) {
  std::vector<domain::PlatformInstallation> result;
  std::set<std::wstring> known;
  auto roots = StandardSearchRoots();
  roots.insert(roots.end(), user_roots.begin(), user_roots.end());
  for (const auto& root : roots) ScanRoot(root, result, known);
  ScanRegistry(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, result, known);
  ScanRegistry(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, result, known);
  ScanRegistry(HKEY_CURRENT_USER, KEY_WOW64_64KEY, result, known);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.version > right.version; });
  return result;
}

}  // namespace ibstart::platform
