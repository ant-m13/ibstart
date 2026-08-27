#include "core/catalog/catalog_metadata_service.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <utility>

namespace ibstart::catalog {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && utf::FindNoCaseOrdinal(left, right) == 0;
}

}  // namespace

CatalogMetadataService::CatalogMetadataService(storage::StorageLayout layout) : repository_(std::move(layout)) {}

const storage::CatalogState& CatalogMetadataService::Read() const { return repository_.Read(); }

const storage::CatalogState& CatalogMetadataService::Reload() { return repository_.Reload(); }

bool CatalogMetadataService::ToggleFavorite(std::wstring database_id, std::wstring legacy_database_name) {
  if (database_id.empty()) return false;
  const auto& favorites = Read().favorites;
  const auto existing_id = std::find(favorites.begin(), favorites.end(), database_id);
  const auto existing_legacy = existing_id == favorites.end() && !legacy_database_name.empty() ?
      std::find_if(favorites.begin(), favorites.end(), [&](const auto& value) { return EqualNoCase(value, legacy_database_name); }) : favorites.end();
  const bool added = existing_id == favorites.end() && existing_legacy == favorites.end();
  repository_.Update([&](storage::CatalogState& state) {
    auto favorite = std::find(state.favorites.begin(), state.favorites.end(), database_id);
    if (favorite == state.favorites.end() && !legacy_database_name.empty()) {
      favorite = std::find_if(state.favorites.begin(), state.favorites.end(), [&](const auto& value) {
        return EqualNoCase(value, legacy_database_name);
      });
    }
    if (favorite == state.favorites.end()) {
      state.favorites.insert(state.favorites.begin(), std::move(database_id));
      if (state.favorites.size() > kMaxFavorites) state.favorites.resize(kMaxFavorites);
    } else {
      state.favorites.erase(favorite);
    }
  });
  return added;
}

void CatalogMetadataService::RenameDatabaseMetadata(std::wstring previous_name, std::wstring updated_name,
    std::wstring previous_tag_id, std::wstring updated_tag_id) {
  if (previous_name == updated_name && previous_tag_id == updated_tag_id) return;
  repository_.Update([&](storage::CatalogState& state) {
    if (previous_name != updated_name || previous_tag_id != updated_tag_id) {
      for (auto& favorite : state.favorites) {
        if (EqualNoCase(favorite, previous_name) || EqualNoCase(favorite, previous_tag_id)) favorite = updated_tag_id;
      }
    }
    if (previous_tag_id != updated_tag_id) {
      const auto existing = state.tags.find(previous_tag_id);
      if (existing != state.tags.end()) {
        auto tags = std::move(existing->second);
        state.tags.erase(existing);
        state.tags[updated_tag_id] = std::move(tags);
      }
    }
  });
}

void CatalogMetadataService::SetTags(std::wstring database_id, std::vector<std::wstring> tags) {
  if (database_id.empty()) return;
  repository_.Update([&](storage::CatalogState& state) {
    if (tags.empty()) state.tags.erase(database_id);
    else state.tags[database_id] = std::move(tags);
  });
}

bool CatalogMetadataService::AddTag(std::wstring database_id, std::wstring tag) {
  if (database_id.empty() || tag.empty()) return false;
  const auto& savedTags = Read().tags;
  if (const auto assigned = savedTags.find(database_id); assigned != savedTags.end() &&
      std::any_of(assigned->second.begin(), assigned->second.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) {
    return false;
  }
  repository_.Update([&](storage::CatalogState& state) { state.tags[database_id].push_back(std::move(tag)); });
  return true;
}

bool CatalogMetadataService::RemoveTags(std::wstring_view database_id) {
  if (database_id.empty()) return false;
  const std::wstring id(database_id);
  if (!Read().tags.contains(id)) return false;
  repository_.Update([&](storage::CatalogState& state) { state.tags.erase(id); });
  return true;
}

void CatalogMetadataService::ReplaceTagConfiguration(storage::DatabaseTags tags, storage::TagStyles styles) {
  repository_.Update([&](storage::CatalogState& state) {
    state.tags = std::move(tags);
    state.tag_styles = std::move(styles);
  });
}

void CatalogMetadataService::RecordLaunch(domain::HistoryItem item) { repository_.AppendHistory(std::move(item)); }

void CatalogMetadataService::ClearHistory() { repository_.ClearHistory(); }

}  // namespace ibstart::catalog
