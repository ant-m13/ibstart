#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"
#include "core/storage/json_codec.hpp"
#include "core/windows_path.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ibstart::storage {
namespace {

std::filesystem::path PathFor(const StorageLayout& layout, std::wstring_view name) {
  const auto path = layout.root / std::wstring(name);
  if (!windows_path::IsWithinLimit(path)) {
    throw std::runtime_error("Application data path is too long: " + utf::ToUtf8(windows_path::LengthError(path)));
  }
  return path;
}

std::wstring NormalizedStoragePath(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(path, error);
    if (error) normalized = path.lexically_normal();
  }
  auto result = normalized.wstring();
  std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return result;
}

std::string FileSizeLimitFailure(std::string_view action, const std::filesystem::path& path,
    std::uintmax_t size, std::uintmax_t limit) {
  return std::string(action) + ": " + utf::ToUtf8(path.wstring()) + " is " + std::to_string(size) +
      " bytes, exceeding the " + std::to_string(limit / (1024ULL * 1024ULL)) + " MiB safety limit.";
}

std::optional<StorageFingerprint> FingerprintOf(const std::filesystem::path& path) {
  if (!windows_path::IsWithinLimit(path)) {
    throw std::runtime_error("Application data path is too long: " + utf::ToUtf8(windows_path::LengthError(path)));
  }
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());
  if (!exists) return std::nullopt;
  if (!std::filesystem::is_regular_file(path, error)) {
    if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());
    throw std::runtime_error("Application data path is not a regular file: " + utf::ToUtf8(path.wstring()));
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());
  if (size > kMaxStorageFileSize) {
    throw std::runtime_error(FileSizeLimitFailure("Cannot inspect application data file", path, size, kMaxStorageFileSize));
  }
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());

  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open application data file: " + utf::ToUtf8(path.wstring()));
  std::uint64_t hash = 1469598103934665603ULL;
  char buffer[8192];
  std::uintmax_t hashed_size = 0;
  while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
    const auto count = input.gcount();
    hashed_size += static_cast<std::uintmax_t>(count);
    if (hashed_size > kMaxStorageFileSize) {
      throw std::runtime_error(FileSizeLimitFailure("Cannot inspect application data file", path,
          hashed_size, kMaxStorageFileSize));
    }
    for (std::streamsize index = 0; index < count; ++index) {
      hash ^= static_cast<unsigned char>(buffer[index]);
      hash *= 1099511628211ULL;
    }
  }
  if (!input.eof()) throw std::runtime_error("Cannot read application data file completely: " + utf::ToUtf8(path.wstring()));
  return StorageFingerprint{hashed_size, write_time, hash};
}

struct FileSnapshot {
  std::string contents;
  std::optional<StorageFingerprint> fingerprint;
};

FileSnapshot ReadFileSnapshot(const std::filesystem::path& path) {
  const auto before = FingerprintOf(path);
  if (!before) return {{}, std::nullopt};

  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open application data file: " + utf::ToUtf8(path.wstring()));
  std::string contents(static_cast<size_t>(before->size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
      throw std::runtime_error("Cannot read application data file completely: " + utf::ToUtf8(path.wstring()));
    }
  }

  const auto after = FingerprintOf(path);
  if (before != after) {
    throw StorageConflictError("Application data file changed while it was being read: " + utf::ToUtf8(path.wstring()));
  }
  return {std::move(contents), after};
}

void VerifyFingerprint(const std::filesystem::path& path,
    const std::optional<StorageFingerprint>& expected) {
  if (FingerprintOf(path) != expected) {
    throw StorageConflictError("Application data file was changed by another process: " + utf::ToUtf8(path.wstring()));
  }
}

