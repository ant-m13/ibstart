#include "core/v8i/v8i_file_store.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ibstart::v8i {
namespace {

std::wstring Timestamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_s(&local, &now);
  std::wostringstream stream;
  stream << std::put_time(&local, L"%Y%m%d_%H%M%S");
  return stream.str();
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& target) {
  const auto base = target.filename().wstring() + L".ibstart.tmp." + std::to_wstring(GetCurrentProcessId());
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    const auto candidate = target.parent_path() / (base + L"." + std::to_wstring(attempt));
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw std::runtime_error("Unable to allocate a temporary file alongside ibases.v8i.");
}

}  // namespace

V8iFileStore::V8iFileStore(std::filesystem::path path) : path_(std::move(path)) {}

std::optional<V8iFileStore::Fingerprint> V8iFileStore::FingerprintOf(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return std::nullopt;
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw std::runtime_error("Cannot inspect ibases.v8i: " + error.message());
  const auto time = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error("Cannot inspect ibases.v8i: " + error.message());
  return Fingerprint{size, time};
}

V8iDocument V8iFileStore::Read() {
  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open ibases.v8i for reading: " + utf::ToUtf8(path_.wstring()));
  const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) throw std::runtime_error("Cannot read ibases.v8i completely.");
  loaded_fingerprint_ = FingerprintOf(path_);
  return V8iDocument::ParseUtf8(bytes);
}

void V8iFileStore::CreateBackup() const {
  if (!std::filesystem::exists(path_)) return;
  const auto prefix = path_.filename().wstring() + L".bak_" + Timestamp();
  auto backup = path_.parent_path() / prefix;
  for (unsigned suffix = 1; std::filesystem::exists(backup); ++suffix) backup = path_.parent_path() / (prefix + L"_" + std::to_wstring(suffix));
  std::error_code error;
  std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::none, error);
  if (error) throw std::runtime_error("Cannot create ibases.v8i backup: " + error.message());
}

std::vector<std::filesystem::path> V8iFileStore::Backups() const {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  if (!std::filesystem::exists(path_.parent_path(), error)) return result;
  const auto prefix = path_.filename().wstring() + L".bak_";
  for (const auto& item : std::filesystem::directory_iterator(path_.parent_path(), error)) {
    if (error) break;
    if (item.is_regular_file() && item.path().filename().wstring().starts_with(prefix)) result.push_back(item.path());
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
  });
  return result;
}

void V8iFileStore::PruneBackups() const {
  auto backups = Backups();
  for (size_t index = 5; index < backups.size(); ++index) {
    std::error_code error;
    std::filesystem::remove(backups[index], error);
  }
}

void V8iFileStore::Save(const V8iDocument& document) {
  if (loaded_fingerprint_ && FingerprintOf(path_) != loaded_fingerprint_) {
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
    CreateBackup();
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::runtime_error("Cannot replace ibases.v8i atomically: " + utf::ToUtf8(utf::LastErrorMessage()));
    }
  } catch (...) {
    std::filesystem::remove(temporary, error);
    throw;
  }
  loaded_fingerprint_ = FingerprintOf(path_);
  PruneBackups();
}

}  // namespace ibstart::v8i
