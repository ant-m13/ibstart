#pragma once

#include "core/domain/model.hpp"

#include <Windows.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ibstart::storage {

struct StorageLayout {
  std::filesystem::path root;
  bool portable{false};
};

struct Settings {
  std::filesystem::path active_ibases;
  std::wstring selected_entry;
  bool simple_mode{false};
  bool show_tags_in_list{true};
  bool folders_first_when_sorting{true};
  std::vector<std::filesystem::path> recent_ibases;
  std::vector<std::filesystem::path> platform_search_paths;
  int window_x{CW_USEDEFAULT};
  int window_y{CW_USEDEFAULT};
  int window_width{900};
  int window_height{560};
};

using DatabaseTags = std::map<std::wstring, std::vector<std::wstring>>;

struct TagStyle {
  COLORREF background{RGB(226, 242, 244)};
  COLORREF text{RGB(0, 86, 102)};

  bool operator==(const TagStyle&) const = default;
};

using TagStyles = std::map<std::wstring, TagStyle>;

using LastLaunchTimes = std::map<std::wstring, std::chrono::system_clock::time_point>;

struct CatalogState {
  std::vector<std::wstring> favorites;
  std::vector<domain::HistoryItem> history;
  LastLaunchTimes last_launches;
  DatabaseTags tags;
  TagStyles tag_styles;
};

[[nodiscard]] StorageLayout ResolveLayout(const std::filesystem::path& executable_path);
[[nodiscard]] std::optional<std::filesystem::path> FindStandardIbases();
void EnsureWritable(const StorageLayout& layout);
[[nodiscard]] Settings LoadSettings(const StorageLayout& layout);
void SaveSettings(const StorageLayout& layout, const Settings& settings);
[[nodiscard]] CatalogState LoadCatalogState(const StorageLayout& layout);
void SaveCatalogState(const StorageLayout& layout, const CatalogState& state);
[[nodiscard]] std::vector<domain::HistoryItem> LoadHistory(const StorageLayout& layout);
void AppendHistory(const StorageLayout& layout, domain::HistoryItem item);
void ClearHistory(const StorageLayout& layout);
[[nodiscard]] LastLaunchTimes LoadLastLaunchTimes(const StorageLayout& layout);
[[nodiscard]] std::vector<std::wstring> LoadFavorites(const StorageLayout& layout);
void SaveFavorites(const StorageLayout& layout, const std::vector<std::wstring>& favorites);
[[nodiscard]] DatabaseTags LoadTags(const StorageLayout& layout);
void SaveTags(const StorageLayout& layout, const DatabaseTags& tags);
[[nodiscard]] TagStyles LoadTagStyles(const StorageLayout& layout);
void SaveTagStyles(const StorageLayout& layout, const TagStyles& styles);
void SaveTagsAndStyles(const StorageLayout& layout, const DatabaseTags& tags, const TagStyles& styles);

}  // namespace ibstart::storage
