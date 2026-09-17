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

struct DirectorySizeResult {
  uintmax_t bytes{};
  bool complete{true};
  bool cancelled{};
  std::vector<std::wstring> errors;
};

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

struct AllowedCacheRoot {
  std::filesystem::path root;
  // APPDATA and LOCALAPPDATA are Windows-owned configuration anchors. They
  // may themselves be mounted through an alias or junction, but every path
  // component below the anchor must remain a real directory.
  std::filesystem::path trusted_base;
};

std::vector<AllowedCacheRoot> AllowedCacheRoots() {
  std::vector<AllowedCacheRoot> roots;
  const auto roaming = Env(L"APPDATA");
  const auto local = Env(L"LOCALAPPDATA");
  if (!roaming.empty()) {
    const std::filesystem::path base(roaming);
    roots.push_back({base / L"1C" / L"1Cv8", base});
  }
  if (!local.empty()) {
    const std::filesystem::path base(local);
    roots.push_back({base / L"1C" / L"1Cv8", base});
    roots.push_back({base / L"IBStart" / L"cache", base});
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
  for (const auto& allowed : AllowedCacheRoots()) {
    auto root = NormalizedLower(allowed.root);
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

void AddScanError(DirectorySizeResult& result, std::wstring message) {
  result.complete = false;
  // A damaged or inaccessible tree can contain a very large number of
  // entries.  Keep the persisted/UI result bounded while retaining enough
  // diagnostics for the log and the status message.
  constexpr std::size_t kMaximumReportedErrors = 32;
  if (result.errors.size() < kMaximumReportedErrors) result.errors.push_back(std::move(message));
}

DirectorySizeResult SizeOf(const std::filesystem::path& root, std::stop_token stop) {
  DirectorySizeResult result;
  if (stop.stop_requested()) {
    result.cancelled = true;
    result.complete = false;
    return result;
  }

  std::error_code error;
  const DWORD root_attributes = GetFileAttributesW(root.c_str());
  if (root_attributes == INVALID_FILE_ATTRIBUTES) {
    AddScanError(result, WindowsFailure(L"Не удалось прочитать", root, GetLastError()));
    return result;
  }
  if ((root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    AddScanError(result, L"Пропущен reparse point в каталоге кэша: " + root.wstring());
    return result;
  }
  if ((root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    AddScanError(result, L"Ожидался каталог кэша: " + root.wstring());
    return result;
  }

  std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::none, error);
  if (error) {
    AddScanError(result, WindowsFailure(L"Не удалось перечислить", root, GetLastError()));
    return result;
  }
  const std::filesystem::recursive_directory_iterator end;
  for (; iterator != end;) {
    if (stop.stop_requested()) {
      result.cancelled = true;
      result.complete = false;
      return result;
    }

    const auto path = iterator->path();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      AddScanError(result, WindowsFailure(L"Не удалось прочитать", path, GetLastError()));
    } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      AddScanError(result, L"Пропущен reparse point в кэше: " + path.wstring());
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) iterator.disable_recursion_pending();
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      error.clear();
      const auto bytes = std::filesystem::file_size(path, error);
      if (error) AddScanError(result, L"Не удалось определить размер файла: " + path.wstring());
      else result.bytes += bytes;
    }

    error.clear();
    iterator.increment(error);
    if (error) {
      AddScanError(result, L"Не удалось продолжить перечисление кэша: " + root.wstring());
      error.clear();
    }
  }
  return result;
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

std::optional<std::filesystem::path> RelativePathUnder(const std::filesystem::path& path,
                                                        const std::filesystem::path& root_path) {
  const auto candidate = NormalizeWindowsPath(path.lexically_normal().wstring());
  auto root = NormalizeWindowsPath(root_path.lexically_normal().wstring());
  if (!root.ends_with(L'\\')) root.push_back(L'\\');
  if (!candidate.starts_with(root) || candidate.size() <= root.size()) return std::nullopt;
  return std::filesystem::path(candidate.substr(root.size()));
}

bool HasReparsePointBelowTrustedBase(const std::filesystem::path& path, const AllowedCacheRoot& allowed,
                                     std::wstring& failure) {
  const auto relative_to_root = RelativePathUnder(path, allowed.root);
  const auto relative_to_base = RelativePathUnder(path, allowed.trusted_base);
  if (!relative_to_root || !relative_to_base) return false;

  std::filesystem::path current = allowed.trusted_base;
  for (const auto& component : *relative_to_base) {
    const auto name = component.wstring();
    if (name.empty() || name == L".") continue;
    if (name == L"..") return false;
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      failure = WindowsFailure(L"Не удалось проверить путь очистки", current, GetLastError());
      return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      failure = L"Отказ от очистки пути с reparse point: " + current.wstring();
      return false;
    }
  }
  return true;
}

bool IsHandleInAllowedCacheRoot(HANDLE handle, const std::filesystem::path& path, std::wstring& failure) {
  const auto final_path = FinalPath(handle, path, failure);
  if (!final_path) return false;

  const auto candidate = NormalizeWindowsPath(*final_path);
  const auto expected = NormalizeWindowsPath(NormalizedLower(path));
  if (candidate != expected) {
    failure = L"Отказ от очистки пути с reparse point: " + path.wstring();
    return false;
  }
  for (const auto& allowed : AllowedCacheRoots()) {
    auto root = NormalizeWindowsPath(NormalizedLower(allowed.root));
    if (!root.ends_with(L'\\')) root.push_back(L'\\');
    if (!candidate.starts_with(root) || candidate.size() <= root.size()) continue;
    if (!RelativePathUnder(path, allowed.root)) continue;
    if (!HasReparsePointBelowTrustedBase(path, allowed, failure)) {
      if (failure.empty()) failure = L"Отказ от очистки каталога вне allowlist: " + path.wstring();
      return false;
    }
    return true;
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
  if (NormalizeWindowsPath(*final_path) != NormalizeWindowsPath(NormalizedLower(path))) {
    failure = L"Путь изменился во время очистки: " + path.wstring();
    return false;
  }
  return IsHandleInAllowedCacheRoot(handle, path, failure);
}

bool PathStillNames(const OpenDirectory& directory, std::wstring& failure) {
  return HandleStillNames(directory.handle.get(), directory.path, directory.identity, true, failure);
}

bool PathStillNames(const OpenEntry& entry, std::wstring& failure) {
  return HandleStillNames(entry.handle.get(), entry.path, entry.metadata.identity,
                          (entry.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, failure);
}

bool ReadDirectoryEntries(const OpenDirectory& directory, std::vector<DirectoryEntry>& entries,
                          std::wstring& failure, std::stop_token stop = {}) {
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
  bool restart = true;
  for (;;) {
    if (stop.stop_requested()) return false;
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

bool ValidateTree(const OpenDirectory& directory, std::wstring& failure, std::stop_token stop = {}) {
  if (stop.stop_requested()) return false;
  if (!PathStillNames(directory, failure)) return false;

  std::vector<DirectoryEntry> entries;
  if (!ReadDirectoryEntries(directory, entries, failure, stop)) return false;
  for (const auto& entry : entries) {
    if (stop.stop_requested()) return false;
    OpenEntry child;
    if (!OpenChild(directory, entry, child, failure)) return false;
    if (child.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) {
      const OpenDirectory child_directory{child.path, std::move(child.handle), child.metadata.identity};
      if (!ValidateTree(child_directory, failure, stop)) return false;
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

bool DeleteTree(OpenDirectory& directory, const OpenDirectory& root, RemovalStats& stats,
                std::wstring& failure, std::stop_token stop = {}) {
  if (stop.stop_requested()) return false;
  if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure)) return false;
  if (!PathStillNames(directory, failure)) return false;

  std::vector<DirectoryEntry> entries;
  if (!ReadDirectoryEntries(directory, entries, failure, stop)) return false;
  for (const auto& entry : entries) {
    if (stop.stop_requested()) return false;
    if (!IsHandleInAllowedCacheRoot(root.handle.get(), root.path, failure)) return false;
    OpenEntry child;
    if (!OpenChild(directory, entry, child, failure)) return false;

    if (child.metadata.attributes & FILE_ATTRIBUTE_DIRECTORY) {
      OpenDirectory child_directory{child.path, std::move(child.handle), child.metadata.identity};
      if (!DeleteTree(child_directory, root, stats, failure, stop)) return false;
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

ScanResult Scan(const domain::Database& database, std::stop_token stop) {
  ScanResult result;
  if (stop.stop_requested()) {
    result.cancelled = true;
    result.complete = false;
    return result;
  }

  const auto identifier = SafeIdentifier(database.id.empty() ? database.name : database.id);
  if (!identifier) {
    result.complete = false;
    result.errors.push_back(L"Недопустимый идентификатор базы для поиска кэша.");
    return result;
  }

  // IBStart only targets explicit cache subdirectories; it never derives a
  // path from Connect and therefore cannot scan or remove a file base.
  std::vector<std::filesystem::path> paths;
  const auto roaming = Env(L"APPDATA");
  const auto local = Env(L"LOCALAPPDATA");
  if (!roaming.empty()) paths.push_back(std::filesystem::path(roaming) / L"1C" / L"1Cv8" / *identifier);
  if (!local.empty()) {
    paths.push_back(std::filesystem::path(local) / L"1C" / L"1Cv8" / *identifier);
    paths.push_back(std::filesystem::path(local) / L"IBStart" / L"cache" / *identifier);
  }

  std::vector<std::wstring> seen_paths;
  for (const auto& path : paths) {
    if (stop.stop_requested()) {
      result.cancelled = true;
      result.complete = false;
      result.items.clear();
      result.bytes = 0;
      return result;
    }
    const auto normalized = NormalizedLower(path);
    if (std::find(seen_paths.begin(), seen_paths.end(), normalized) != seen_paths.end()) continue;
    seen_paths.push_back(normalized);

    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
      // A cache directory disappearing between enumeration and inspection is
      // equivalent to an empty cache.  Other errors must remain visible as a
      // partial result.
      if (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND ||
          error.value() == ERROR_INVALID_NAME || error.value() == ERROR_INVALID_DRIVE) {
        continue;
      }
      result.complete = false;
      result.errors.push_back(L"Не удалось проверить каталог кэша: " + path.wstring());
      continue;
    }
    if (!std::filesystem::exists(status)) continue;
    if (!std::filesystem::is_directory(status)) {
      result.complete = false;
      result.errors.push_back(L"Путь кэша не является каталогом: " + path.wstring());
      continue;
    }
    if (!IsSafeCachePath(path)) {
      result.complete = false;
      result.errors.push_back(L"Отказ от сканирования небезопасного пути кэша: " + path.wstring());
      continue;
    }

    const auto directory = SizeOf(path, stop);
    if (directory.cancelled) {
      result.cancelled = true;
      result.complete = false;
      result.items.clear();
      result.bytes = 0;
      return result;
    }
    result.items.push_back({path, directory.bytes});
    result.bytes += directory.bytes;
    if (!directory.complete) result.complete = false;
    for (const auto& error_message : directory.errors) {
      if (result.errors.size() < 32) result.errors.push_back(error_message);
    }
  }
  return result;
}

std::vector<CacheItem> CandidatesFor(const domain::Database& database, std::stop_token stop) {
  const auto scan = Scan(database, stop);
  return scan.cancelled ? std::vector<CacheItem>{} : scan.items;
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

ClearResult Clear(const std::vector<CacheItem>& candidates, std::stop_token stop) {
  ClearResult result;
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  // This check is advisory only. A running 1C client may hold cache files, but it
  // must not prevent the rest of the allowlisted candidates from being attempted.
  result.active_one_c_process = HasActiveOneCProcess();
  for (const auto& item : candidates) {
    if (stop.stop_requested()) {
      result.cancelled = true;
      break;
    }
    if (_wcsicmp(item.path.filename().c_str(), L"1Cv8.1CD") == 0 || !IsSafeCachePath(item.path)) {
      result.errors.push_back(L"Отказ от небезопасного пути очистки: " + item.path.wstring()); continue;
    }

    std::wstring failure;
    auto root = OpenRootDirectory(item.path, failure);
    if (!root || !ValidateTree(*root, failure, stop)) {
      if (stop.stop_requested()) {
        result.cancelled = true;
        break;
      }
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
    const bool deleted_tree = DeleteTree(*root, *root, stats, failure, stop);
    if (stop.stop_requested()) {
      result.cancelled = true;
      break;
    }
    if (!deleted_tree || !IsSafeCachePath(item.path) ||
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
  if (stop.stop_requested()) result.cancelled = true;
  return result;
}

}  // namespace ibstart::cache
