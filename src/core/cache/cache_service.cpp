#include "core/cache/cache_service.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

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

uintmax_t SizeOf(const std::filesystem::path& root) {
  uintmax_t result = 0; std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
    if (error) { error.clear(); continue; }
    if (it->is_regular_file(error)) {
      const auto size = it->file_size(error);
      if (!error) result += size;
    }
    error.clear();
  }
  return result;
}

std::wstring SafeId(std::wstring value) {
  for (auto& character : value) if (!std::iswalnum(character) && character != L'_' && character != L'-') character = L'_';
  return value.empty() ? L"unknown" : value;
}

std::wstring NormalizedLower(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error).wstring();
  if (error) normalized = path.lexically_normal().wstring();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return normalized;
}

bool IsSafeCachePath(const std::filesystem::path& path) {
  const auto local = Env(L"LOCALAPPDATA");
  if (local.empty()) return false;
  auto candidate = NormalizedLower(path);
  const std::vector<std::filesystem::path> allowedRoots = {
      std::filesystem::path(local) / L"1C" / L"1Cv8" / L"cache",
      std::filesystem::path(local) / L"IBStart" / L"cache"};
  for (const auto& rootPath : allowedRoots) {
    auto root = NormalizedLower(rootPath);
    if (!root.ends_with(L'\\')) root.push_back(L'\\');
    if (candidate.starts_with(root) && candidate.size() > root.size()) return true;
  }
  return false;
}
}  // namespace

std::vector<CacheItem> CandidatesFor(const domain::Database& database) {
  std::vector<CacheItem> result;
  const auto local = Env(L"LOCALAPPDATA");
  if (local.empty()) return result;
  const auto identifier = SafeId(database.id.empty() ? database.name : database.id);
  // IBStart only targets explicit cache subdirectories; it never derives a path from Connect and therefore cannot remove a file base.
  const std::vector<std::filesystem::path> paths = {
      std::filesystem::path(local) / L"1C" / L"1Cv8" / L"cache" / identifier,
      std::filesystem::path(local) / L"IBStart" / L"cache" / identifier};
  for (const auto& path : paths) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) result.push_back({path, SizeOf(path)});
  }
  return result;
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
