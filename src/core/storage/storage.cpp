#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"
#include "core/storage/json_codec.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ibstart::storage {
namespace {

std::filesystem::path PathFor(const StorageLayout& layout, std::wstring_view name) {
  return layout.root / std::wstring(name);
}

void WriteAtomically(const std::filesystem::path& path, std::string_view contents) {
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
  if (error) throw std::runtime_error("Cannot create application data directory: " + error.message());
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
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::runtime_error("Cannot save application data: " + utf::ToUtf8(utf::LastErrorMessage()));
    }
  } catch (...) {
    std::filesystem::remove(temporary, error);
    throw;
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());
  if (!exists) return {};
  if (!std::filesystem::is_regular_file(path, error)) {
    if (error) throw std::runtime_error("Cannot inspect application data file: " + utf::ToUtf8(path.wstring()) + ": " + error.message());
    throw std::runtime_error("Application data path is not a regular file: " + utf::ToUtf8(path.wstring()));
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open application data file: " + utf::ToUtf8(path.wstring()));
  const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) throw std::runtime_error("Cannot read application data file completely: " + utf::ToUtf8(path.wstring()));
  return contents;
}

void AppendHistoryToState(CatalogState& state, domain::HistoryItem item) {
  state.history.erase(std::remove_if(state.history.begin(), state.history.end(), [&](const auto& existing) {
    return existing.database_id == item.database_id;
  }), state.history.end());
  const auto id = item.database_id;
  const auto timestamp = item.timestamp;
  state.history.insert(state.history.begin(), std::move(item));
  if (state.history.size() > 20) state.history.resize(20);
  if (!id.empty()) state.last_launches[id] = timestamp;
}

}  // namespace

Settings LoadSettings(const StorageLayout& layout) {
  Settings result;
  const auto contents = ReadFile(PathFor(layout, L"settings.json"));
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

void SaveSettings(const StorageLayout& layout, const Settings& settings) {
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
  WriteAtomically(PathFor(layout, L"settings.json"), json);
}

CatalogState LoadCatalogState(const StorageLayout& layout) {
  CatalogState result;
  const auto contents = ReadFile(PathFor(layout, L"catalog-state.json"));
  if (const auto root = json::RootObject(contents)) {
    json::ForEachArrayObject(*root, "favorites", [&](const json::Object& object) {
      if (const auto favorite = json::ObjectString(object, "favorite")) result.favorites.push_back(*favorite);
    });
    json::ForEachArrayObject(*root, "history", [&](const json::Object& object) {
      const auto history_id = json::ObjectString(object, "history_id");
      const auto history_time = json::ObjectInteger(object, "time");
      const auto history_mode = json::ObjectInt(object, "mode");
      if (history_id && history_time && history_mode &&
          *history_mode >= static_cast<int>(domain::LaunchMode::enterprise) &&
          *history_mode <= static_cast<int>(domain::LaunchMode::web_client)) {
        result.history.push_back({*history_id, std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*history_time)),
            static_cast<domain::LaunchMode>(*history_mode)});
      }
    });
    json::ForEachArrayObject(*root, "last_launches", [&](const json::Object& object) {
      const auto launch_id = json::ObjectString(object, "last_launch_id");
      const auto launch_time = json::ObjectInteger(object, "time");
      if (launch_id && launch_time && !launch_id->empty()) {
        result.last_launches[*launch_id] = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*launch_time));
      }
    });
    json::ForEachArrayObject(*root, "tags", [&](const json::Object& object) {
      const auto tag_id = json::ObjectString(object, "tag_id");
      const auto tags = json::StringArray(json::ObjectValue(object, "values"));
      if (tag_id && !tag_id->empty() && tags && !tags->empty()) result.tags[*tag_id] = *tags;
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

  for (const auto& history : result.history) {
    if (!history.database_id.empty() && !result.last_launches.contains(history.database_id)) {
      result.last_launches[history.database_id] = history.timestamp;
    }
  }
  return result;
}

void SaveCatalogState(const StorageLayout& layout, const CatalogState& state) {
  std::string json = "{\n  \"schema_version\": 1,\n  \"favorites\": [";
  for (std::size_t index = 0; index < state.favorites.size(); ++index) {
    if (index) json += ", ";
    json += "{\"favorite\": \"" + ::ibstart::storage::json::Escape(state.favorites[index]) + "\"}";
  }
  json += "],\n  \"history\": [";
  for (std::size_t index = 0; index < state.history.size(); ++index) {
    const auto& record = state.history[index];
    if (index) json += ", ";
    json += "{\"history_id\": \"" + ::ibstart::storage::json::Escape(record.database_id) + "\", \"time\": " +
        std::to_string(std::chrono::system_clock::to_time_t(record.timestamp)) + ", \"mode\": " +
        std::to_string(static_cast<int>(record.mode)) + "}";
  }
  json += "],\n  \"last_launches\": [";
  std::size_t written = 0;
  for (const auto& [id, timestamp] : state.last_launches) {
    if (id.empty()) continue;
    if (written++) json += ", ";
    json += "{\"last_launch_id\": \"" + ::ibstart::storage::json::Escape(id) + "\", \"time\": " +
        std::to_string(std::chrono::system_clock::to_time_t(timestamp)) + "}";
  }
  json += "],\n  \"tags\": [";
  written = 0;
  for (const auto& [id, values] : state.tags) {
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
  for (const auto& [tag, style] : state.tag_styles) {
    if (tag.empty()) continue;
    if (written++) json += ", ";
    json += "{\"tag_style\": \"" + ::ibstart::storage::json::Escape(tag) + "\", \"background\": " +
        std::to_string(style.background) + ", \"text\": " + std::to_string(style.text) + "}";
  }
  json += "]\n}\n";
  WriteAtomically(PathFor(layout, L"catalog-state.json"), json);
}

CatalogStateRepository::CatalogStateRepository(StorageLayout layout) : layout_(std::move(layout)) {}

const CatalogState& CatalogStateRepository::Read() {
  if (!state_) state_ = LoadCatalogState(layout_);
  return *state_;
}

const CatalogState& CatalogStateRepository::Reload() {
  state_ = LoadCatalogState(layout_);
  return *state_;
}

void CatalogStateRepository::Update(const std::function<void(CatalogState&)>& mutation) {
  CatalogState updated = Read();
  mutation(updated);
  SaveCatalogState(layout_, updated);
  state_ = std::move(updated);
}

void CatalogStateRepository::AppendHistory(domain::HistoryItem item) {
  Update([&](CatalogState& state) { AppendHistoryToState(state, std::move(item)); });
}

void CatalogStateRepository::ClearHistory() {
  Update([](CatalogState& state) { state.history.clear(); });
}

}  // namespace ibstart::storage
