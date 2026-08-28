#include "core/catalog/catalog_metadata_service.hpp"

#include "core/domain/identifier.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace ibstart::catalog {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return domain::EqualIdentifier(left, right);
}

void RemoveDuplicateFavorites(std::vector<std::wstring>& favorites) {
  std::vector<std::wstring> unique;
  unique.reserve(favorites.size());
  for (auto& favorite : favorites) {
    if (std::any_of(unique.begin(), unique.end(), [&](const auto& existing) {
          return EqualNoCase(existing, favorite);
        })) {
      continue;
    }
    unique.push_back(std::move(favorite));
  }
  favorites = std::move(unique);
}

void MergeTagAssignments(storage::DatabaseTags& tags, std::wstring_view previous_id,
    std::wstring_view updated_id) {
  if (previous_id.empty() || updated_id.empty() || EqualNoCase(previous_id, updated_id)) return;
  const auto previous = tags.find(std::wstring(previous_id));
  if (previous == tags.end()) return;

  const auto updated = tags.find(std::wstring(updated_id));
  if (updated == tags.end()) {
    auto values = std::move(previous->second);
    tags.erase(previous);
    tags.emplace(std::wstring(updated_id), std::move(values));
    return;
  }

  for (const auto& value : previous->second) {
    if (std::none_of(updated->second.begin(), updated->second.end(), [&](const auto& existing) {
          return EqualNoCase(existing, value);
        })) {
      updated->second.push_back(value);
    }
  }
  tags.erase(previous);
}

void MergeHistory(storage::CatalogState& state, std::wstring_view previous_id,
    std::wstring_view updated_id) {
  if (previous_id.empty() || updated_id.empty() || EqualNoCase(previous_id, updated_id)) return;

  std::optional<std::size_t> first_match;
  std::optional<domain::HistoryItem> latest;
  for (std::size_t index = 0; index < state.history.size(); ++index) {
    const auto& item = state.history[index];
    if (!EqualNoCase(item.database_id, previous_id) && !EqualNoCase(item.database_id, updated_id)) continue;
    if (!first_match) first_match = index;

    auto candidate = item;
    candidate.database_id = updated_id;
    if (!latest || candidate.timestamp > latest->timestamp) latest = std::move(candidate);
  }
  if (first_match && latest) {
    std::vector<domain::HistoryItem> merged;
    merged.reserve(state.history.size());
    for (std::size_t index = 0; index < state.history.size(); ++index) {
      const auto& item = state.history[index];
      if (!EqualNoCase(item.database_id, previous_id) && !EqualNoCase(item.database_id, updated_id)) {
        merged.push_back(item);
      } else if (index == *first_match) {
        merged.push_back(*latest);
      }
    }
    state.history = std::move(merged);
  }
}

void MergeLastLaunch(storage::LastLaunchTimes& last_launches, std::wstring_view previous_id,
    std::wstring_view updated_id) {
  if (previous_id.empty() || updated_id.empty() || EqualNoCase(previous_id, updated_id)) return;
  const auto previous = last_launches.find(std::wstring(previous_id));
  if (previous == last_launches.end()) return;

  const auto updated = last_launches.find(std::wstring(updated_id));
  if (updated == last_launches.end()) {
    last_launches.emplace(std::wstring(updated_id), previous->second);
  } else if (updated->second < previous->second) {
    updated->second = previous->second;
  }
  last_launches.erase(previous);
}

}  // namespace

CatalogMetadataService::CatalogMetadataService(storage::StorageLayout layout) : repository_(std::move(layout)) {}

const storage::CatalogState& CatalogMetadataService::Read() const { return repository_.Read(); }

const storage::CatalogState& CatalogMetadataService::Reload() { return repository_.Reload(); }

bool CatalogMetadataService::ToggleFavorite(std::wstring database_id, std::wstring legacy_database_name) {
  if (database_id.empty()) return false;
  bool added = false;
  repository_.Update([&](storage::CatalogState& state) {
    auto favorite = std::find_if(state.favorites.begin(), state.favorites.end(), [&](const auto& value) {
      return EqualNoCase(value, database_id);
    });
    if (favorite == state.favorites.end() && !legacy_database_name.empty()) {
      favorite = std::find_if(state.favorites.begin(), state.favorites.end(), [&](const auto& value) {
        return EqualNoCase(value, legacy_database_name);
      });
    }
    if (favorite == state.favorites.end()) {
      added = true;
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
  if (previous_name == updated_name && EqualNoCase(previous_tag_id, updated_tag_id)) return;
  repository_.Update([&](storage::CatalogState& state) {
    if (previous_name != updated_name || !EqualNoCase(previous_tag_id, updated_tag_id)) {
      for (auto& favorite : state.favorites) {
        if (EqualNoCase(favorite, previous_name) || EqualNoCase(favorite, previous_tag_id)) favorite = updated_tag_id;
      }
      RemoveDuplicateFavorites(state.favorites);
      if (state.favorites.size() > kMaxFavorites) state.favorites.resize(kMaxFavorites);
    }
    if (!EqualNoCase(previous_tag_id, updated_tag_id)) {
      MergeTagAssignments(state.tags, previous_tag_id, updated_tag_id);
    }
    // A database without an explicit ID uses its name as the metadata key. A
    // name change therefore changes the key, while an explicit ID remains
    // stable. Do not migrate history for an explicit ID change: the user guide
    // documents that manual ID changes require deliberate metadata handling.
    if (previous_name != updated_name && EqualNoCase(previous_tag_id, previous_name)) {
      MergeHistory(state, previous_tag_id, updated_tag_id);
      MergeLastLaunch(state.last_launches, previous_tag_id, updated_tag_id);
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
  bool added = false;
  repository_.Update([&](storage::CatalogState& state) {
    if (const auto assigned = state.tags.find(database_id); assigned != state.tags.end() &&
        std::any_of(assigned->second.begin(), assigned->second.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) {
      return;
    }
    state.tags[database_id].push_back(std::move(tag));
    added = true;
  });
  return added;
}

bool CatalogMetadataService::RemoveTags(std::wstring_view database_id) {
  if (database_id.empty()) return false;
  const std::wstring id(database_id);
  bool removed = false;
  repository_.Update([&](storage::CatalogState& state) { removed = state.tags.erase(id) != 0; });
  return removed;
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
