#include "ui/tree_presentation.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace ibstart::ui::presentation {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TrimText(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

LPARAM TreeItemData(HWND tree, HTREEITEM item) {
  if (!tree || !item) return 0;
  TVITEMW data{};
  data.mask = TVIF_PARAM;
  data.hItem = item;
  return TreeView_GetItem(tree, &data) ? data.lParam : 0;
}

}  // namespace

std::vector<std::wstring> ParseTags(std::wstring_view text) {
  std::vector<std::wstring> result;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find_first_of(L",;\r\n", start);
    const auto tag = TrimText(text.substr(start, end == std::wstring_view::npos ? text.size() - start : end - start));
    if (!tag.empty() && std::none_of(result.begin(), result.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) result.push_back(tag);
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::wstring TagsText(const std::vector<std::wstring>& tags) {
  std::wstring result;
  for (const auto& tag : tags) {
    if (!result.empty()) result += L", ";
    result += tag;
  }
  return result;
}

std::wstring TagId(const domain::Entry& entry) { return entry.ValueOr(L"ID", entry.name); }

const std::vector<std::wstring>& TagsFor(const storage::DatabaseTags& tags, const domain::Entry& entry) {
  static const std::vector<std::wstring> empty;
  const auto found = tags.find(TagId(entry));
  return found == tags.end() ? empty : found->second;
}

const storage::TagStyle* TagStyleFor(const storage::TagStyles& styles, std::wstring_view tag) {
  if (const auto exact = styles.find(std::wstring(tag)); exact != styles.end()) return &exact->second;
  const auto found = std::find_if(styles.begin(), styles.end(), [&](const auto& item) { return EqualNoCase(item.first, tag); });
  return found == styles.end() ? nullptr : &found->second;
}

std::vector<std::wstring> KnownTags(const storage::DatabaseTags& tags, const storage::TagStyles& styles) {
  std::vector<std::wstring> result;
  const auto append = [&](std::wstring_view tag) {
    if (!tag.empty() && std::none_of(result.begin(), result.end(), [&](const auto& existing) { return EqualNoCase(existing, tag); })) result.emplace_back(tag);
  };
  for (const auto& [_, values] : tags) for (const auto& tag : values) append(tag);
  for (const auto& [tag, _] : styles) append(tag);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
  return result;
}

void EraseTagStyle(storage::TagStyles& styles, std::wstring_view name) {
  for (auto it = styles.begin(); it != styles.end();) {
    if (EqualNoCase(it->first, name)) it = styles.erase(it);
    else ++it;
  }
}

bool ContainsTag(const std::vector<std::wstring>& tags, std::wstring_view value) {
  return std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return EqualNoCase(tag, value); });
}

