#include "core/v8i/v8i_file_store.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ibstart::v8i {
namespace {

std::string PathText(const std::filesystem::path& path) {
  return utf::ToUtf8(path.wstring());
}

std::string FilesystemFailure(std::string_view action, const std::filesystem::path& path, const std::error_code& error) {
  return std::string(action) + ": " + PathText(path) + ": " + error.message();
}

std::wstring Timestamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_s(&local, &now);
  std::wostringstream stream;
  stream << std::put_time(&local, L"%Y%m%d_%H%M%S");
  return stream.str();
}

bool IsManagedBackupName(std::wstring_view filename, std::wstring_view prefix) {
  if (!filename.starts_with(prefix)) return false;
  const auto suffix = filename.substr(prefix.size());
  constexpr size_t kTimestampLength = 15;  // YYYYMMDD_HHMMSS
  if (suffix.size() < kTimestampLength) return false;
  for (size_t index = 0; index < kTimestampLength; ++index) {
    if (index == 8) {
      if (suffix[index] != L'_') return false;
    } else if (suffix[index] < L'0' || suffix[index] > L'9') {
      return false;
    }
  }
  if (suffix.size() == kTimestampLength) return true;
  if (suffix[kTimestampLength] != L'_' || suffix.size() == kTimestampLength + 1) return false;
  return std::all_of(suffix.begin() + kTimestampLength + 1, suffix.end(), [](wchar_t character) {
    return character >= L'0' && character <= L'9';
  });
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& target) {
  const auto base = target.filename().wstring() + L".ibstart.tmp." + std::to_wstring(GetCurrentProcessId());
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    const auto candidate = target.parent_path() / (base + L"." + std::to_wstring(attempt));
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) {
      if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect temporary ibases.v8i path", candidate, error));
      return candidate;
    }
  }
  throw std::runtime_error("Unable to allocate a temporary file alongside ibases.v8i: " + PathText(target));
}

std::wstring SaveMutexName(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(path, error);
    if (error) normalized = path.lexically_normal();
  }
  auto key = normalized.wstring();
  std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  std::uint64_t hash = 1469598103934665603ULL;
  for (const wchar_t character : key) {
    hash ^= static_cast<std::uint16_t>(character);
    hash *= 1099511628211ULL;
  }
  std::wostringstream name;
  name << L"Local\\IBStart.V8iSave." << std::hex << std::setw(16) << std::setfill(L'0') << hash;
  return name.str();
}

class SaveMutex final {
 public:
  explicit SaveMutex(const std::filesystem::path& path) {
    const auto name = SaveMutexName(path);
    handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!handle_) throw std::runtime_error("Cannot create ibases.v8i save mutex: " + utf::ToUtf8(utf::LastErrorMessage()));
    const DWORD wait = WaitForSingleObject(handle_, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      acquired_ = true;
      return;
    }
    const DWORD lastError = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    CloseHandle(handle_);
    handle_ = nullptr;
    throw std::runtime_error("Cannot acquire ibases.v8i save mutex: " + utf::ToUtf8(utf::LastErrorMessage(lastError)));
  }

  ~SaveMutex() {
    if (acquired_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
  }

  SaveMutex(const SaveMutex&) = delete;
  SaveMutex& operator=(const SaveMutex&) = delete;

 private:
  HANDLE handle_{};
  bool acquired_{false};
};

class SaveLock final {
 public:
  explicit SaveLock(const std::filesystem::path& path) {
    // Do not share write or delete access: a concurrent atomic replacement
    // needs delete access to the target and is rejected during the save.
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      handle_ = nullptr;
      throw std::runtime_error("Cannot lock ibases.v8i for saving: " + PathText(path) + ": " +
          utf::ToUtf8(utf::LastErrorMessage(error)));
    }
  }

  ~SaveLock() { if (handle_) CloseHandle(handle_); }

  SaveLock(const SaveLock&) = delete;
  SaveLock& operator=(const SaveLock&) = delete;

 private:
  HANDLE handle_{};
};

}  // namespace

