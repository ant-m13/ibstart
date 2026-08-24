#pragma once

#include "core/catalog/catalog.hpp"
#include "core/storage/storage.hpp"

#include <CommCtrl.h>

#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui::presentation {

enum class TreeTagFilterKind { all, favorites, tag };

struct TreeTagFilter {
  TreeTagFilterKind kind{TreeTagFilterKind::all};
  std::wstring tag;
};

[[nodiscard]] std::vector<std::wstring> ParseTags(std::wstring_view text);
[[nodiscard]] std::wstring TagsText(const std::vector<std::wstring>& tags);
[[nodiscard]] std::wstring TagId(const domain::Entry& entry);
[[nodiscard]] const std::vector<std::wstring>& TagsFor(const storage::DatabaseTags& tags, const domain::Entry& entry);
[[nodiscard]] const storage::TagStyle* TagStyleFor(const storage::TagStyles& styles, std::wstring_view tag);
[[nodiscard]] std::vector<std::wstring> KnownTags(const storage::DatabaseTags& tags, const storage::TagStyles& styles);
void EraseTagStyle(storage::TagStyles& styles, std::wstring_view name);
[[nodiscard]] bool ContainsTag(const std::vector<std::wstring>& tags, std::wstring_view value);
[[nodiscard]] std::vector<std::wstring> CollectFilterTags(const catalog::Catalog& catalog, const storage::DatabaseTags& tags);
[[nodiscard]] bool MatchesSearchFilter(const catalog::Catalog& catalog, const catalog::TreeItem& item,
    std::wstring_view search_filter, const storage::DatabaseTags& tags);
[[nodiscard]] bool MatchesTagFilter(const catalog::Catalog& catalog, const catalog::TreeItem& item,
    const TreeTagFilter& filter, const storage::DatabaseTags& tags, const std::vector<std::wstring>& favorites);

[[nodiscard]] LRESULT DrawTreeSearchMatches(HWND tree, NMTVCUSTOMDRAW* draw, const catalog::Catalog* catalog,
    const storage::Settings& settings, const storage::DatabaseTags& tags, const storage::TagStyles& styles,
    std::wstring_view search_filter, HFONT controls_font);

}  // namespace ibstart::ui::presentation