void WriteAtomically(const std::filesystem::path& path, std::string_view contents,
    const std::optional<StorageFingerprint>& expected) {
  if (!windows_path::IsWithinLimit(path)) {
    throw std::runtime_error("Application data path is too long: " + utf::ToUtf8(windows_path::LengthError(path)));
  }
  if (contents.size() > kMaxStorageFileSize) {
    throw std::runtime_error(FileSizeLimitFailure("Cannot save application data file", path,
        contents.size(), kMaxStorageFileSize));
  }
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
  if (error) throw std::runtime_error("Cannot create application data directory: " + error.message());
  VerifyFingerprint(path, expected);
  const auto temporary_base = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
  if (!windows_path::IsWithinLimit(std::filesystem::path(temporary_base))) {
    throw std::runtime_error("Temporary application data path is too long: " +
        utf::ToUtf8(windows_path::LengthError(std::filesystem::path(temporary_base))));
  }
  std::filesystem::path temporary;
  bool allocated = false;
  for (unsigned suffix = 0; suffix != 1000; ++suffix) {
    auto candidate = temporary_base;
    if (suffix != 0) candidate += L"." + std::to_wstring(suffix);
    temporary = std::filesystem::path(std::move(candidate));
    if (!windows_path::IsWithinLimit(temporary)) {
      throw std::runtime_error("Temporary application data path is too long: " +
          utf::ToUtf8(windows_path::LengthError(temporary)));
    }
    error.clear();
    const bool exists = std::filesystem::exists(temporary, error);
    if (error) throw std::runtime_error("Cannot inspect temporary application data path: " + error.message());
    if (!exists) {
      allocated = true;
      break;
    }
  }
  if (!allocated) throw std::runtime_error("Cannot allocate a temporary application data file.");
  try {
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("Cannot write application data.");
      output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      output.flush();
      if (!output) throw std::runtime_error("Cannot write application data.");
    }
    // The profile mutex serializes cooperating writers; this check also
    // rejects an external writer that changed the file while the temporary
    // contents were built.
    bool replaced = false;
    DWORD last_error = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt != 50; ++attempt) {
      // Recheck before every retry as well. A transient sharing violation must
      // not turn into an opportunity to overwrite a file changed meanwhile.
      VerifyFingerprint(path, expected);
      if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        replaced = true;
        break;
      }
      last_error = GetLastError();
      if (last_error != ERROR_ACCESS_DENIED && last_error != ERROR_SHARING_VIOLATION &&
          last_error != ERROR_LOCK_VIOLATION) {
        break;
      }
      if (attempt + 1 != 50) Sleep(10);
    }
    if (!replaced) {
      throw std::runtime_error("Cannot save application data " + utf::ToUtf8(path.wstring()) +
          ": " + utf::ToUtf8(utf::LastErrorMessage(last_error)) +
          " (Win32 " + std::to_string(last_error) + ")");
    }
  } catch (...) {
    std::filesystem::remove(temporary, error);
    throw;
  }
}

class StorageMutex final {
 public:
  explicit StorageMutex(const StorageLayout& layout) {
    const auto name = StorageMutexName(layout);
    handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!handle_) {
      const DWORD last_error = GetLastError();
      throw std::runtime_error("Cannot create application data mutex: " + utf::ToUtf8(utf::LastErrorMessage(last_error)));
    }
    const DWORD wait = WaitForSingleObject(handle_, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      acquired_ = true;
      return;
    }
    const DWORD last_error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    CloseHandle(handle_);
    handle_ = nullptr;
    throw std::runtime_error("Cannot acquire application data mutex: " + utf::ToUtf8(utf::LastErrorMessage(last_error)));
  }

  ~StorageMutex() {
    if (acquired_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
  }

  StorageMutex(const StorageMutex&) = delete;
  StorageMutex& operator=(const StorageMutex&) = delete;

 private:
  HANDLE handle_{};
  bool acquired_{false};
};

bool IsValidHistoryItem(const domain::HistoryItem& item) noexcept {
  const auto mode = static_cast<int>(item.mode);
  return !item.database_id.empty() &&
      mode >= static_cast<int>(domain::LaunchMode::enterprise) &&
      mode <= static_cast<int>(domain::LaunchMode::web_client);
}

void AppendHistoryToState(CatalogState& state, domain::HistoryItem item) {
  if (!IsValidHistoryItem(item)) return;
  state.history.erase(std::remove_if(state.history.begin(), state.history.end(), [&](const auto& existing) {
    return domain::EqualIdentifier(existing.database_id, item.database_id);
  }), state.history.end());
  const auto id = item.database_id;
  const auto timestamp = item.timestamp;
  state.history.insert(state.history.begin(), std::move(item));
  if (state.history.size() > kMaxHistory) state.history.resize(kMaxHistory);
  if (!id.empty()) state.last_launches[id] = timestamp;
}

