#include "core/platform/platform_discovery.hpp"
#include "core/platform/platform_version.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <set>
#include <vector>

namespace ibstart::platform {
namespace {

std::wstring Environment(std::wstring_view name) {
  const DWORD required = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
  if (required == 0) return {};
  std::wstring value(required, L'\0');
  if (GetEnvironmentVariableW(std::wstring(name).c_str(), value.data(), required) == 0) return {};
  value.resize(required - 1);
  return value;
}

std::optional<domain::ClientBitness> ExecutableBitness(const std::filesystem::path& executable) {
  DWORD binaryType{};
  if (!GetBinaryTypeW(executable.c_str(), &binaryType)) return std::nullopt;
  if (binaryType == SCS_32BIT_BINARY) return domain::ClientBitness::x86;
  if (binaryType == SCS_64BIT_BINARY) return domain::ClientBitness::x64;
  return std::nullopt;
}

void AddClient(const std::filesystem::path& executable, std::vector<domain::PlatformInstallation>& output,
    std::set<std::wstring>& known, std::optional<domain::ClientBitness> bitness = std::nullopt) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(executable, error)) return;
  const auto canonical = std::filesystem::weakly_canonical(executable, error);
  auto key = (error ? executable : canonical).wstring();
  std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  if (!known.insert(key).second) return;
  const auto bin = executable.parent_path();
  const auto install = bin.parent_path();
  const auto version = install.filename().wstring();
  auto normalizedPath = executable.wstring();
  std::transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  const bool x86Path = normalizedPath.find(L"(x86)") != std::wstring::npos;
  const auto detectedBitness = ExecutableBitness(executable).value_or(
      bitness.value_or(x86Path ? domain::ClientBitness::x86 : domain::ClientBitness::x64));
  error.clear();
  output.push_back({executable, version, detectedBitness,
      executable.filename() == L"1cv8c.exe" || std::filesystem::exists(bin / L"1cv8c.exe", error)});
}

// A separate installation of the thin client contains 1cv8c.exe but not
// 1cv8.exe.  It is sufficient for web bases, so it must participate in the
// same discovery flow as a full platform installation.
void AddInstallation(const std::filesystem::path& directory, std::vector<domain::PlatformInstallation>& output,
    std::set<std::wstring>& known, std::optional<domain::ClientBitness> bitness = std::nullopt) {
  const auto thick = directory / L"1cv8.exe";
  std::error_code error;
  if (std::filesystem::is_regular_file(thick, error)) {
    AddClient(thick, output, known, bitness);
    return;
  }
  error.clear();
  AddClient(directory / L"1cv8c.exe", output, known, bitness);
}

void ScanRoot(const std::filesystem::path& root, std::vector<domain::PlatformInstallation>& output, std::set<std::wstring>& known) {
  std::error_code error;
  if (!std::filesystem::exists(root, error)) return;
  if (root.filename() == L"1cv8.exe" || root.filename() == L"1cv8c.exe") AddInstallation(root.parent_path(), output, known);
  AddInstallation(root, output, known);
  AddInstallation(root / L"bin", output, known);
  error.clear();
  for (std::filesystem::directory_iterator firstIt(root, std::filesystem::directory_options::skip_permission_denied, error), end; firstIt != end; firstIt.increment(error)) {
    if (error) { error.clear(); continue; }
    const auto& first = *firstIt;
    if (!first.is_directory(error)) { error.clear(); continue; }
    AddInstallation(first.path() / L"bin", output, known);
    error.clear();
    for (std::filesystem::directory_iterator secondIt(first.path(), std::filesystem::directory_options::skip_permission_denied, error), secondEnd; secondIt != secondEnd; secondIt.increment(error)) {
      if (error) { error.clear(); continue; }
      const auto& second = *secondIt;
      if (!second.is_directory(error)) { error.clear(); continue; }
      AddInstallation(second.path() / L"bin", output, known);
    }
    error.clear();
  }
}

std::optional<std::filesystem::path> RegistryInstallLocation(HKEY key) {
  DWORD type{};
  DWORD bytes{};
  if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) return std::nullopt;
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS) return std::nullopt;
  std::wstring value(buffer.data());
  if (type == REG_EXPAND_SZ) {
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring expanded(required, L'\0');
    if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) == 0) return std::nullopt;
    expanded.resize(required - 1);
    value = std::move(expanded);
  }
  return std::filesystem::path(value);
}

void ScanRegistry(HKEY hive, REGSAM view, std::vector<domain::PlatformInstallation>& output, std::set<std::wstring>& known) {
  HKEY root{};
  if (RegOpenKeyExW(hive, L"SOFTWARE\\1C\\1Cv8", 0, KEY_READ | view, &root) != ERROR_SUCCESS) return;
  for (DWORD firstIndex = 0;; ++firstIndex) {
    wchar_t firstName[256]; DWORD firstLength = 256;
    if (RegEnumKeyExW(root, firstIndex, firstName, &firstLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY version{};
    if (RegOpenKeyExW(root, firstName, 0, KEY_READ | view, &version) != ERROR_SUCCESS) continue;
    const auto bitness = view == KEY_WOW64_32KEY ? domain::ClientBitness::x86 : domain::ClientBitness::x64;
    if (const auto location = RegistryInstallLocation(version)) { AddInstallation(*location, output, known, bitness); AddInstallation(*location / L"bin", output, known, bitness); }
    for (DWORD secondIndex = 0;; ++secondIndex) {
      wchar_t secondName[256]; DWORD secondLength = 256;
      if (RegEnumKeyExW(version, secondIndex, secondName, &secondLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      HKEY install{};
      if (RegOpenKeyExW(version, secondName, 0, KEY_READ | view, &install) != ERROR_SUCCESS) continue;
      if (const auto location = RegistryInstallLocation(install)) { AddInstallation(*location, output, known, bitness); AddInstallation(*location / L"bin", output, known, bitness); }
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
  ScanRegistry(HKEY_CURRENT_USER, KEY_WOW64_32KEY, result, known);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return IsNewerVersion(left.version, right.version); });
  return result;
}

}  // namespace ibstart::platform
