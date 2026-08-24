#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace ibstart::storage {
namespace {

std::wstring Env(std::wstring_view name) {
  const DWORD size = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
  if (!size) return {};
  std::wstring result(size, L'\0');
  if (GetEnvironmentVariableW(std::wstring(name).c_str(), result.data(), size) == 0) return {};
  result.resize(size - 1);
  return result;
}

std::filesystem::path PathFor(const StorageLayout& layout, std::wstring_view name) { return layout.root / std::wstring(name); }

std::string Escape(std::wstring_view value) {
  std::string result;
  const auto utf8 = utf::ToUtf8(value);
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char unit : utf8) {
    if (unit == '\\') result += "\\\\";
    else if (unit == '"') result += "\\\"";
    else if (unit == '\n') result += "\\n";
    else if (unit == '\r') result += "\\r";
    else if (unit == '\t') result += "\\t";
    else if (unit == '\b') result += "\\b";
    else if (unit == '\f') result += "\\f";
    else if (unit < 0x20) { result += "\\u00"; result.push_back(hex[unit >> 4]); result.push_back(hex[unit & 0x0F]); }
    else result.push_back(static_cast<char>(unit));
  }
  return result;
}

std::wstring Unescape(std::string_view value) {
  std::wstring result;
  std::string raw;
  const auto flushRaw = [&] {
    if (!raw.empty()) { result += utf::FromUtf8(raw); raw.clear(); }
  };
  const auto hexDigit = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  const auto unicodeUnit = [&](size_t offset) -> wchar_t {
    if (offset + 4 > value.size()) throw std::invalid_argument("Truncated JSON Unicode escape.");
    unsigned unit = 0;
    for (size_t digit = 0; digit < 4; ++digit) {
      const int parsed = hexDigit(value[offset + digit]);
      if (parsed < 0) throw std::invalid_argument("Invalid JSON Unicode escape.");
      unit = unit * 16 + static_cast<unsigned>(parsed);
    }
    return static_cast<wchar_t>(unit);
  };
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\') {
      if (index + 1 >= value.size()) throw std::invalid_argument("Truncated JSON escape.");
      flushRaw();
      const char next = value[++index];
      if (next == 'n') result.push_back(L'\n');
      else if (next == 'r') result.push_back(L'\r');
      else if (next == 't') result.push_back(L'\t');
      else if (next == 'b') result.push_back(L'\b');
      else if (next == 'f') result.push_back(L'\f');
      else if (next == '"' || next == '\\' || next == '/') result.push_back(static_cast<wchar_t>(next));
      else if (next == 'u') {
        const wchar_t high = unicodeUnit(index + 1);
        index += 4;
        if (high >= 0xD800 && high <= 0xDBFF) {
          if (index + 6 >= value.size() || value[index + 1] != '\\' || value[index + 2] != 'u') throw std::invalid_argument("Unpaired JSON high surrogate.");
          const wchar_t low = unicodeUnit(index + 3);
          if (low < 0xDC00 || low > 0xDFFF) throw std::invalid_argument("Invalid JSON surrogate pair.");
          result.push_back(high);
          result.push_back(low);
          index += 6;
        } else if (high >= 0xDC00 && high <= 0xDFFF) {
          throw std::invalid_argument("Unpaired JSON low surrogate.");
        } else {
          result.push_back(high);
        }
      } else {
        throw std::invalid_argument("Invalid JSON escape.");
      }
    } else {
      if (static_cast<unsigned char>(value[index]) < 0x20) throw std::invalid_argument("Unescaped JSON control character.");
      raw.push_back(value[index]);
    }
  }
  flushRaw();
  return result;
}