void AddFavorite(CatalogState& state, std::wstring favorite) {
  if (favorite.empty()) return;
  if (std::none_of(state.favorites.begin(), state.favorites.end(), [&](const auto& existing) {
        return domain::EqualIdentifier(existing, favorite);
      })) {
    if (state.favorites.size() >= kMaxFavorites) return;
    state.favorites.push_back(std::move(favorite));
  }
}

void AddHistory(CatalogState& state, domain::HistoryItem item) {
  if (!IsValidHistoryItem(item)) return;
  const auto existing = std::find_if(state.history.begin(), state.history.end(), [&](const auto& value) {
    return domain::EqualIdentifier(value.database_id, item.database_id);
  });
  if (existing == state.history.end()) {
    if (state.history.size() >= kMaxHistory) return;
    state.history.push_back(std::move(item));
  } else if (existing->timestamp < item.timestamp) {
    *existing = std::move(item);
  }
}

void AddLastLaunch(LastLaunchTimes& launches, std::wstring id,
    std::chrono::system_clock::time_point timestamp) {
  const auto [existing, inserted] = launches.emplace(std::move(id), timestamp);
  if (!inserted && existing->second < timestamp) existing->second = timestamp;
}

void AddTagAssignment(DatabaseTags& tags, std::wstring id, std::vector<std::wstring> values) {
  if (id.empty() || values.empty()) return;
  auto [assignment, inserted] = tags.emplace(std::move(id), std::vector<std::wstring>{});
  static_cast<void>(inserted);
  for (auto& value : values) {
    if (std::none_of(assignment->second.begin(), assignment->second.end(), [&](const auto& existing) {
          return domain::EqualIdentifier(existing, value);
        })) {
      assignment->second.push_back(std::move(value));
    }
  }
}

}  // namespace

