#pragma once

#include "core/storage/storage.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::catalog {

// Owns user metadata associated with catalog entries.  The service keeps
// catalog-state updates transactional, leaving UI code responsible only for
// selecting an entry and refreshing its controls after a successful change.
class CatalogMetadataService {
 public:
  static constexpr size_t kMaxFavorites = 9;

  explicit CatalogMetadataService(storage::StorageLayout layout);

  [[nodiscard]] const storage::CatalogState& Read() const;
  [[nodiscard]] const storage::CatalogState& Reload();
  [[nodiscard]] bool ToggleFavorite(std::wstring database_id, std::wstring legacy_database_name = {});
  void RenameDatabaseMetadata(std::wstring previous_name, std::wstring updated_name,
      std::wstring previous_tag_id, std::wstring updated_tag_id);
  void SetTags(std::wstring database_id, std::vector<std::wstring> tags);
  [[nodiscard]] bool AddTag(std::wstring database_id, std::wstring tag);
  [[nodiscard]] bool RemoveTags(std::wstring_view database_id);
  void ReplaceTagConfiguration(storage::DatabaseTags tags, storage::TagStyles styles);
  void RecordLaunch(domain::HistoryItem item);
  void ClearHistory();

 private:
  mutable storage::CatalogStateRepository repository_;
};

}  // namespace ibstart::catalog
