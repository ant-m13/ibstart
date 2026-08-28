#include "core/cache/cache_service.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

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

class ScopedHandle {
 public:
  ScopedHandle() noexcept = default;
  explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~ScopedHandle() { Reset(); }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = other.handle_;
      other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  void Reset() noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

  HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct FileIdentity {
  DWORD volume_serial{};
  ULONGLONG file_index{};

  friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct HandleMetadata {
  FileIdentity identity;
  DWORD attributes{};
  uintmax_t size{};
};

struct DirectoryEntry {
  std::wstring name;
  FileIdentity identity;
  bool directory{};
  bool reparse_point{};
};

struct OpenDirectory {
  std::filesystem::path path;
  ScopedHandle handle;
  FileIdentity identity;
};

struct OpenEntry {
  std::filesystem::path path;
  ScopedHandle handle;
  HandleMetadata metadata;
};

struct RemovalStats {
  uintmax_t files{};
  uintmax_t bytes{};
};

// Do not share deletion while a candidate or one of its entries is being
// processed. This prevents a validated object from being renamed underneath
// its handle.
constexpr DWORD kFileShare = FILE_SHARE_READ | FILE_SHARE_WRITE;

std::wstring NormalizeWindowsPath(std::wstring value) {
  if (value.starts_with(L"\\\\?\\UNC\\")) {
    value = L"\\\\" + value.substr(8);
  } else if (value.starts_with(L"\\\\?\\")) {
    value.erase(0, 4);
  }
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3 && value.back() == L'\\') value.pop_back();
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return value;
}

struct LocalFreeDeleter {
  void operator()(wchar_t* value) const noexcept {
    if (value != nullptr) static_cast<void>(LocalFree(value));
  }
};

std::wstring WindowsErrorMessage(DWORD error) {
  wchar_t* raw_buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw_buffer), 0, nullptr);
  std::unique_ptr<wchar_t, LocalFreeDeleter> buffer(raw_buffer);
  if (!length || !buffer) return L"код " + std::to_wstring(error);

  std::wstring message(buffer.get(), length);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
  return message;
}

std::wstring WindowsFailure(std::wstring_view action, const std::filesystem::path& path, DWORD error) {
  return std::wstring(action) + L" " + path.wstring() + L": " + WindowsErrorMessage(error);
}

std::optional<std::wstring> FinalPath(HANDLE handle, const std::filesystem::path& path, std::wstring& failure) {
  std::wstring value(32768, L'\0');
  DWORD length = GetFinalPathNameByHandleW(handle, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0) {
    failure = WindowsFailure(L"Не удалось проверить расположение", path, GetLastError());
    return std::nullopt;
  }
  if (length >= value.size()) {
    value.resize(static_cast<size_t>(length) + 1);
    length = GetFinalPathNameByHandleW(handle, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= value.size()) {
      failure = WindowsFailure(L"Не удалось проверить расположение", path, GetLastError());
      return std::nullopt;
    }
  }
  value.resize(length);
  return value;
}

bool IsHandleInAllowedCacheRoot(HANDLE handle, const std::filesystem::path& path, std::wstring& failure) {
  const auto final_path = FinalPath(handle, path, failure);
  if (!final_path) return false;

  const auto candidate = NormalizeWindowsPath(*final_path);
  const auto expected = NormalizeWindowsPath(path.lexically_normal().wstring());
  if (candidate != expected) {
    failure = L"Отказ от очистки пути с reparse point: " + path.wstring();
    return false;
  }
  for (const auto& root_path : AllowedCacheRoots()) {
    // Use the lexical allowlist root here. Resolving it with weakly_canonical would
    // make a junction in the allowlist itself look like an approved physical root.
    auto root = NormalizeWindowsPath(root_path.lexically_normal().wstring());
    if (!root.ends_with(L'\\')) root.push_back(L'\\');
    if (candidate.starts_with(root) && candidate.size() > root.size()) return true;
  }
  failure = L"Отказ от очистки каталога вне allowlist: " + path.wstring();
  return false;
}