namespace {

std::uint64_t StoragePathHash(const StorageLayout& layout) {
  const auto key = NormalizedStoragePath(layout.root);
  std::uint64_t hash = 1469598103934665603ULL;
  for (const wchar_t character : key) {
    hash ^= static_cast<std::uint16_t>(character);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::wstring ProfileMutexName(std::wstring_view kind, const StorageLayout& layout) {
  std::wostringstream name;
  name << L"Global\\IBStart." << kind << L"." << std::hex << std::setw(16) << std::setfill(L'0') << StoragePathHash(layout);
  return name.str();
}

}  // namespace

std::wstring StorageMutexName(const StorageLayout& layout) {
  return ProfileMutexName(L"Storage", layout);
}

std::wstring InstanceMutexName(const StorageLayout& layout) {
  return ProfileMutexName(L"Instance", layout);
}

void NormalizeCatalogState(CatalogState& state) {
  std::vector<std::wstring> favorites;
  favorites.reserve(std::min(state.favorites.size(), kMaxFavorites));
  for (auto& favorite : state.favorites) {
    if (favorite.empty() || std::any_of(favorites.begin(), favorites.end(), [&](const auto& existing) {
          return domain::EqualIdentifier(existing, favorite);
        })) {
      continue;
    }
    favorites.push_back(std::move(favorite));
    if (favorites.size() == kMaxFavorites) break;
  }
  state.favorites = std::move(favorites);

  std::vector<domain::HistoryItem> history;
  history.reserve(std::min(state.history.size(), kMaxHistory));
  for (auto& item : state.history) {
    if (!IsValidHistoryItem(item)) continue;
    const auto existing = std::find_if(history.begin(), history.end(), [&](const auto& value) {
      return domain::EqualIdentifier(value.database_id, item.database_id);
    });
    if (existing != history.end()) {
      if (existing->timestamp < item.timestamp) *existing = std::move(item);
      continue;
    }
    if (history.size() == kMaxHistory) continue;
    history.push_back(std::move(item));
  }
  state.history = std::move(history);
}

namespace {

Settings ParseSettings(std::string_view contents) {
  Settings result;
  if (const auto root = json::RootObject(contents)) {
    const auto credential_schema = json::ObjectInt(*root, "credentials_schema_version");
    const bool supported_credentials = !credential_schema || *credential_schema == 1;
    if (const auto active = json::ObjectString(*root, "active_ibases")) result.active_ibases = *active;
    if (const auto selected = json::ObjectString(*root, "selected_entry")) result.selected_entry = *selected;
    if (const auto simple = json::ObjectInt(*root, "simple_mode")) result.simple_mode = *simple != 0;
    if (const auto show_tags = json::ObjectInt(*root, "show_tags_in_list")) result.show_tags_in_list = *show_tags != 0;
    if (const auto folders_first = json::ObjectInt(*root, "folders_first_when_sorting")) result.folders_first_when_sorting = *folders_first != 0;
    if (const auto x = json::ObjectInt(*root, "window_x")) result.window_x = *x;
    if (const auto y = json::ObjectInt(*root, "window_y")) result.window_y = *y;
    if (const auto width = json::ObjectInt(*root, "window_width")) result.window_width = std::clamp(*width, 480, 10000);
    if (const auto height = json::ObjectInt(*root, "window_height")) result.window_height = std::clamp(*height, 320, 10000);
    json::ForEachArrayObject(*root, "platform_paths", [&](const json::Object& object) {
      if (const auto path = json::ObjectString(object, "platform_path")) result.platform_search_paths.emplace_back(*path);
    });
    json::ForEachArrayObject(*root, "recent_lists", [&](const json::Object& object) {
      if (const auto recent = json::ObjectString(object, "recent_list")) result.recent_ibases.emplace_back(*recent);
    });
    json::ForEachArrayObject(*root, "credentials", [&](const json::Object& object) {
      credentials::Credential c;
      const auto id = json::ObjectString(object, "id");
      const auto title = json::ObjectString(object, "title");
      const auto user = json::ObjectString(object, "user_name");
      const auto password = json::ObjectString(object, "password");
      if (!id || !title || !user || !password) return;
      c.id = *id; c.title = *title; c.user_name = *user; c.password = *password;
      const auto has_nul = [](std::wstring_view value) { return value.find(L'\0') != std::wstring_view::npos; };
      const auto blank = [](std::wstring_view value) { return value.empty() || std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(c) != 0; }); };
      if (blank(c.id) || blank(c.title) || blank(c.user_name) || has_nul(c.id) || has_nul(c.title) || has_nul(c.user_name) || has_nul(c.password)) {
        c.scope = credentials::ScopeMode::invalid;
      }
      const auto* scope = json::ObjectValue(object, "scope");
      if (!scope || scope->kind != json::ValueKind::object) { c.scope = credentials::ScopeMode::invalid; result.credentials.push_back(std::move(c)); return; }
      const auto scope_object = json::RootObject(scope->raw);
      if (!scope_object) { c.scope = credentials::ScopeMode::invalid; result.credentials.push_back(std::move(c)); return; }
      const auto mode = json::ObjectString(*scope_object, "mode");
      if (c.scope != credentials::ScopeMode::invalid) {
        if (mode && *mode == L"all") c.scope = credentials::ScopeMode::all;
        else if (mode && *mode == L"selected") c.scope = credentials::ScopeMode::selected;
        else c.scope = credentials::ScopeMode::invalid;
      }
      json::ForEachArrayObject(*scope_object, "targets", [&](const json::Object& target) {
        const auto path = json::ObjectString(target, "catalog_path");
        const auto kind = json::ObjectString(target, "kind");
        const auto entry = json::ObjectString(target, "entry_id");
        const auto name = json::ObjectString(target, "last_known_name");
        if (!path || !kind || !entry || !name || *kind != L"database" && *kind != L"group") { c.scope = credentials::ScopeMode::invalid; return; }
        const auto normalized = credentials::NormalizeCatalogPath(*path);
        if (normalized.empty() || entry->find(L'\0') != std::wstring::npos || name->find(L'\0') != std::wstring::npos) { c.scope = credentials::ScopeMode::invalid; return; }
        c.targets.push_back({normalized, *kind == L"database" ? credentials::TargetKind::database : credentials::TargetKind::group, *entry, *name});
      });
      if (c.scope == credentials::ScopeMode::selected && c.targets.empty()) c.scope = credentials::ScopeMode::invalid;
      result.credentials.push_back(std::move(c));
    });
    if (!supported_credentials) for (auto& credential : result.credentials) credential.scope = credentials::ScopeMode::invalid;
    for (std::size_t i = 0; i < result.credentials.size(); ++i) {
      for (std::size_t j = i + 1; j < result.credentials.size(); ++j) {
        if (domain::EqualIdentifier(result.credentials[i].id, result.credentials[j].id) ||
            domain::EqualIdentifier(result.credentials[i].title, result.credentials[j].title)) {
          result.credentials[i].scope = credentials::ScopeMode::invalid;
          result.credentials[j].scope = credentials::ScopeMode::invalid;
        }
      }
    }
  }
  return result;
}

std::string SerializeSettings(const Settings& settings) {
  std::string json = "{\n  \"active_ibases\": \"" + ::ibstart::storage::json::Escape(settings.active_ibases.wstring()) + "\",\n";
  json += "  \"selected_entry\": \"" + ::ibstart::storage::json::Escape(settings.selected_entry) + "\",\n";
  json += "  \"simple_mode\": " + std::string(settings.simple_mode ? "1" : "0") + ",\n";
  json += "  \"show_tags_in_list\": " + std::string(settings.show_tags_in_list ? "1" : "0") + ",\n";
  json += "  \"folders_first_when_sorting\": " + std::string(settings.folders_first_when_sorting ? "1" : "0") + ",\n";
  json += "  \"window_x\": " + std::to_string(settings.window_x) + ",\n  \"window_y\": " + std::to_string(settings.window_y);
  json += ",\n  \"window_width\": " + std::to_string(settings.window_width) + ",\n  \"window_height\": " + std::to_string(settings.window_height) + ",\n  \"recent_lists\": [";
  for (std::size_t index = 0; index < settings.recent_ibases.size(); ++index) {
    if (index) json += ", ";
    json += "{\"recent_list\": \"" + ::ibstart::storage::json::Escape(settings.recent_ibases[index].wstring()) + "\"}";
  }
  json += "],\n  \"platform_paths\": [";
  for (std::size_t index = 0; index < settings.platform_search_paths.size(); ++index) {
    if (index) json += ", ";
    json += "{\"platform_path\": \"" + ::ibstart::storage::json::Escape(settings.platform_search_paths[index].wstring()) + "\"}";
  }
  json += "],\n  \"credentials_schema_version\": 1,\n  \"credentials\": [";
  for (std::size_t i = 0; i < settings.credentials.size(); ++i) {
    if (i) json += ", ";
    const auto& c = settings.credentials[i];
    const auto mode = c.scope == credentials::ScopeMode::all ? "all" : c.scope == credentials::ScopeMode::selected ? "selected" : "invalid";
    json += "{\"id\": \"" + json::Escape(c.id) + "\", \"title\": \"" + json::Escape(c.title) +
        "\", \"user_name\": \"" + json::Escape(c.user_name) + "\", \"password\": \"" + json::Escape(c.password) +
        "\", \"scope\": {\"mode\": \"" + mode + "\", \"targets\": [";
    for (std::size_t j = 0; j < c.targets.size(); ++j) {
      if (j) json += ", ";
      const auto& t = c.targets[j];
      json += "{\"catalog_path\": \"" + json::Escape(t.catalog_path.wstring()) + "\", \"kind\": \"" +
          std::string(t.kind == credentials::TargetKind::database ? "database" : "group") + "\", \"entry_id\": \"" +
          json::Escape(t.entry_id) + "\", \"last_known_name\": \"" + json::Escape(t.last_known_name) + "\"}";
    }
    json += "]}}";
  }
  json += "]\n}\n";
  return json;
}

CatalogState ParseCatalogState(std::string_view contents) {
  CatalogState result;
  if (const auto root = json::RootObject(contents)) {
    json::ForEachArrayObject(*root, "favorites", [&](const json::Object& object) {
      if (const auto favorite = json::ObjectString(object, "favorite")) AddFavorite(result, *favorite);
    });
    json::ForEachArrayObject(*root, "history", [&](const json::Object& object) {
      const auto history_id = json::ObjectString(object, "history_id");
      const auto history_time = json::ObjectInteger(object, "time");
      const auto history_mode = json::ObjectInt(object, "mode");
      if (history_id && history_time && history_mode) {
        AddHistory(result, {*history_id, std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*history_time)),
            static_cast<domain::LaunchMode>(*history_mode)});
      }
    });
    json::ForEachArrayObject(*root, "last_launches", [&](const json::Object& object) {
      const auto launch_id = json::ObjectString(object, "last_launch_id");
      const auto launch_time = json::ObjectInteger(object, "time");
      if (launch_id && launch_time && !launch_id->empty()) {
        AddLastLaunch(result.last_launches, *launch_id,
            std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*launch_time)));
      }
    });
    json::ForEachArrayObject(*root, "tags", [&](const json::Object& object) {
      const auto tag_id = json::ObjectString(object, "tag_id");
      const auto tags = json::StringArray(json::ObjectValue(object, "values"));
      if (tag_id && tags) AddTagAssignment(result.tags, *tag_id, *tags);
    });
    json::ForEachArrayObject(*root, "tag_styles", [&](const json::Object& object) {
      const auto style_name = json::ObjectString(object, "tag_style");
      const auto background = json::ObjectInteger(object, "background");
      const auto text = json::ObjectInteger(object, "text");
      if (style_name && !style_name->empty() && background && text && *background >= 0 && *text >= 0 &&
          *background <= 0xFFFFFF && *text <= 0xFFFFFF) {
        result.tag_styles[*style_name] = {static_cast<COLORREF>(*background), static_cast<COLORREF>(*text)};
      }
    });
  }

  NormalizeCatalogState(result);
  for (const auto& history : result.history) {
    if (!history.database_id.empty() && !result.last_launches.contains(history.database_id)) {
      result.last_launches.emplace(history.database_id, history.timestamp);
    }
  }
  return result;
}

