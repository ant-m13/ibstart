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
  std::wstring text(size, L'\0'); GetEnvironmentVariableW(name, text.data(), size); text.resize(size - 1); return text;
}

uintmax_t SizeOf(const std::filesystem::path& root) {
  uintmax_t result = 0; std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error)) {
    if (!error && entry.is_regular_file(error)) result += entry.file_size(error);
  }
  return result;
}

std::wstring SafeId(std::wstring value) {
  for (auto& character : value) if (!std::iswalnum(character) && character != L'_' && character != L'-') character = L'_';
  return value.empty() ? L"unknown" : value;
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
    if (item.path.filename() == L"1Cv8.1CD" || item.path.wstring().find(L"\\cache\\") == std::wstring::npos) {
      result.errors.push_back(L"Отказ от небезопасного пути очистки: " + item.path.wstring()); continue;
    }
    uintmax_t fileCount = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(item.path, std::filesystem::directory_options::skip_permission_denied, error)) if (entry.is_regular_file(error)) ++fileCount;
    std::filesystem::remove_all(item.path, error);
    if (error) { const auto message = error.message(); result.errors.push_back(L"Не удалось очистить " + item.path.wstring() + L": " + std::wstring(message.begin(), message.end())); }
    else { result.files += fileCount; result.bytes += item.bytes; }
  }
  return result;
}

}  // namespace ibstart::cache
