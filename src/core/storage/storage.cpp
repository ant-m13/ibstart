#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"
#include "core/storage/json_codec.hpp"

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
  return layout.root / std::wstring(name);
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

std::optional<StorageFingerprint> FingerprintOf(const std::filesystem::path& path) {
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
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());

  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open application data file: " + utf::ToUtf8(path.wstring()));
  std::uint64_t hash = 1469598103934665603ULL;
  char buffer[8192];
  while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
    for (std::streamsize index = 0; index < input.gcount(); ++index) {
      hash ^= static_cast<unsigned char>(buffer[index]);
      hash *= 1099511628211ULL;
    }
  }
  if (!input.eof()) throw std::runtime_error("Cannot read application data file completely: " + utf::ToUtf8(path.wstring()));
  return StorageFingerprint{size, write_time, hash};
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
  std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) throw std::runtime_error("Cannot read application data file completely: " + utf::ToUtf8(path.wstring()));

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
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
  if (error) throw std::runtime_error("Cannot create application data directory: " + error.message());
  VerifyFingerprint(path, expected);
  const auto temporary_base = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
  std::filesystem::path temporary;
  bool allocated = false;
  for (unsigned suffix = 0; suffix != 1000; ++suffix) {
    auto candidate = temporary_base;
    if (suffix != 0) candidate += L"." + std::to_wstring(suffix);
    temporary = std::filesystem::path(std::move(candidate));
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
    VerifyFingerprint(path, expected);
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::runtime_error("Cannot save application data: " + utf::ToUtf8(utf::LastErrorMessage()));
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