V8iFileStore::V8iFileStore(std::filesystem::path path) : path_(std::move(path)) {}

std::optional<V8iFileStore::Fingerprint> V8iFileStore::FingerprintOf(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i", path, error));
  if (!exists) return std::nullopt;
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i", path, error));
  const auto time = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i", path, error));
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot inspect ibases.v8i contents: " + PathText(path));
  std::uint64_t hash = 1469598103934665603ULL;
  char buffer[8192];
  while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
    for (std::streamsize index = 0; index < input.gcount(); ++index) { hash ^= static_cast<unsigned char>(buffer[index]); hash *= 1099511628211ULL; }
  }
  if (!input.eof()) throw std::runtime_error("Cannot inspect ibases.v8i contents completely: " + PathText(path));
  return Fingerprint{size, time, hash};
}

V8iDocument V8iFileStore::Read() {
  const auto before = FingerprintOf(path_);
  if (!before) throw std::runtime_error("Cannot open ibases.v8i for reading: " + utf::ToUtf8(path_.wstring()));
  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open ibases.v8i for reading: " + utf::ToUtf8(path_.wstring()));
  const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) throw std::runtime_error("Cannot read ibases.v8i completely.");
  const auto after = FingerprintOf(path_);
  if (before != after) throw ExternalModificationError("ibases.v8i changed while it was being read. Reload it.");
  loaded_fingerprint_ = after;
  fingerprint_known_ = true;
  return V8iDocument::ParseUtf8(bytes);
}

void V8iFileStore::AcceptCurrentContentsForOverwrite() {
  const auto before = FingerprintOf(path_);
  const auto after = FingerprintOf(path_);
  if (before != after) throw ExternalModificationError("ibases.v8i changed while overwrite was being prepared. Try again.");
  loaded_fingerprint_ = after;
  fingerprint_known_ = true;
}

void V8iFileStore::CreateBackup() const {
  std::error_code error;
  if (!std::filesystem::exists(path_, error)) {
    if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i before backup", path_, error));
    return;
  }
  const auto prefix = path_.filename().wstring() + L".bak_" + Timestamp();
  auto backup = path_.parent_path() / prefix;
  for (unsigned suffix = 1;; ++suffix) {
    const bool exists = std::filesystem::exists(backup, error);
    if (error) throw std::runtime_error(FilesystemFailure("Cannot allocate ibases.v8i backup path", backup, error));
    if (!exists) break;
    backup = path_.parent_path() / (prefix + L"_" + std::to_wstring(suffix));
  }
  std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::none, error);
  if (error) throw std::runtime_error(FilesystemFailure("Cannot create ibases.v8i backup", backup, error));
}

std::vector<std::filesystem::path> V8iFileStore::Backups() const {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> dated;
  std::error_code error;
  if (!std::filesystem::exists(path_.parent_path(), error)) {
    if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i backup directory", path_.parent_path(), error));
    return {};
  }
  const auto prefix = path_.filename().wstring() + L".bak_";
  std::filesystem::directory_iterator it(path_.parent_path(), std::filesystem::directory_options::skip_permission_denied, error);
  if (error) throw std::runtime_error(FilesystemFailure("Cannot enumerate ibases.v8i backups", path_.parent_path(), error));
  const std::filesystem::directory_iterator end;
  while (it != end) {
    if (IsManagedBackupName(it->path().filename().wstring(), prefix)) {
      if (!it->is_regular_file(error)) {
        if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i backup", it->path(), error));
      } else {
        const auto time = it->last_write_time(error);
        if (error) throw std::runtime_error(FilesystemFailure("Cannot inspect ibases.v8i backup", it->path(), error));
        dated.emplace_back(time, it->path());
      }
    }
    it.increment(error);
    if (error) throw std::runtime_error(FilesystemFailure("Cannot enumerate ibases.v8i backups", path_.parent_path(), error));
  }
  std::sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::filesystem::path> result;
  result.reserve(dated.size());
  for (auto& item : dated) result.push_back(std::move(item.second));
  return result;
}