std::optional<std::wstring> TryUnescape(std::string_view value) {
  try {
    return Unescape(value);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void WriteAtomically(const std::filesystem::path& path, std::string_view contents) {
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
  if (error) throw std::runtime_error("Cannot create application data directory: " + error.message());
  const auto temporaryBase = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
  std::filesystem::path temporary;
  bool allocated = false;
  for (unsigned suffix = 0; suffix != 1000; ++suffix) {
    auto candidate = temporaryBase;
    if (suffix != 0) candidate += L"." + std::to_wstring(suffix);
    temporary = std::filesystem::path(std::move(candidate));
    error.clear();
    const bool exists = std::filesystem::exists(temporary, error);
    if (error) throw std::runtime_error("Cannot inspect temporary application data path: " + error.message());
    if (!exists) { allocated = true; break; }
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

std::optional<std::wstring> JsonString(std::string_view json, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  std::smatch match;
  const std::string body(json);
  if (!std::regex_search(body, match, expression)) return std::nullopt;
  return TryUnescape(match[1].str());
}

std::optional<int> JsonInteger(std::string_view json, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*(-?[0-9]+)");
  std::smatch match;
  const std::string body(json);
  if (!std::regex_search(body, match, expression)) return std::nullopt;
  try { return std::stoi(match[1].str()); } catch (...) { return std::nullopt; }
}

void SkipJsonWhitespace(std::string_view json, size_t& position) {
  while (position < json.size() && (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' || json[position] == '\n')) ++position;
}

std::optional<std::string> ReadJsonRawString(std::string_view json, size_t& position) {
  SkipJsonWhitespace(json, position);
  if (position >= json.size() || json[position++] != '"') return std::nullopt;
  std::string result;
  while (position < json.size()) {
    const char character = json[position++];
    if (character == '"') return result;
    if (character == '\\') {
      if (position >= json.size()) return std::nullopt;
      result.push_back(character);
      result.push_back(json[position++]);
    } else {
      result.push_back(character);
    }
  }
  return std::nullopt;
}

bool ConsumeJsonCharacter(std::string_view json, size_t& position, char expected) {
  SkipJsonWhitespace(json, position);
  if (position >= json.size() || json[position] != expected) return false;
  ++position;
  return true;
}

}  // namespace

StorageLayout ResolveLayout(const std::filesystem::path& executable_path) {
  const auto marker = executable_path.parent_path() / L"IBStart.portable";
  std::error_code error;
  if (std::filesystem::is_regular_file(marker, error) && std::filesystem::file_size(marker, error) == 0) {
    return {executable_path.parent_path() / L"data", true};
  }
  const auto local = Env(L"LOCALAPPDATA");
  if (local.empty()) throw std::runtime_error("LOCALAPPDATA is unavailable; cannot determine IBStart data directory.");
  return {std::filesystem::path(local) / L"IBStart", false};
}

std::optional<std::filesystem::path> FindStandardIbases() {
  const auto appData = Env(L"APPDATA");
  const auto localData = Env(L"LOCALAPPDATA");
  std::vector<std::filesystem::path> candidates;
  if (!appData.empty()) {
    candidates.emplace_back(std::filesystem::path(appData) / L"1C" / L"1CEStart" / L"ibases.v8i");
    candidates.emplace_back(std::filesystem::path(appData) / L"1C" / L"1Cv8" / L"ibases.v8i");
  }
  if (!localData.empty()) candidates.emplace_back(std::filesystem::path(localData) / L"1C" / L"1CEStart" / L"ibases.v8i");
  for (const auto& candidate : candidates) if (std::filesystem::is_regular_file(candidate)) return candidate;
  return std::nullopt;
}

void EnsureWritable(const StorageLayout& layout) {
  std::error_code error;
  std::filesystem::create_directories(layout.root / L"logs", error);
  if (error) throw std::runtime_error("IBStart data directory cannot be created: " + error.message());
  const auto probe = layout.root / (L".write_probe." + std::to_wstring(GetCurrentProcessId()));
  std::ofstream output(probe, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("IBStart data directory is not writable: " + utf::ToUtf8(layout.root.wstring()));
  output.close();
  std::filesystem::remove(probe, error);
}

Settings LoadSettings(const StorageLayout& layout) {
  Settings result;
  const auto json = ReadFile(PathFor(layout, L"settings.json"));
  if (const auto active = JsonString(json, "active_ibases")) result.active_ibases = *active;
  if (const auto selected = JsonString(json, "selected_entry")) result.selected_entry = *selected;
  if (const auto simple = JsonInteger(json, "simple_mode")) result.simple_mode = *simple != 0;
  if (const auto showTags = JsonInteger(json, "show_tags_in_list")) result.show_tags_in_list = *showTags != 0;
  if (const auto foldersFirst = JsonInteger(json, "folders_first_when_sorting")) result.folders_first_when_sorting = *foldersFirst != 0;
  if (const auto x = JsonInteger(json, "window_x")) result.window_x = *x;
  if (const auto y = JsonInteger(json, "window_y")) result.window_y = *y;
  if (const auto width = JsonInteger(json, "window_width")) result.window_width = std::clamp(*width, 480, 10000);
  if (const auto height = JsonInteger(json, "window_height")) result.window_height = std::clamp(*height, 320, 10000);
  const std::regex pathExpression("\\\"platform_path\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  for (std::sregex_iterator it(json.begin(), json.end(), pathExpression), end; it != end; ++it) {
    if (const auto path = TryUnescape((*it)[1].str())) result.platform_search_paths.emplace_back(*path);
  }
  const std::regex recentExpression("\\\"recent_list\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  for (std::sregex_iterator it(json.begin(), json.end(), recentExpression), end; it != end; ++it) {
    if (const auto recent = TryUnescape((*it)[1].str())) result.recent_ibases.emplace_back(*recent);
  }
  return result;
}

void SaveSettings(const StorageLayout& layout, const Settings& settings) {
  std::string json = "{\n  \"active_ibases\": \"" + Escape(settings.active_ibases.wstring()) + "\",\n";
  json += "  \"selected_entry\": \"" + Escape(settings.selected_entry) + "\",\n";
  json += "  \"simple_mode\": " + std::string(settings.simple_mode ? "1" : "0") + ",\n";
  json += "  \"show_tags_in_list\": " + std::string(settings.show_tags_in_list ? "1" : "0") + ",\n";
  json += "  \"folders_first_when_sorting\": " + std::string(settings.folders_first_when_sorting ? "1" : "0") + ",\n";
  json += "  \"window_x\": " + std::to_string(settings.window_x) + ",\n  \"window_y\": " + std::to_string(settings.window_y);
  json += ",\n  \"window_width\": " + std::to_string(settings.window_width) + ",\n  \"window_height\": " + std::to_string(settings.window_height) + ",\n  \"recent_lists\": [";
  for (size_t index = 0; index < settings.recent_ibases.size(); ++index) {
    if (index) json += ", ";
    json += "{\"recent_list\": \"" + Escape(settings.recent_ibases[index].wstring()) + "\"}";
  }
  json += "],\n  \"platform_paths\": [";
  for (size_t index = 0; index < settings.platform_search_paths.size(); ++index) {
    if (index) json += ", ";
    json += "{\"platform_path\": \"" + Escape(settings.platform_search_paths[index].wstring()) + "\"}";
  }
  json += "]\n}\n";
  WriteAtomically(PathFor(layout, L"settings.json"), json);
}

CatalogState LoadCatalogState(const StorageLayout& layout) {
  CatalogState result;
  const auto json = ReadFile(PathFor(layout, L"catalog-state.json"));
  const std::regex favorite("\\\"favorite\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  for (std::sregex_iterator it(json.begin(), json.end(), favorite), end; it != end; ++it) {
    if (const auto value = TryUnescape((*it)[1].str())) result.favorites.push_back(*value);
  }

  const std::regex history("\\{\\s*\\\"history_id\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"\\s*,\\s*\\\"time\\\"\\s*:\\s*([0-9]+)\\s*,\\s*\\\"mode\\\"\\s*:\\s*([0-9]+)\\s*\\}");
  for (std::sregex_iterator it(json.begin(), json.end(), history), end; it != end; ++it) {
    try {
      const int mode = std::stoi((*it)[3].str());
      const auto id = TryUnescape((*it)[1].str());
      if (id && mode >= static_cast<int>(domain::LaunchMode::enterprise) && mode <= static_cast<int>(domain::LaunchMode::web_client)) {
        result.history.push_back({*id, std::chrono::system_clock::from_time_t(std::stoll((*it)[2].str())), static_cast<domain::LaunchMode>(mode)});
      }
    } catch (const std::exception&) {
    }
  }

  const std::regex launch("\\{\\s*\\\"last_launch_id\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"\\s*,\\s*\\\"time\\\"\\s*:\\s*([0-9]+)\\s*\\}");
  for (std::sregex_iterator it(json.begin(), json.end(), launch), end; it != end; ++it) {
    try {
      const auto id = TryUnescape((*it)[1].str());
      if (id && !id->empty()) result.last_launches[*id] = std::chrono::system_clock::from_time_t(std::stoll((*it)[2].str()));
    } catch (const std::exception&) {
    }
  }

  size_t position = 0;
  while (position < json.size()) {
    while (position < json.size() && json[position] != '{') ++position;
    if (position == json.size()) break;
    ++position;
    const auto idKey = ReadJsonRawString(json, position);
    if (!idKey || *idKey != "tag_id" || !ConsumeJsonCharacter(json, position, ':')) continue;
    const auto rawId = ReadJsonRawString(json, position);
    if (!rawId || !ConsumeJsonCharacter(json, position, ',')) continue;
    const auto valuesKey = ReadJsonRawString(json, position);
    if (!valuesKey || *valuesKey != "values" || !ConsumeJsonCharacter(json, position, ':') || !ConsumeJsonCharacter(json, position, '[')) continue;
    std::vector<std::wstring> values;
    for (;;) {
      SkipJsonWhitespace(json, position);
      if (position < json.size() && json[position] == ']') { ++position; break; }
      const auto rawTag = ReadJsonRawString(json, position);
      const auto tag = rawTag ? TryUnescape(*rawTag) : std::nullopt;
      if (!tag) { values.clear(); break; }
      values.push_back(*tag);
      SkipJsonWhitespace(json, position);
      if (position < json.size() && json[position] == ',') { ++position; continue; }
      if (position < json.size() && json[position] == ']') { ++position; break; }
      values.clear();
      break;
    }
    if (!ConsumeJsonCharacter(json, position, '}')) continue;
    const auto id = TryUnescape(*rawId);
    if (id && !id->empty() && !values.empty()) result.tags[*id] = std::move(values);
  }

  const std::regex style("\\{\\s*\\\"tag_style\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"\\s*,\\s*\\\"background\\\"\\s*:\\s*([0-9]+)\\s*,\\s*\\\"text\\\"\\s*:\\s*([0-9]+)\\s*\\}");
  for (std::sregex_iterator it(json.begin(), json.end(), style), end; it != end; ++it) {
    try {
      const auto tag = TryUnescape((*it)[1].str());
      const auto background = std::stoul((*it)[2].str());
      const auto text = std::stoul((*it)[3].str());
      if (tag && !tag->empty() && background <= 0xFFFFFFu && text <= 0xFFFFFFu) result.tag_styles[*tag] = {static_cast<COLORREF>(background), static_cast<COLORREF>(text)};
    } catch (const std::exception&) {
    }
  }

  for (const auto& history : result.history) if (!history.database_id.empty() && !result.last_launches.contains(history.database_id)) result.last_launches[history.database_id] = history.timestamp;
  return result;
}

void SaveCatalogState(const StorageLayout& layout, const CatalogState& state) {
  std::string json = "{\n  \"schema_version\": 1,\n  \"favorites\": [";
  for (size_t index = 0; index < state.favorites.size(); ++index) {
    if (index) json += ", ";
    json += "{\"favorite\": \"" + Escape(state.favorites[index]) + "\"}";
  }
  json += "],\n  \"history\": [";
  for (size_t index = 0; index < state.history.size(); ++index) {
    const auto& record = state.history[index];
    if (index) json += ", ";
    json += "{\"history_id\": \"" + Escape(record.database_id) + "\", \"time\": " + std::to_string(std::chrono::system_clock::to_time_t(record.timestamp)) + ", \"mode\": " + std::to_string(static_cast<int>(record.mode)) + "}";
  }
  json += "],\n  \"last_launches\": [";
  size_t written = 0;
  for (const auto& [id, timestamp] : state.last_launches) {
    if (id.empty()) continue;
    if (written++) json += ", ";
    json += "{\"last_launch_id\": \"" + Escape(id) + "\", \"time\": " + std::to_string(std::chrono::system_clock::to_time_t(timestamp)) + "}";
  }
  json += "],\n  \"tags\": [";
  written = 0;
  for (const auto& [id, values] : state.tags) {
    if (id.empty() || values.empty()) continue;
    if (written++) json += ", ";
    json += "{\"tag_id\": \"" + Escape(id) + "\", \"values\": [";
    for (size_t index = 0; index < values.size(); ++index) {
      if (index) json += ", ";
      json += "\"" + Escape(values[index]) + "\"";
    }
    json += "]}";
  }
  json += "],\n  \"tag_styles\": [";
  written = 0;
  for (const auto& [tag, style] : state.tag_styles) {
    if (tag.empty()) continue;
    if (written++) json += ", ";
    json += "{\"tag_style\": \"" + Escape(tag) + "\", \"background\": " + std::to_string(style.background) + ", \"text\": " + std::to_string(style.text) + "}";
  }
  json += "]\n}\n";
  WriteAtomically(PathFor(layout, L"catalog-state.json"), json);
}

std::vector<domain::HistoryItem> LoadHistory(const StorageLayout& layout) { return LoadCatalogState(layout).history; }

void AppendHistory(const StorageLayout& layout, domain::HistoryItem item) {
  auto state = LoadCatalogState(layout);
  state.history.erase(std::remove_if(state.history.begin(), state.history.end(), [&](const auto& existing) { return existing.database_id == item.database_id; }), state.history.end());
  const auto id = item.database_id;
  const auto timestamp = item.timestamp;
  state.history.insert(state.history.begin(), std::move(item));
  if (state.history.size() > 20) state.history.resize(20);
  if (!id.empty()) state.last_launches[id] = timestamp;
  SaveCatalogState(layout, state);
}

void ClearHistory(const StorageLayout& layout) { auto state = LoadCatalogState(layout); state.history.clear(); SaveCatalogState(layout, state); }
LastLaunchTimes LoadLastLaunchTimes(const StorageLayout& layout) { return LoadCatalogState(layout).last_launches; }
std::vector<std::wstring> LoadFavorites(const StorageLayout& layout) { return LoadCatalogState(layout).favorites; }
void SaveFavorites(const StorageLayout& layout, const std::vector<std::wstring>& favorites) { auto state = LoadCatalogState(layout); state.favorites = favorites; SaveCatalogState(layout, state); }
DatabaseTags LoadTags(const StorageLayout& layout) { return LoadCatalogState(layout).tags; }
void SaveTags(const StorageLayout& layout, const DatabaseTags& tags) { auto state = LoadCatalogState(layout); state.tags = tags; SaveCatalogState(layout, state); }
TagStyles LoadTagStyles(const StorageLayout& layout) { return LoadCatalogState(layout).tag_styles; }
void SaveTagStyles(const StorageLayout& layout, const TagStyles& styles) { auto state = LoadCatalogState(layout); state.tag_styles = styles; SaveCatalogState(layout, state); }
void SaveTagsAndStyles(const StorageLayout& layout, const DatabaseTags& tags, const TagStyles& styles) {
  auto state = LoadCatalogState(layout);
  state.tags = tags;
  state.tag_styles = styles;
  SaveCatalogState(layout, state);
}

}  // namespace ibstart::storage