std::string SerializeCatalogState(const CatalogState& state) {
  CatalogState normalized = state;
  NormalizeCatalogState(normalized);
  std::string json = "{\n  \"schema_version\": 1,\n  \"favorites\": [";
  for (std::size_t index = 0; index < normalized.favorites.size(); ++index) {
    if (index) json += ", ";
    json += "{\"favorite\": \"" + ::ibstart::storage::json::Escape(normalized.favorites[index]) + "\"}";
  }
  json += "],\n  \"history\": [";
  for (std::size_t index = 0; index < normalized.history.size(); ++index) {
    const auto& record = normalized.history[index];
    if (index) json += ", ";
    json += "{\"history_id\": \"" + ::ibstart::storage::json::Escape(record.database_id) + "\", \"time\": " +
        std::to_string(std::chrono::system_clock::to_time_t(record.timestamp)) + ", \"mode\": " +
        std::to_string(static_cast<int>(record.mode)) + "}";
  }
  json += "],\n  \"last_launches\": [";
  std::size_t written = 0;
  for (const auto& [id, timestamp] : normalized.last_launches) {
    if (id.empty()) continue;
    if (written++) json += ", ";
    json += "{\"last_launch_id\": \"" + ::ibstart::storage::json::Escape(id) + "\", \"time\": " +
        std::to_string(std::chrono::system_clock::to_time_t(timestamp)) + "}";
  }
  json += "],\n  \"tags\": [";
  written = 0;
  for (const auto& [id, values] : normalized.tags) {
    if (id.empty() || values.empty()) continue;
    if (written++) json += ", ";
    json += "{\"tag_id\": \"" + ::ibstart::storage::json::Escape(id) + "\", \"values\": [";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index) json += ", ";
      json += "\"" + ::ibstart::storage::json::Escape(values[index]) + "\"";
    }
    json += "]}";
  }
  json += "],\n  \"tag_styles\": [";
  written = 0;
  for (const auto& [tag, style] : normalized.tag_styles) {
    if (tag.empty()) continue;
    if (written++) json += ", ";
    json += "{\"tag_style\": \"" + ::ibstart::storage::json::Escape(tag) + "\", \"background\": " +
        std::to_string(style.background) + ", \"text\": " + std::to_string(style.text) + "}";
  }
  json += "]\n}\n";
  return json;
}