void V8iFileStore::AddMaintenanceWarning(std::string message) {
  maintenance_warnings_.push_back(std::move(message));
}

void V8iFileStore::PruneBackups() {
  std::vector<std::filesystem::path> backups;
  try {
    backups = Backups();
  } catch (const std::exception& error) {
    AddMaintenanceWarning("Cannot prune ibases.v8i backups: " + std::string(error.what()));
    return;
  }
  for (size_t index = 5; index < backups.size(); ++index) {
    std::error_code error;
    std::filesystem::remove(backups[index], error);
    if (error) AddMaintenanceWarning(FilesystemFailure("Cannot remove obsolete ibases.v8i backup", backups[index], error));
  }
}

void V8iFileStore::Save(const V8iDocument& document) {
  maintenance_warnings_.clear();
  SaveMutex saveMutex(path_);
  if (!fingerprint_known_) {
    throw ExternalModificationError("ibases.v8i could not be verified after the previous save. Reload it before saving again.");
  }
  if (FingerprintOf(path_) != loaded_fingerprint_) {
    throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
  }
  std::error_code error;
  if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path(), error);
  if (error) throw std::runtime_error("Cannot create ibases.v8i directory: " + error.message());

  const auto temporary = UniqueTemporaryPath(path_);
  try {
    const auto contents = document.SerializeUtf8();
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("Cannot create temporary ibases.v8i file.");
      output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      output.flush();
      if (!output) throw std::runtime_error("Cannot write temporary ibases.v8i file.");
    }
    if (loaded_fingerprint_) {
      {
        SaveLock lock(path_);
        if (FingerprintOf(path_) != loaded_fingerprint_) {
          throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
        }
        CreateBackup();
        if (FingerprintOf(path_) != loaded_fingerprint_) {
          throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
        }
      }  // MoveFileExW requires the target handle to be closed before replacement.
      if (FingerprintOf(path_) != loaded_fingerprint_) {
        throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
      }
    } else {
      if (FingerprintOf(path_) != loaded_fingerprint_) {
        throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
      }
      CreateBackup();
      if (FingerprintOf(path_) != loaded_fingerprint_) {
        throw ExternalModificationError("ibases.v8i was changed by another program. Reload it before saving.");
      }
    }
    const DWORD replace_flags = MOVEFILE_WRITE_THROUGH | (loaded_fingerprint_ ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), replace_flags)) {
      throw std::runtime_error("Cannot replace ibases.v8i atomically: " + utf::ToUtf8(utf::LastErrorMessage()));
    }
  } catch (...) {
    std::filesystem::remove(temporary, error);
    if (error) AddMaintenanceWarning(FilesystemFailure("Cannot remove temporary ibases.v8i file", temporary, error));
    throw;
  }
  // MoveFileExW is the commit point. A verifier error after this line must not
  // be reported as a failed save: the new document is already visible on disk.
  // Mark the fingerprint as unknown so a later save requires an explicit read
  // instead of risking an overwrite of state that we could not verify.
  try {
    const auto committed = FingerprintOf(path_);
    if (committed) {
      loaded_fingerprint_ = committed;
      fingerprint_known_ = true;
    } else {
      loaded_fingerprint_.reset();
      fingerprint_known_ = false;
      AddMaintenanceWarning("ibases.v8i was saved but disappeared before verification; reload is required before the next save.");
    }
  } catch (const std::exception& error) {
    loaded_fingerprint_.reset();
    fingerprint_known_ = false;
    AddMaintenanceWarning("ibases.v8i was saved but its new fingerprint could not be verified; reload is required before the next save: " +
        std::string(error.what()));
  }
  PruneBackups();
}

}  // namespace ibstart::v8i