std::vector<std::wstring> CollectFilterTags(const catalog::Catalog& catalog, const storage::DatabaseTags& tags) {
  std::vector<std::wstring> result;
  for (const auto* entry : catalog.Databases()) {
    for (const auto& tag : TagsFor(tags, *entry)) {
      if (!ContainsTag(result, tag)) result.push_back(tag);
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
  return result;
}

std::vector<std::wstring> CollectRecentDatabaseNames(const catalog::Catalog& catalog,
    const std::vector<domain::HistoryItem>& history) {
  std::vector<std::wstring> result;
  result.reserve(history.size());
  const auto databases = catalog.Databases();
  for (const auto& launch : history) {
    for (const auto* entry : databases) {
      if (entry->ValueOr(L"ID", entry->name) != launch.database_id) continue;
      result.push_back(entry->name);
      break;
    }
  }
  return result;
}

std::vector<catalog::TreeItem> FilterTreeItems(const catalog::Catalog& catalog,
    const std::vector<catalog::TreeItem>& items, std::wstring_view search_filter,
    const TreeTagFilter& filter, const storage::DatabaseTags& tags,
    const std::vector<std::wstring>& favorites) {
  std::unordered_map<const catalog::TreeItem*, bool> search_cache;
  std::unordered_map<const catalog::TreeItem*, bool> tag_cache;
  const auto matches_search = [&](const auto& self, const catalog::TreeItem& item) -> bool {
    if (const auto found = search_cache.find(&item); found != search_cache.end()) return found->second;
    bool result = search_filter.empty();
    if (!result) {
      if (const auto* entry = catalog.Find(item.name)) {
        result = catalog::MatchesSearchText(*entry, search_filter);
        if (!result && entry->IsDatabase()) {
          const auto& entry_tags = TagsFor(tags, *entry);
          result = std::any_of(entry_tags.begin(), entry_tags.end(), [&](const auto& tag) {
            return utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
          });
        }
      }
    }
    if (!result) {
      result = std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) {
        return self(self, child);
      });
    }
    search_cache.emplace(&item, result);
    return result;
  };
  const auto matches_tag = [&](const auto& self, const catalog::TreeItem& item) -> bool {
    if (const auto found = tag_cache.find(&item); found != tag_cache.end()) return found->second;
    bool result = filter.kind == TreeTagFilterKind::all;
    if (!result) {
      if (const auto* entry = catalog.Find(item.name); entry && entry->IsDatabase()) {
        if (filter.kind == TreeTagFilterKind::favorites) result = ContainsTag(favorites, entry->name);
        else result = ContainsTag(TagsFor(tags, *entry), filter.tag);
      }
    }
    if (!result) {
      result = std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) {
        return self(self, child);
      });
    }
    tag_cache.emplace(&item, result);
    return result;
  };
  const auto build = [&](const auto& self, const std::vector<catalog::TreeItem>& source) -> std::vector<catalog::TreeItem> {
    std::vector<catalog::TreeItem> result;
    result.reserve(source.size());
    for (const auto& item : source) {
      if (!matches_search(matches_search, item) || !matches_tag(matches_tag, item)) continue;
      auto copy = item;
      copy.children = self(self, item.children);
      result.push_back(std::move(copy));
    }
    return result;
  };
  return build(build, items);
}

bool MatchesSearchFilter(const catalog::Catalog& catalog, const catalog::TreeItem& item, std::wstring_view search_filter,
    const storage::DatabaseTags& tags) {
  if (search_filter.empty()) return true;
  if (const auto* entry = catalog.Find(item.name)) {
    if (catalog::MatchesSearchText(*entry, search_filter)) return true;
    if (entry->IsDatabase()) {
      const auto& entry_tags = TagsFor(tags, *entry);
      if (std::any_of(entry_tags.begin(), entry_tags.end(), [&](const auto& tag) {
        return utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
      })) return true;
    }
  }
  return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) {
    return MatchesSearchFilter(catalog, child, search_filter, tags);
  });
}

bool MatchesTagFilter(const catalog::Catalog& catalog, const catalog::TreeItem& item, const TreeTagFilter& filter,
    const storage::DatabaseTags& tags, const std::vector<std::wstring>& favorites) {
  if (filter.kind == TreeTagFilterKind::all) return true;
  if (const auto* entry = catalog.Find(item.name); entry && entry->IsDatabase()) {
    if (filter.kind == TreeTagFilterKind::favorites) return ContainsTag(favorites, entry->name);
    return ContainsTag(TagsFor(tags, *entry), filter.tag);
  }
  return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) {
    return MatchesTagFilter(catalog, child, filter, tags, favorites);
  });
}