void SaveFile(const StorageLayout& layout, const std::filesystem::path& path, std::string_view contents) {
  StorageMutex mutex(layout);
  const auto expected = FingerprintOf(path);
  WriteAtomically(path, contents, expected);
}

std::optional<StorageFingerprint> TryFingerprintOf(const std::filesystem::path& path) noexcept {
  try {
    return FingerprintOf(path);
  } catch (...) {
    return std::nullopt;
  }
}

void MergeChangedSettings(Settings& target, const Settings& baseline, const Settings& requested) {
  if (requested.active_ibases != baseline.active_ibases) target.active_ibases = requested.active_ibases;
  if (requested.selected_entry != baseline.selected_entry) target.selected_entry = requested.selected_entry;
  if (requested.simple_mode != baseline.simple_mode) target.simple_mode = requested.simple_mode;
  if (requested.show_tags_in_list != baseline.show_tags_in_list) target.show_tags_in_list = requested.show_tags_in_list;
  if (requested.folders_first_when_sorting != baseline.folders_first_when_sorting) {
    target.folders_first_when_sorting = requested.folders_first_when_sorting;
  }
  if (requested.recent_ibases != baseline.recent_ibases) target.recent_ibases = requested.recent_ibases;
  if (requested.platform_search_paths != baseline.platform_search_paths) {
    target.platform_search_paths = requested.platform_search_paths;
  }
  if (requested.window_x != baseline.window_x) target.window_x = requested.window_x;
  if (requested.window_y != baseline.window_y) target.window_y = requested.window_y;
  if (requested.window_width != baseline.window_width) target.window_width = requested.window_width;
  if (requested.window_height != baseline.window_height) target.window_height = requested.window_height;
  // Credentials are merged by record ID so unrelated edits from another
  // window survive. A record changed in both snapshots is a real conflict.
  const auto find = [](auto& values, const auto& id) {
    return std::find_if(values.begin(), values.end(), [&](const auto& value) { return domain::EqualIdentifier(value.id, id); });
  };
  for (const auto& old : baseline.credentials) {
    const auto old_requested = find(requested.credentials, old.id);
    const auto old_latest = find(target.credentials, old.id);
    const bool requested_changed = old_requested == requested.credentials.end() || *old_requested != old;
    const bool latest_changed = old_latest == target.credentials.end() || *old_latest != old;
    if (requested_changed && latest_changed && !(old_latest == target.credentials.end() && old_requested == requested.credentials.end()) &&
        (old_latest == target.credentials.end() || old_requested == requested.credentials.end() || *old_latest != *old_requested)) {
      throw StorageConflictError("Credential record was changed by another process: " + utf::ToUtf8(old.id));
    }
  }
  for (const auto& value : requested.credentials) {
    const auto old = find(baseline.credentials, value.id);
    if (old == baseline.credentials.end()) {
      const auto latest = find(target.credentials, value.id);
      if (latest != target.credentials.end() && *latest != value) throw StorageConflictError("Credential record ID was added by another process: " + utf::ToUtf8(value.id));
      if (latest == target.credentials.end()) target.credentials.push_back(value);
    } else if (*old != value) {
      auto latest = find(target.credentials, value.id);
      if (latest == target.credentials.end()) target.credentials.push_back(value); else *latest = value;
    }
  }
  for (const auto& old : baseline.credentials) if (find(requested.credentials, old.id) == requested.credentials.end()) {
    const auto latest = find(target.credentials, old.id);
    if (latest != target.credentials.end()) target.credentials.erase(latest);
  }
  for (const auto& value : target.credentials) {
    if (value.scope == credentials::ScopeMode::invalid) continue;
    if (!credentials::Validate(value, target.credentials).empty()) throw StorageConflictError("Invalid or duplicate credential record.");
  }
}

}  // namespace

