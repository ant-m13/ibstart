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
  GetEnvironmentVariableW(std::wstring(name).c_str(), result.data(), size);
  result.resize(size - 1);
  return result;
}

std::filesystem::path PathFor(const StorageLayout& layout, std::wstring_view name) { return layout.root / std::wstring(name); }

std::string Escape(std::wstring_view value) {
  std::string result;
  for (const wchar_t unit : value) {
    if (unit == L'\\') result += "\\\\";
    else if (unit == L'"') result += "\\\"";
    else if (unit == L'\n') result += "\\n";
    else if (unit == L'\r') result += "\\r";
    else result += utf::ToUtf8(std::wstring_view(&unit, 1));
  }
  return result;
}

std::wstring Unescape(std::string_view value) {
  std::string raw;
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\' && index + 1 < value.size()) {
      const char next = value[++index];
      raw.push_back(next == 'n' ? '\n' : next == 'r' ? '\r' : next);
    } else raw.push_back(value[index]);
  }
  return utf::FromUtf8(raw);
}

void WriteAtomically(const std::filesystem::path& path, std::string_view contents) {
  const auto temporary = path.wstring() + L".tmp";
  {
    std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot write application data.");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("Cannot write application data.");
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("Cannot save application data: " + utf::ToUtf8(utf::LastErrorMessage()));
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return input ? std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()) : std::string{};
}

std::optional<std::wstring> JsonString(std::string_view json, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  std::smatch match;
  const std::string body(json);
  if (!std::regex_search(body, match, expression)) return std::nullopt;
  return Unescape(match[1].str());
}

std::optional<int> JsonInteger(std::string_view json, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*(-?[0-9]+)");
  std::smatch match;
  const std::string body(json);
  if (!std::regex_search(body, match, expression)) return std::nullopt;
  return std::stoi(match[1].str());
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
  const auto probe = layout.root / L".write_probe";
  std::ofstream output(probe, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("IBStart data directory is not writable: " + utf::ToUtf8(layout.root.wstring()));
  output.close();
  std::filesystem::remove(probe, error);
}

Settings LoadSettings(const StorageLayout& layout) {
  Settings result;
  const auto json = ReadFile(PathFor(layout, L"settings.json"));
  if (const auto active = JsonString(json, "active_ibases")) result.active_ibases = *active;
  if (const auto simple = JsonInteger(json, "simple_mode")) result.simple_mode = *simple != 0;
  if (const auto x = JsonInteger(json, "window_x")) result.window_x = *x;
  if (const auto y = JsonInteger(json, "window_y")) result.window_y = *y;
  if (const auto width = JsonInteger(json, "window_width")) result.window_width = std::max(*width, 480);
  if (const auto height = JsonInteger(json, "window_height")) result.window_height = std::max(*height, 320);
  const std::regex pathExpression("\\\"platform_path\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  for (std::sregex_iterator it(json.begin(), json.end(), pathExpression), end; it != end; ++it) result.platform_search_paths.emplace_back(Unescape((*it)[1].str()));
  return result;
}

void SaveSettings(const StorageLayout& layout, const Settings& settings) {
  std::string json = "{\n  \"active_ibases\": \"" + Escape(settings.active_ibases.wstring()) + "\",\n";
  json += "  \"simple_mode\": " + std::string(settings.simple_mode ? "1" : "0") + ",\n";
  json += "  \"window_x\": " + std::to_string(settings.window_x) + ",\n  \"window_y\": " + std::to_string(settings.window_y);
  json += ",\n  \"window_width\": " + std::to_string(settings.window_width) + ",\n  \"window_height\": " + std::to_string(settings.window_height) + ",\n  \"platform_paths\": [";
  for (size_t index = 0; index < settings.platform_search_paths.size(); ++index) {
    if (index) json += ", ";
    json += "{\"platform_path\": \"" + Escape(settings.platform_search_paths[index].wstring()) + "\"}";
  }
  json += "]\n}\n";
  WriteAtomically(PathFor(layout, L"settings.json"), json);
}

std::vector<domain::HistoryItem> LoadHistory(const StorageLayout& layout) {
  std::vector<domain::HistoryItem> result;
  const std::regex item("\\{\\s*\\\"id\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"\\s*,\\s*\\\"time\\\"\\s*:\\s*([0-9]+)\\s*,\\s*\\\"mode\\\"\\s*:\\s*([0-9]+)\\s*\\}");
  const auto json = ReadFile(PathFor(layout, L"history.json"));
  for (std::sregex_iterator it(json.begin(), json.end(), item), end; it != end; ++it) {
    result.push_back({Unescape((*it)[1].str()), std::chrono::system_clock::from_time_t(std::stoll((*it)[2].str())), static_cast<domain::LaunchMode>(std::stoi((*it)[3].str()))});
  }
  return result;
}

void AppendHistory(const StorageLayout& layout, domain::HistoryItem item) {
  auto history = LoadHistory(layout);
  history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& existing) { return existing.database_id == item.database_id; }), history.end());
  history.insert(history.begin(), std::move(item));
  if (history.size() > 20) history.resize(20);
  std::string json = "[\n";
  for (size_t index = 0; index < history.size(); ++index) {
    const auto& record = history[index];
    json += "  {\"id\": \"" + Escape(record.database_id) + "\", \"time\": " + std::to_string(std::chrono::system_clock::to_time_t(record.timestamp)) + ", \"mode\": " + std::to_string(static_cast<int>(record.mode)) + "}";
    json += index + 1 == history.size() ? "\n" : ",\n";
  }
  json += "]\n";
  WriteAtomically(PathFor(layout, L"history.json"), json);
}

std::vector<std::wstring> LoadFavorites(const StorageLayout& layout) {
  std::vector<std::wstring> result;
  const std::regex item("\\\"((?:\\\\.|[^\\\"])*)\\\"");
  const auto json = ReadFile(PathFor(layout, L"favorites.json"));
  for (std::sregex_iterator it(json.begin(), json.end(), item), end; it != end; ++it) result.push_back(Unescape((*it)[1].str()));
  return result;
}

void SaveFavorites(const StorageLayout& layout, const std::vector<std::wstring>& favorites) {
  std::string json = "[";
  for (size_t index = 0; index < favorites.size(); ++index) { if (index) json += ", "; json += "\"" + Escape(favorites[index]) + "\""; }
  json += "]\n";
  WriteAtomically(PathFor(layout, L"favorites.json"), json);
}

}  // namespace ibstart::storage
