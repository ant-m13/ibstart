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
  bool simple_mode{false};
  std::vector<std::filesystem::path> platform_search_paths;
  int window_x{CW_USEDEFAULT};
  int window_y{CW_USEDEFAULT};
  int window_width{900};
  int window_height{560};
};

using DatabaseTags = std::map<std::wstring, std::vector<std::wstring>>;

[[nodiscard]] StorageLayout ResolveLayout(const std::filesystem::path& executable_path);
[[nodiscard]] std::optional<std::filesystem::path> FindStandardIbases();
void EnsureWritable(const StorageLayout& layout);
[[nodiscard]] Settings LoadSettings(const StorageLayout& layout);
void SaveSettings(const StorageLayout& layout, const Settings& settings);
[[nodiscard]] std::vector<domain::HistoryItem> LoadHistory(const StorageLayout& layout);
void AppendHistory(const StorageLayout& layout, domain::HistoryItem item);
void ClearHistory(const StorageLayout& layout);
[[nodiscard]] std::vector<std::wstring> LoadFavorites(const StorageLayout& layout);
void SaveFavorites(const StorageLayout& layout, const std::vector<std::wstring>& favorites);
[[nodiscard]] DatabaseTags LoadTags(const StorageLayout& layout);
void SaveTags(const StorageLayout& layout, const DatabaseTags& tags);

}  // namespace ibstart::storage