LRESULT DrawTreeSearchMatches(HWND tree, NMTVCUSTOMDRAW* draw, const catalog::Catalog* catalog,
    const storage::Settings& settings, const storage::DatabaseTags& tags_by_database,
    const storage::TagStyles& styles, std::wstring_view search_filter, HFONT controls_font) {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return (!settings.simple_mode && settings.show_tags_in_list) || !search_filter.empty() ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
  if (draw->nmcd.dwDrawStage != CDDS_ITEMPOSTPAINT) return CDRF_DODEFAULT;

  const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
  wchar_t text[512]{};
  TVITEMW tree_item{};
  tree_item.mask = TVIF_TEXT;
  tree_item.hItem = item;
  tree_item.pszText = text;
  tree_item.cchTextMax = 512;
  if (!TreeView_GetItem(tree, &tree_item)) return CDRF_DODEFAULT;
  const std::wstring_view label(text);

  RECT label_rect{};
  if (!TreeView_GetItemRect(tree, item, &label_rect, TRUE)) return CDRF_DODEFAULT;
  if (catalog && TreeItemData(tree, item) == 0) {
    if (const auto* entry = catalog->Find(label); entry && entry->IsDatabase()) {
      const auto& tags = TagsFor(tags_by_database, *entry);
      const bool tag_matches_search = !search_filter.empty() && std::any_of(tags.begin(), tags.end(), [&](const auto& tag) {
        return utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
      });
      if (!tags.empty() && !settings.simple_mode && (settings.show_tags_in_list || tag_matches_search)) {
        RECT client{};
        GetClientRect(tree, &client);
        const int saved = SaveDC(draw->nmcd.hdc);
        const HFONT font = controls_font ? controls_font : reinterpret_cast<HFONT>(SendMessageW(tree, WM_GETFONT, 0, 0));
        HFONT bold_font{};
        LOGFONTW bold_description{};
        if (font && GetObjectW(font, static_cast<int>(sizeof(bold_description)), &bold_description) == static_cast<int>(sizeof(bold_description))) {
          bold_description.lfWeight = FW_BOLD;
          bold_font = CreateFontIndirectW(&bold_description);
        }
        if (font) SelectObject(draw->nmcd.hdc, font);
        SetBkMode(draw->nmcd.hdc, TRANSPARENT);
        int x = label_rect.right + 8;
        const int label_height = static_cast<int>(label_rect.bottom - label_rect.top);
        const int height = std::max(16, label_height - 2);
        const int y = static_cast<int>(label_rect.top) + (label_height - height) / 2;
        const auto measure = [&](std::wstring_view fragment, HFONT selected_font) {
          SIZE size{};
          const HGDIOBJ previous = selected_font ? SelectObject(draw->nmcd.hdc, selected_font) : nullptr;
          GetTextExtentPoint32W(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &size);
          if (previous) SelectObject(draw->nmcd.hdc, previous);
          return size.cx;
        };
        const auto tag_is_visible = [&](const std::wstring& tag) {
          return settings.show_tags_in_list || (!search_filter.empty() && utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos);
        };
        const int overflow_width = measure(L"…", font) + 14;
        const auto draw_overflow = [&](int chip_x) {
          const storage::TagStyle style{};
          const HBRUSH brush = CreateSolidBrush(style.background);
          const HPEN pen = CreatePen(PS_SOLID, 1, style.background);
          if (brush && pen) {
            const auto old_brush = SelectObject(draw->nmcd.hdc, brush);
            const auto old_pen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, chip_x, y, chip_x + overflow_width, y + height, height, height);
            SelectObject(draw->nmcd.hdc, old_brush);
            SelectObject(draw->nmcd.hdc, old_pen);
            SetTextColor(draw->nmcd.hdc, style.text);
            RECT text_rect{chip_x + 7, y, chip_x + overflow_width - 7, y + height};
            DrawTextW(draw->nmcd.hdc, L"…", 1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
          }
          if (brush) DeleteObject(brush);
          if (pen) DeleteObject(pen);
        };
        for (size_t tag_index = 0; tag_index < tags.size(); ++tag_index) {
          const auto& tag = tags[tag_index];
          const bool matches = !search_filter.empty() && utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
          if (!tag_is_visible(tag)) continue;
          int text_width = 0;
          if (matches) {
            size_t start = 0;
            size_t match = utf::FindNoCaseOrdinal(tag, search_filter, start);
            while (match != std::wstring_view::npos) {
              text_width += measure(std::wstring_view(tag).substr(start, match - start), font);
              text_width += measure(std::wstring_view(tag).substr(match, search_filter.size()), bold_font ? bold_font : font);
              start = match + search_filter.size();
              match = utf::FindNoCaseOrdinal(tag, search_filter, start);
            }
            text_width += measure(std::wstring_view(tag).substr(start), font);
          } else {
            text_width = measure(tag, font);
          }
          const int width = text_width + 14;
          const bool has_more_tags = std::any_of(tags.begin() + static_cast<std::ptrdiff_t>(tag_index + 1), tags.end(), tag_is_visible);
          if (x + width + (has_more_tags ? overflow_width + 4 : 0) > client.right - 4) {
            if (x + overflow_width <= client.right - 4) draw_overflow(x);
            break;
          }
          const auto* configured = TagStyleFor(styles, tag);
          const storage::TagStyle style = configured ? *configured : storage::TagStyle{};
          const HBRUSH brush = CreateSolidBrush(style.background);
          const HPEN pen = CreatePen(PS_SOLID, 1, style.background);
          if (brush && pen) {
            const auto old_brush = SelectObject(draw->nmcd.hdc, brush);
            const auto old_pen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, x, y, x + width, y + height, height, height);
            SelectObject(draw->nmcd.hdc, old_brush);
            SelectObject(draw->nmcd.hdc, old_pen);
            SetTextColor(draw->nmcd.hdc, style.text);
            int text_x = x + 7;
            const auto draw_segment = [&](std::wstring_view fragment, HFONT selected_font) {
              if (fragment.empty()) return;
              const HGDIOBJ previous = selected_font ? SelectObject(draw->nmcd.hdc, selected_font) : nullptr;
              SIZE size{};
              GetTextExtentPoint32W(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &size);
              RECT text_rect{text_x, y, text_x + size.cx, y + height};
              DrawTextW(draw->nmcd.hdc, fragment.data(), static_cast<int>(fragment.size()), &text_rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
              text_x += size.cx;
              if (previous) SelectObject(draw->nmcd.hdc, previous);
            };
            if (matches) {
              size_t start = 0;
              size_t match = utf::FindNoCaseOrdinal(tag, search_filter, start);
              while (match != std::wstring_view::npos) {
                draw_segment(std::wstring_view(tag).substr(start, match - start), font);
                draw_segment(std::wstring_view(tag).substr(match, search_filter.size()), bold_font ? bold_font : font);
                start = match + search_filter.size();
                match = utf::FindNoCaseOrdinal(tag, search_filter, start);
              }
              draw_segment(std::wstring_view(tag).substr(start), font);
            } else {
              draw_segment(tag, font);
            }
          }
          if (brush) DeleteObject(brush);
          if (pen) DeleteObject(pen);
          x += width + 4;
        }
        if (bold_font) DeleteObject(bold_font);
        RestoreDC(draw->nmcd.hdc, saved);
      }
    }
  }
  if (search_filter.empty() || utf::FindNoCaseOrdinal(label, search_filter) == std::wstring_view::npos) return CDRF_DODEFAULT;
  const int saved = SaveDC(draw->nmcd.hdc);
  if (const auto font = reinterpret_cast<HFONT>(SendMessageW(tree, WM_GETFONT, 0, 0))) SelectObject(draw->nmcd.hdc, font);
  SetBkMode(draw->nmcd.hdc, TRANSPARENT);
  SetTextColor(draw->nmcd.hdc, RGB(0, 97, 0));
  const HBRUSH match_brush = CreateSolidBrush(RGB(198, 239, 206));
  if (!match_brush) {
    RestoreDC(draw->nmcd.hdc, saved);
    return CDRF_DODEFAULT;
  }

  size_t start = 0;
  size_t match = utf::FindNoCaseOrdinal(label, search_filter, start);
  while (match != std::wstring_view::npos) {
    SIZE prefix_size{}, match_size{};
    GetTextExtentPoint32W(draw->nmcd.hdc, label.data(), static_cast<int>(match), &prefix_size);
    GetTextExtentPoint32W(draw->nmcd.hdc, label.data() + match, static_cast<int>(search_filter.size()), &match_size);
    RECT match_rect{label_rect.left + prefix_size.cx, label_rect.top + 1, label_rect.left + prefix_size.cx + match_size.cx, label_rect.bottom - 1};
    FillRect(draw->nmcd.hdc, &match_rect, match_brush);
    RECT text_rect{match_rect.left, label_rect.top, match_rect.right, label_rect.bottom};
    DrawTextW(draw->nmcd.hdc, label.data() + match, static_cast<int>(search_filter.size()), &text_rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    start = match + search_filter.size();
    match = utf::FindNoCaseOrdinal(label, search_filter, start);
  }
  DeleteObject(match_brush);
  RestoreDC(draw->nmcd.hdc, saved);
  return CDRF_DODEFAULT;
}

}  // namespace ibstart::ui::presentation