Settings LoadSettings(const StorageLayout& layout) {
  StorageMutex mutex(layout);
  const auto snapshot = ReadFileSnapshot(PathFor(layout, L"settings.json"));
  return ParseSettings(snapshot.contents);
}

void SaveSettings(const StorageLayout& layout, const Settings& settings) {
  SaveFile(layout, PathFor(layout, L"settings.json"), SerializeSettings(settings));
}

CatalogState LoadCatalogState(const StorageLayout& layout) {
  StorageMutex mutex(layout);
  const auto snapshot = ReadFileSnapshot(PathFor(layout, L"catalog-state.json"));
  return ParseCatalogState(snapshot.contents);
}

void SaveCatalogState(const StorageLayout& layout, const CatalogState& state) {
  SaveFile(layout, PathFor(layout, L"catalog-state.json"), SerializeCatalogState(state));
}

SettingsRepository::SettingsRepository(StorageLayout layout) : layout_(std::move(layout)) {}

const Settings& SettingsRepository::Read() {
  if (!settings_) {
    StorageMutex mutex(layout_);
    const auto snapshot = ReadFileSnapshot(PathFor(layout_, L"settings.json"));
    settings_ = ParseSettings(snapshot.contents);
    fingerprint_ = snapshot.fingerprint;
  }
  return *settings_;
}

