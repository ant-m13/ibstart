#include "core/cache/cache_service.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace ibstart::cache {
namespace {

std::wstring Env(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring text(size, L'\0');
  if (GetEnvironmentVariableW(name, text.data(), size) == 0) return {};
  text.resize(size - 1);
  return text;
}

uintmax_t SizeOf(const std::filesystem::path& root, std::stop_token stop = {}) {
  uintmax_t result = 0; std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
    if (stop.stop_requested()) return 0;
    if (error) { error.clear(); continue; }
    if (it->is_regular_file(error)) {
      const auto size = it->file_size(error);
      if (!error) result += size;
    }
    error.clear();
  }
  return result;
}

std::optional<std::wstring> SafeIdentifier(std::wstring value) {
  if (value.empty() || value == L"." || value == L".." || value.back() == L'.' || value.back() == L' ') return std::nullopt;
  constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
  if (std::any_of(value.begin(), value.end(), [&](wchar_t character) {
        return character < 0x20 || invalid.find(character) != std::wstring_view::npos;
      })) {
    return std::nullopt;
  }
  return value;
}

std::wstring NormalizedLower(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error).wstring();
  if (error) normalized = path.lexically_normal().wstring();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return normalized;
}

std::vector<std::filesystem::path> AllowedCacheRoots() {
  std::vector<std::filesystem::path> roots;
  const auto roaming = Env(L"APPDATA");
  const auto local = Env(L"LOCALAPPDATA");
  if (!roaming.empty()) roots.push_back(std::filesystem::path(roaming) / L"1C" / L"1Cv8");
  if (!local.empty()) {
    roots.push_back(std::filesystem::path(local) / L"1C" / L"1Cv8");
    roots.push_back(std::filesystem::path(local) / L"IBStart" / L"cache");
  }
  return roots;
}

// The <id> folder under the 1C roots may shadow licence storage; IBStart never clears these.
bool IsReservedCacheFolder(std::wstring_view name) {
  return name == L"licenses" || name == L"license" || name == L"lic";
}

bool ContainsReservedCacheFolder(std::wstring_view relative) {
  size_t start = 0;
  while (start < relative.size()) {
    const size_t end = relative.find_first_of(L"\\/", start);
    const auto component = relative.substr(start, end == std::wstring_view::npos ? relative.size() - start : end - start);
    if (IsReservedCacheFolder(component)) return true;
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return false;
}

bool IsSafeCachePath(const std::filesystem::path& path) {
  const auto candidate = NormalizedLower(path);
  for (const auto& rootPath : AllowedCacheRoots()) {
    auto root = NormalizedLower(rootPath);
    if (!root.ends_with(L'\\')) root.push_back(L'\\');
    if (candidate.starts_with(root) && candidate.size() > root.size()) {
      return !ContainsReservedCacheFolder(std::wstring_view(candidate).substr(root.size()));
    }
  }
  return false;
}
}  // namespace

std::vector<CacheItem> CandidatesFor(const domain::Database& database, std::stop_token stop) {
  std::vector<CacheItem> result;
  if (stop.stop_requested()) return result;
  const auto identifier = SafeIdentifier(database.id.empty() ? database.name : database.id);
  if (!identifier) return result;
  // IBStart only targets explicit cache subdirectories; it never derives a path from Connect and therefore cannot remove a file base.
  std::vector<std::filesystem::path> paths;
  const auto roaming = Env(L"APPDATA");
  const auto local = Env(L"LOCALAPPDATA");
  if (!roaming.empty()) paths.push_back(std::filesystem::path(roaming) / L"1C" / L"1Cv8" / *identifier);
  if (!local.empty()) {
    paths.push_back(std::filesystem::path(local) / L"1C" / L"1Cv8" / *identifier);
    paths.push_back(std::filesystem::path(local) / L"IBStart" / L"cache" / *identifier);
  }
  for (const auto& path : paths) {
    if (stop.stop_requested()) return {};
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
      const auto bytes = SizeOf(path, stop);
      if (stop.stop_requested()) return {};
      result.push_back({path, bytes});
    }
  }
  return result;
}

std::wstring FormatSize(uintmax_t bytes) {
  if (bytes < 1024) return std::to_wstring(bytes) + L" Б";

  constexpr const wchar_t* units[] = {L"КБ", L"МБ", L"ГБ", L"ТБ", L"ПБ"};
  constexpr size_t unitCount = sizeof(units) / sizeof(*units);
  double value = static_cast<double>(bytes) / 1024.0;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < unitCount) {
    value /= 1024.0;
    ++unit;
  }

  const auto roundedTenths = static_cast<unsigned long long>(std::llround(value * 10.0));
  std::wostringstream text;
  text << std::fixed << std::setprecision(roundedTenths % 10 == 0 ? 0 : 1) << value;
  auto result = text.str();
  std::replace(result.begin(), result.end(), L'.', L',');
  return result + L" " + units[unit];
}

bool HasActiveOneCProcess() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{sizeof(entry)};
  bool found = false;
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (_wcsicmp(entry.szExeFile, L"1cv8.exe") == 0 || _wcsicmp(entry.szExeFile, L"1cv8c.exe") == 0) { found = true; break; }
  }
  CloseHandle(snapshot);
  return found;
}

ClearResult Clear(const std::vector<CacheItem>& candidates) {
  ClearResult result;
  for (const auto& item : candidates) {
    std::error_code error;
    if (_wcsicmp(item.path.filename().c_str(), L"1Cv8.1CD") == 0 || !IsSafeCachePath(item.path)) {
      result.errors.push_back(L"Отказ от небезопасного пути очистки: " + item.path.wstring()); continue;
    }
    const uintmax_t bytes = SizeOf(item.path);
    uintmax_t fileCount = 0;
    for (std::filesystem::recursive_directory_iterator it(item.path, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) { if (error) { error.clear(); continue; } if (it->is_regular_file(error) && !error) ++fileCount; error.clear(); }
    std::filesystem::remove_all(item.path, error);
    if (error) { const auto message = error.message(); result.errors.push_back(L"Не удалось очистить " + item.path.wstring() + L": " + std::wstring(message.begin(), message.end())); }
    else { result.files += fileCount; result.bytes += bytes; }
  }
  return result;
}

}  // namespace ibstart::cache