ScopedHandle OpenPath(const std::filesystem::path& path, bool directory, DWORD access, std::wstring& failure) {
  // Never resolve the final path component through a junction or symbolic link.
  DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
  if (directory) flags |= FILE_FLAG_BACKUP_SEMANTICS;
  const HANDLE handle = CreateFileW(path.c_str(), access, kFileShare, nullptr, OPEN_EXISTING, flags, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    failure = WindowsFailure(L"Не удалось открыть", path, GetLastError());
    return {};
  }
  return ScopedHandle(handle);
}

bool ReadHandleMetadata(HANDLE handle, const std::filesystem::path& path, HandleMetadata& metadata,
                        std::wstring& failure) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) {
    failure = WindowsFailure(L"Не удалось проверить", path, GetLastError());
    return false;
  }

  metadata.identity = {
      information.dwVolumeSerialNumber,
      (static_cast<ULONGLONG>(information.nFileIndexHigh) << 32) | information.nFileIndexLow};
  metadata.attributes = information.dwFileAttributes;
  metadata.size = (static_cast<uintmax_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
  return true;
}

bool CheckSafeMetadata(const std::filesystem::path& path, const HandleMetadata& metadata, bool directory,
                       std::wstring& failure) {
  if ((metadata.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    failure = L"Отказ от очистки reparse point: " + path.wstring();
    return false;
  }
  if (((metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
    failure = L"Тип элемента очистки изменился: " + path.wstring();
    return false;
  }
  return true;
}

std::optional<OpenDirectory> OpenRootDirectory(const std::filesystem::path& path, std::wstring& failure) {
  const DWORD access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE;
  auto handle = OpenPath(path, true, access, failure);
  if (!handle.valid()) return std::nullopt;

  HandleMetadata metadata;
  if (!ReadHandleMetadata(handle.get(), path, metadata, failure) ||
      !CheckSafeMetadata(path, metadata, true, failure)) {
    return std::nullopt;
  }
  if (!IsHandleInAllowedCacheRoot(handle.get(), path, failure)) return std::nullopt;
  return OpenDirectory{path, std::move(handle), metadata.identity};
}

bool HandleStillNames(HANDLE handle, const std::filesystem::path& path, const FileIdentity& expected,
                      bool directory, std::wstring& failure) {
  HandleMetadata metadata;
  if (!ReadHandleMetadata(handle, path, metadata, failure) ||
      !CheckSafeMetadata(path, metadata, directory, failure)) {
    return false;
  }
  if (metadata.identity != expected) {
    failure = L"Путь изменился во время очистки: " + path.wstring();
    return false;
  }
  const auto final_path = FinalPath(handle, path, failure);
  if (!final_path) return false;
  if (NormalizeWindowsPath(*final_path) != NormalizeWindowsPath(path.lexically_normal().wstring())) {
    failure = L"Путь изменился во время очистки: " + path.wstring();
    return false;
  }
  return true;
}

bool PathStillNames(const OpenDirectory& directory, std::wstring& failure) {
  return HandleStillNames(directory.handle.get(), directory.path, directory.identity, true, failure);
}

bool PathStillNames(const OpenEntry& entry, std::wstring& failure) {
  return HandleStillNames(entry.handle.get(), entry.path, entry.metadata.identity,
                          (entry.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, failure);
}

bool ReadDirectoryEntries(const OpenDirectory& directory, std::vector<DirectoryEntry>& entries,
                          std::wstring& failure) {
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
  bool restart = true;
  for (;;) {
    const auto info_class = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
    restart = false;
    if (!GetFileInformationByHandleEx(directory.handle.get(), info_class,
                                       buffer.data(), static_cast<DWORD>(buffer.size()))) {
      const DWORD error = GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_HANDLE_EOF) return true;
      failure = WindowsFailure(L"Не удалось перечислить", directory.path, error);
      return false;
    }

    size_t offset = 0;
    for (;;) {
      if (offset > buffer.size() || offsetof(FILE_ID_BOTH_DIR_INFO, FileName) > buffer.size() - offset) {
        failure = L"Повреждённая запись каталога очистки: " + directory.path.wstring();
        return false;
      }
      const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
      const size_t name_offset = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (entry->FileNameLength % sizeof(wchar_t) != 0 ||
          entry->FileNameLength > buffer.size() - offset - name_offset) {
        failure = L"Повреждённая запись каталога очистки: " + directory.path.wstring();
        return false;
      }

      const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (!name.empty() && name != L"." && name != L"..") {
        entries.push_back({
            name,
            {directory.identity.volume_serial, static_cast<ULONGLONG>(entry->FileId.QuadPart)},
            (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
      }

      if (entry->NextEntryOffset == 0) break;
      if (entry->NextEntryOffset < name_offset || entry->NextEntryOffset > buffer.size() - offset) {
        failure = L"Повреждённая запись каталога очистки: " + directory.path.wstring();
        return false;
      }
      offset += entry->NextEntryOffset;
    }
  }
}

bool OpenChild(const OpenDirectory& parent, const DirectoryEntry& entry, OpenEntry& result,
               std::wstring& failure) {
  const auto child_path = parent.path / entry.name;
  if (entry.reparse_point) {
    failure = L"Отказ от очистки reparse point: " + child_path.wstring();
    return false;
  }
  if (!PathStillNames(parent, failure)) return false;

  const DWORD access = FILE_READ_ATTRIBUTES | DELETE |
      (entry.directory ? FILE_LIST_DIRECTORY : 0);
  auto handle = OpenPath(child_path, entry.directory, access, failure);
  if (!handle.valid()) return false;

  HandleMetadata metadata;
  if (!ReadHandleMetadata(handle.get(), child_path, metadata, failure) ||
      !CheckSafeMetadata(child_path, metadata, entry.directory, failure)) {
    return false;
  }
  if (entry.identity.file_index != 0 && metadata.identity != entry.identity) {
    failure = L"Элемент изменился во время очистки: " + child_path.wstring();
    return false;
  }
  if (!HandleStillNames(handle.get(), child_path, metadata.identity, entry.directory, failure)) return false;
  if (!PathStillNames(parent, failure)) return false;

  result = OpenEntry{child_path, std::move(handle), metadata};
  return true;
}

bool ValidateTree(const OpenDirectory& directory, std::wstring& failure) {
  if (!PathStillNames(directory, failure)) return false;

  std::vector<DirectoryEntry> entries;
  if (!ReadDirectoryEntries(directory, entries, failure)) return false;
  for (const auto& entry : entries) {
    OpenEntry child;
    if (!OpenChild(directory, entry, child, failure)) return false;
    if (child.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) {
      const OpenDirectory child_directory{child.path, std::move(child.handle), child.metadata.identity};
      if (!ValidateTree(child_directory, failure)) return false;
    }
  }
  return PathStillNames(directory, failure);
}

bool DeleteOpenedHandle(HANDLE handle, const std::filesystem::path& path, bool directory, std::wstring& failure) {
  // The delete request is bound to the already validated handle, not re-resolved from path.
  HandleMetadata metadata;
  if (!ReadHandleMetadata(handle, path, metadata, failure) ||
      !CheckSafeMetadata(path, metadata, directory, failure)) {
    return false;
  }
  // POSIX semantics removes the directory entry as soon as the disposition is
  // set, even though this validation handle remains open until the current
  // traversal scope ends.  That is required before deleting an otherwise empty
  // parent directory.
  FILE_DISPOSITION_INFO_EX disposition_ex{};
  disposition_ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
  if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &disposition_ex, sizeof(disposition_ex))) return true;

  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) return true;

  failure = WindowsFailure(L"Не удалось удалить", path, GetLastError());
  return false;
}

bool DeleteTree(OpenDirectory& directory, const OpenDirectory& root, RemovalStats& stats, std::wstring& failure) {
  if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure)) return false;
  if (!PathStillNames(directory, failure)) return false;

  std::vector<DirectoryEntry> entries;
  if (!ReadDirectoryEntries(directory, entries, failure)) return false;
  for (const auto& entry : entries) {
    if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure)) return false;
    OpenEntry child;
    if (!OpenChild(directory, entry, child, failure)) return false;

    if (child.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) {
      OpenDirectory child_directory{child.path, std::move(child.handle), child.metadata.identity};
      if (!DeleteTree(child_directory, root, stats, failure)) return false;
      if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure) ||
          !PathStillNames(directory, failure) || !PathStillNames(child_directory, failure)) return false;
      HandleMetadata metadata;
      if (!ReadHandleMetadata(child_directory.handle.get(), child_directory.path, metadata, failure) ||
          !CheckSafeMetadata(child_directory.path, metadata, true, failure)) {
        return false;
      }
      if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure) ||
          !DeleteOpenedHandle(child_directory.handle.get(), child_directory.path, true, failure)) return false;
      // Close the disposition handle before the parent is considered for
      // deletion; non-POSIX filesystems may only remove the entry on close.
      child_directory.handle = ScopedHandle();
    } else {
      if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure) ||
          !PathStillNames(directory, failure) ||
          !PathStillNames(child, failure)) {
        return false;
      }
      HandleMetadata metadata;
      if (!ReadHandleMetadata(child.handle.get(), child.path, metadata, failure) ||
          !CheckSafeMetadata(child.path, metadata, false, failure)) {
        return false;
      }
      if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure) ||
          !DeleteOpenedHandle(child.handle.get(), child.path, false, failure)) return false;
      child.handle = ScopedHandle();
      ++stats.files;
      stats.bytes += metadata.size;
    }
  }
  return PathStillNames(directory, failure);
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
  // This check is advisory only. A running 1C client may hold cache files, but it
  // must not prevent the rest of the allowlisted candidates from being attempted.
  result.active_one_c_process = HasActiveOneCProcess();
  for (const auto& item : candidates) {
    if (_wcsicmp(item.path.filename().c_str(), L"1Cv8.1CD") == 0 || !IsSafeCachePath(item.path)) {
      result.errors.push_back(L"Отказ от небезопасного пути очистки: " + item.path.wstring()); continue;
    }

    std::wstring failure;
    auto root = OpenRootDirectory(item.path, failure);
    if (!root || !ValidateTree(*root, failure)) {
      result.errors.push_back(failure.empty() ? L"Отказ от небезопасной очистки: " + item.path.wstring() : failure);
      continue;
    }
    if (!IsSafeCachePath(item.path)) {
      result.errors.push_back(L"Путь изменился во время очистки: " + item.path.wstring());
      continue;
    }

    // Keep the same root handle from validation through deletion. OpenPath also
    // denies delete sharing, so the validated objects cannot be renamed first.
    RemovalStats stats;
    if (!DeleteTree(*root, *root, stats, failure) || !IsSafeCachePath(item.path) ||
        !IsHandleInAllowedCacheRoot(root->handle.get(), item.path, failure) ||
        !PathStillNames(*root, failure) ||
        !DeleteOpenedHandle(root->handle.get(), item.path, true, failure)) {
      result.errors.push_back(failure.empty() ? L"Не удалось безопасно очистить " + item.path.wstring() : failure);
      continue;
    }
    result.files += stats.files;
    result.bytes += stats.bytes;
  }
  result.active_one_c_process = result.active_one_c_process || HasActiveOneCProcess();
  return result;
}

}  // namespace ibstart::cache
