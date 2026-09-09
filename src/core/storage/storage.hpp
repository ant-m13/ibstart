#pragma once

#include "core/domain/identifier.hpp"
#include "core/domain/model.hpp"
#include "core/credentials/credentials.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace ibstart::storage {

inline constexpr std::size_t kMaxFavorites = 9;
inline constexpr std::size_t kMaxHistory = 20;
// Settings and catalog state are parsed from one in-memory JSON document.
// Reject anomalously large files before allocating their contents.
inline constexpr std::uintmax_t kMaxStorageFileSize = 4ULL * 1024ULL * 1024ULL;

struct StorageLayout {
  std::filesystem::path root;
  bool portable{false};
};

struct StorageFingerprint {
  std::uintmax_t size{};
  std::filesystem::file_time_type write_time{};
  std::uint64_t content_hash{};

  bool operator==(const StorageFingerprint&) const = default;
};

class StorageConflictError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Returns the system-wide mutex name used to serialize access to this profile.
// The name is derived from the normalized storage path rather than from the
// executable, so all copies of a portable profile coordinate with one another.
[[nodiscard]] std::wstring StorageMutexName(const StorageLayout& layout);
// Returns the system-wide mutex name used to enforce one visible IBStart
// instance per profile. It intentionally uses a separate object from the
// transaction mutex so a short external storage operation cannot look like a
// running application to the instance-forwarding code.
[[nodiscard]] std::wstring InstanceMutexName(const StorageLayout& layout);
[[nodiscard]] std::uint64_t InstanceIdentity(const StorageLayout& layout);

struct Settings {
  std::filesystem::path active_ibases;
  std::wstring selected_entry;
  bool simple_mode{false};
  bool show_tags_in_list{true};
  bool folders_first_when_sorting{true};
  bool open_last_list_on_startup{true};
  bool restore_last_selection{true};
  domain::ClientType default_client_type{domain::ClientType::automatic};
  domain::ClientArchitecture default_architecture{domain::ClientArchitecture::automatic};
  std::wstring default_platform_version;
  bool confirm_destructive_actions{true};
  bool show_details_panel{true};
  bool show_status_bar{true};
  int tree_density{1};
  bool remember_launch_history{true};
  int launch_history_limit{20};
  int recent_lists_limit{9};
  std::vector<std::filesystem::path> recent_ibases;
  std::vector<std::filesystem::path> platform_search_paths;
  int window_x{CW_USEDEFAULT};
  int window_y{CW_USEDEFAULT};
  int window_width{900};
  int window_height{560};
  std::vector<credentials::Credential> credentials;
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

class SettingsRepository {
 public:
  explicit SettingsRepository(StorageLayout layout);

  [[nodiscard]] const Settings& Read();
  [[nodiscard]] const Settings& Reload();
  void Update(const std::function<void(Settings&)>& mutation);
  // Saves the requested values while preserving settings changed by another
  // process since this repository last read the file.
  void Save(const Settings& settings);

 private:
  StorageLayout layout_;
  std::optional<Settings> settings_;
  std::optional<StorageFingerprint> fingerprint_;
};

class CatalogStateRepository {
 public:
  explicit CatalogStateRepository(StorageLayout layout);

  [[nodiscard]] const CatalogState& Read();
  [[nodiscard]] const CatalogState& Reload();
  void Adopt(CatalogState state);
  void Update(const std::function<void(CatalogState&)>& mutation);
  void AppendHistory(domain::HistoryItem item, std::size_t max_history = kMaxHistory);
  void RemoveHistory(std::wstring_view database_id);
  void ClearHistory();

 private:
  StorageLayout layout_;
  std::optional<CatalogState> state_;
};

[[nodiscard]] StorageLayout ResolveLayout(const std::filesystem::path& executable_path);
[[nodiscard]] std::optional<std::filesystem::path> FindStandardIbases();
void EnsureWritable(const StorageLayout& layout);
[[nodiscard]] bool HasCompleteProfileFiles(const std::filesystem::path& root);
[[nodiscard]] bool IsSameProfilePath(const std::filesystem::path& left, const std::filesystem::path& right);
[[nodiscard]] Settings LoadSettings(const StorageLayout& layout);
void SaveSettings(const StorageLayout& layout, const Settings& settings);
// Copies the profile's own JSON state to a user-selected directory. Credentials
// are omitted unless the caller explicitly opts in.
void ExportProfile(const StorageLayout& source, const std::filesystem::path& target, bool include_credentials);
// Imports profile JSON state. Existing credentials are preserved unless the
// caller explicitly opts in to replacing them with the imported records.
void ImportProfile(const std::filesystem::path& source, const StorageLayout& target, bool include_credentials);
[[nodiscard]] CatalogState LoadCatalogState(const StorageLayout& layout);
// Removes invalid and duplicate favorite/history entries and applies their size limits.
void NormalizeCatalogState(CatalogState& state);
void SaveCatalogState(const StorageLayout& layout, const CatalogState& state);

}  // namespace ibstart::storage