const Settings& SettingsRepository::Reload() {
  StorageMutex mutex(layout_);
  const auto snapshot = ReadFileSnapshot(PathFor(layout_, L"settings.json"));
  settings_ = ParseSettings(snapshot.contents);
  fingerprint_ = snapshot.fingerprint;
  return *settings_;
}

void SettingsRepository::Update(const std::function<void(Settings&)>& mutation) {
  StorageMutex mutex(layout_);
  const auto path = PathFor(layout_, L"settings.json");
  const auto snapshot = ReadFileSnapshot(path);
  Settings updated = ParseSettings(snapshot.contents);
  mutation(updated);
  WriteAtomically(path, SerializeSettings(updated), snapshot.fingerprint);
  settings_ = std::move(updated);
  fingerprint_ = TryFingerprintOf(path);
}

void SettingsRepository::Save(const Settings& settings) {
  const auto baseline = settings_;
  const auto cached_fingerprint = fingerprint_;
  StorageMutex mutex(layout_);
  const auto path = PathFor(layout_, L"settings.json");
  const auto snapshot = ReadFileSnapshot(path);
  Settings updated = ParseSettings(snapshot.contents);
  if (!baseline || cached_fingerprint == snapshot.fingerprint) updated = settings;
  else MergeChangedSettings(updated, *baseline, settings);
  WriteAtomically(path, SerializeSettings(updated), snapshot.fingerprint);
  settings_ = std::move(updated);
  fingerprint_ = TryFingerprintOf(path);
}

CatalogStateRepository::CatalogStateRepository(StorageLayout layout) : layout_(std::move(layout)) {}

const CatalogState& CatalogStateRepository::Read() {
  if (!state_) {
    StorageMutex mutex(layout_);
    const auto snapshot = ReadFileSnapshot(PathFor(layout_, L"catalog-state.json"));
    state_ = ParseCatalogState(snapshot.contents);
  }
  return *state_;
}

const CatalogState& CatalogStateRepository::Reload() {
  StorageMutex mutex(layout_);
  const auto snapshot = ReadFileSnapshot(PathFor(layout_, L"catalog-state.json"));
  state_ = ParseCatalogState(snapshot.contents);
  return *state_;
}

void CatalogStateRepository::Update(const std::function<void(CatalogState&)>& mutation) {
  StorageMutex mutex(layout_);
  const auto path = PathFor(layout_, L"catalog-state.json");
  const auto snapshot = ReadFileSnapshot(path);
  // state_ is a UI cache and may predate a transaction committed by another
  // process. Always mutate the snapshot read while holding the mutex.
  CatalogState updated = ParseCatalogState(snapshot.contents);
  mutation(updated);
  NormalizeCatalogState(updated);
  WriteAtomically(path, SerializeCatalogState(updated), snapshot.fingerprint);
  state_ = std::move(updated);
}

void CatalogStateRepository::AppendHistory(domain::HistoryItem item) {
  Update([&](CatalogState& state) { AppendHistoryToState(state, std::move(item)); });
}

void CatalogStateRepository::ClearHistory() {
  Update([](CatalogState& state) { state.history.clear(); });
}

}  // namespace ibstart::storage
