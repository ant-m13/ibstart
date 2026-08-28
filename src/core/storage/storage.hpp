#pragma once

#include "core/domain/identifier.hpp"
#include "core/domain/model.hpp"

#include <Windows.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ibstart::storage {

inline constexpr std::size_t kMaxFavorites = 9;
inline constexpr std::size_t kMaxHistory = 20;

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

using DatabaseTags = std::map<std::wstring, std::vector<std::wstring>, domain::IdentifierLess>;

struct TagStyle {
  COLORREF background{RGB(226, 242, 244)};
  COLORREF text{RGB(0, 86, 102)};

  bool operator==(const TagStyle&) const = default;
};

using TagStyles = std::map<std::wstring, TagStyle>;

using LastLaunchTimes = std::map<std::wstring, std::chrono::system_clock::time_point, domain::IdentifierLess>;

struct CatalogState {
  std::vector<std::wstring> favorites;
  std::vector<domain::HistoryItem> history;
  LastLaunchTimes last_launches;
  DatabaseTags tags;
  TagStyles tag_styles;
};

class CatalogStateRepository {
 public:
  explicit CatalogStateRepository(StorageLayout layout);

  [[nodiscard]] const CatalogState& Read();
  [[nodiscard]] const CatalogState& Reload();
  void Update(const std::function<void(CatalogState&)>& mutation);
  void AppendHistory(domain::HistoryItem item);
  void ClearHistory();

 private:
  StorageLayout layout_;
  std::optional<CatalogState> state_;
};

[[nodiscard]] StorageLayout ResolveLayout(const std::filesystem::path& executable_path);
[[nodiscard]] std::optional<std::filesystem::path> FindStandardIbases();
void EnsureWritable(const StorageLayout& layout);
[[nodiscard]] Settings LoadSettings(const StorageLayout& layout);
void SaveSettings(const StorageLayout& layout, const Settings& settings);
[[nodiscard]] CatalogState LoadCatalogState(const StorageLayout& layout);
// Removes invalid and duplicate favorite/history entries and applies their size limits.
void NormalizeCatalogState(CatalogState& state);
void SaveCatalogState(const StorageLayout& layout, const CatalogState& state);

}  // namespace ibstart::storage
