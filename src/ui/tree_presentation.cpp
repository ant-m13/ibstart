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
  return domain::EqualIdentifier(left, right);
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

const domain::Entry* EntryForItem(const catalog::Catalog& catalog, const catalog::TreeItem& item) {
  return item.section_index == catalog::kInvalidSectionIndex ? catalog.Find(item.name) :
      catalog.FindBySectionIndex(item.section_index);
}

const domain::Entry* EntryForTreeRow(const catalog::Catalog& catalog, LPARAM item_data, std::wstring_view name) {
  if (item_data >= static_cast<LPARAM>(catalog::kCatalogItemDataBase)) {
    return catalog.FindBySectionIndex(static_cast<size_t>(item_data - static_cast<LPARAM>(catalog::kCatalogItemDataBase)));
  }
  return catalog.Find(name);
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

std::wstring TagId(const domain::Entry& entry) { return catalog::StableDatabaseId(entry); }

const std::vector<std::wstring>& TagsFor(const storage::DatabaseTags& tags, const domain::Entry& entry) {
  static const std::vector<std::wstring> empty;
  const auto found = tags.find(TagId(entry));
  return found == tags.end() ? empty : found->second;
}

bool IsFavorite(const std::vector<std::wstring>& favorites, const domain::Entry& entry) {
  return ContainsTag(favorites, TagId(entry)) || ContainsTag(favorites, entry.name);
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
  for (const auto& launch : history) {
    if (const auto* entry = catalog.FindById(launch.database_id)) result.push_back(entry->name);
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
      if (const auto* entry = EntryForItem(catalog, item)) {
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
      if (const auto* entry = EntryForItem(catalog, item); entry && entry->IsDatabase()) {
        if (filter.kind == TreeTagFilterKind::favorites) result = IsFavorite(favorites, *entry);
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
  if (const auto* entry = EntryForItem(catalog, item)) {
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
  if (const auto* entry = EntryForItem(catalog, item); entry && entry->IsDatabase()) {
    if (filter.kind == TreeTagFilterKind::favorites) return IsFavorite(favorites, *entry);
    return ContainsTag(TagsFor(tags, *entry), filter.tag);
  }
  return std::any_of(item.children.begin(), item.children.end(), [&](const auto& child) {
    return MatchesTagFilter(catalog, child, filter, tags, favorites);
  });
}

LRESULT DrawTreeSearchMatches(HWND tree, NMTVCUSTOMDRAW* draw, const catalog::Catalog* catalog,
    const storage::Settings& settings, const storage::DatabaseTags& tags_by_database,
    const storage::TagStyles& styles, std::wstring_view search_filter, HFONT controls_font, HFONT controls_bold_font) {
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
    if ((settings.simple_mode || !settings.show_tags_in_list) && search_filter.empty()) return CDRF_DODEFAULT;
    return CDRF_NOTIFYITEMDRAW;
  }
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
    const bool needs_postpaint = (!settings.simple_mode && settings.show_tags_in_list) || !search_filter.empty();
    if (!needs_postpaint) return CDRF_DODEFAULT;

    if (!search_filter.empty()) {
      const auto item = reinterpret_cast<HTREEITEM>(draw->nmcd.dwItemSpec);
      wchar_t text[512]{};
      TVITEMW tree_item{};
      tree_item.mask = TVIF_TEXT;
      tree_item.hItem = item;
      tree_item.pszText = text;
      tree_item.cchTextMax = 512;
      if (TreeView_GetItem(tree, &tree_item) &&
          utf::FindNoCaseOrdinal(std::wstring_view(text), search_filter) != std::wstring_view::npos) {
        COLORREF text_background = draw->clrTextBk;
        if (text_background == CLR_NONE) {
          text_background = (draw->nmcd.uItemState & CDIS_SELECTED) != 0 ?
              GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
        }
        // Keep the default TreeView item drawing (icons, lines, selection),
        // but make its copy of the label invisible.  The label is rendered
        // once in CDDS_ITEMPOSTPAINT below.
        draw->clrText = text_background;
      }
    }
    return CDRF_NOTIFYPOSTPAINT;
  }
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
  if (catalog) {
    if (const auto* entry = EntryForTreeRow(*catalog, TreeItemData(tree, item), label); entry && entry->IsDatabase()) {
      const auto& tags = TagsFor(tags_by_database, *entry);
      const bool tag_matches_search = !search_filter.empty() && std::any_of(tags.begin(), tags.end(), [&](const auto& tag) {
        return utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
      });
      if (!tags.empty() && !settings.simple_mode && (settings.show_tags_in_list || tag_matches_search)) {
        RECT client{};
        GetClientRect(tree, &client);
        const int saved = SaveDC(draw->nmcd.hdc);
        const HFONT font = controls_font ? controls_font : reinterpret_cast<HFONT>(SendMessageW(tree, WM_GETFONT, 0, 0));
        HFONT bold_font = controls_bold_font;
        if (!bold_font) {
          LOGFONTW bold_description{};
          if (font && GetObjectW(font, static_cast<int>(sizeof(bold_description)), &bold_description) == static_cast<int>(sizeof(bold_description))) {
            bold_description.lfWeight = FW_BOLD;
            bold_font = CreateFontIndirectW(&bold_description);
          }
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
          const HBRUSH brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
          const HPEN pen = reinterpret_cast<HPEN>(GetStockObject(DC_PEN));
          if (brush && pen) {
            SetDCBrushColor(draw->nmcd.hdc, style.background);
            SetDCPenColor(draw->nmcd.hdc, style.background);
            const auto old_brush = SelectObject(draw->nmcd.hdc, brush);
            const auto old_pen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, chip_x, y, chip_x + overflow_width, y + height, height, height);
            SelectObject(draw->nmcd.hdc, old_brush);
            SelectObject(draw->nmcd.hdc, old_pen);
            SetTextColor(draw->nmcd.hdc, style.text);
            RECT text_rect{chip_x + 7, y, chip_x + overflow_width - 7, y + height};
            DrawTextW(draw->nmcd.hdc, L"…", 1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
          }
        };
        for (size_t tag_index = 0; tag_index < tags.size(); ++tag_index) {
          const auto& tag = tags[tag_index];
          const bool matches = !search_filter.empty() && utf::FindNoCaseOrdinal(tag, search_filter) != std::wstring_view::npos;
          if (!tag_is_visible(tag)) continue;
          struct TagSegment {
            std::wstring_view text;
            HFONT font;
            int width;
          };
          std::vector<TagSegment> segments;
          segments.reserve(matches ? 4 : 1);
          int text_width = 0;
          const auto append_segment = [&](std::wstring_view fragment, HFONT segment_font) {
            if (fragment.empty()) return;
            const int width = measure(fragment, segment_font);
            segments.push_back({fragment, segment_font, width});
            text_width += width;
          };
          if (matches) {
            size_t start = 0;
            size_t match = utf::FindNoCaseOrdinal(tag, search_filter, start);
            while (match != std::wstring_view::npos) {
              append_segment(std::wstring_view(tag).substr(start, match - start), font);
              append_segment(std::wstring_view(tag).substr(match, search_filter.size()), bold_font ? bold_font : font);
              start = match + search_filter.size();
              match = utf::FindNoCaseOrdinal(tag, search_filter, start);
            }
            append_segment(std::wstring_view(tag).substr(start), font);
          } else {
            append_segment(tag, font);
          }
          const int width = text_width + 14;
          const bool has_more_tags = std::any_of(tags.begin() + static_cast<std::ptrdiff_t>(tag_index + 1), tags.end(), tag_is_visible);
          if (x + width + (has_more_tags ? overflow_width + 4 : 0) > client.right - 4) {
            if (x + overflow_width <= client.right - 4) draw_overflow(x);
            break;
          }
          const auto* configured = TagStyleFor(styles, tag);
          const storage::TagStyle style = configured ? *configured : storage::TagStyle{};
          const HBRUSH brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
          const HPEN pen = reinterpret_cast<HPEN>(GetStockObject(DC_PEN));
          if (brush && pen) {
            SetDCBrushColor(draw->nmcd.hdc, style.background);
            SetDCPenColor(draw->nmcd.hdc, style.background);
            const auto old_brush = SelectObject(draw->nmcd.hdc, brush);
            const auto old_pen = SelectObject(draw->nmcd.hdc, pen);
            RoundRect(draw->nmcd.hdc, x, y, x + width, y + height, height, height);
            SelectObject(draw->nmcd.hdc, old_brush);
            SelectObject(draw->nmcd.hdc, old_pen);
            SetTextColor(draw->nmcd.hdc, style.text);
            int text_x = x + 7;
            for (const auto& segment : segments) {
              const HGDIOBJ previous = segment.font ? SelectObject(draw->nmcd.hdc, segment.font) : nullptr;
              RECT text_rect{text_x, y, text_x + segment.width, y + height};
              DrawTextW(draw->nmcd.hdc, segment.text.data(), static_cast<int>(segment.text.size()), &text_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
              if (previous) SelectObject(draw->nmcd.hdc, previous);
              text_x += segment.width;
            }
          }
          x += width + 4;
        }
        if (!controls_bold_font && bold_font) DeleteObject(bold_font);
        RestoreDC(draw->nmcd.hdc, saved);
      }
    }
  }
  if (search_filter.empty() || utf::FindNoCaseOrdinal(label, search_filter) == std::wstring_view::npos) return CDRF_DODEFAULT;
  const int saved = SaveDC(draw->nmcd.hdc);
  if (const auto font = reinterpret_cast<HFONT>(SendMessageW(tree, WM_GETFONT, 0, 0))) SelectObject(draw->nmcd.hdc, font);
  SetBkMode(draw->nmcd.hdc, TRANSPARENT);
  const bool selected = TreeView_GetSelection(tree) == item;
  const COLORREF normal_text_color = GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
  const HBRUSH fill_brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  if (!fill_brush) {
    RestoreDC(draw->nmcd.hdc, saved);
    return CDRF_DODEFAULT;
  }

  // Measure complete prefixes in the same context in which the label is
  // rendered.  This keeps the segment boundaries aligned with the text (for
  // example, `etai` inside `retail3`).
  const auto text_x = [&](size_t index) {
    SIZE extent{};
    if (index != 0) GetTextExtentPoint32W(draw->nmcd.hdc, label.data(), static_cast<int>(index), &extent);
    return label_rect.left + extent.cx;
  };

  COLORREF label_background = draw->clrTextBk;
  if (label_background == CLR_NONE) label_background = TreeView_GetBkColor(tree);
  if (label_background == CLR_NONE) label_background = selected ?
      GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
  SetDCBrushColor(draw->nmcd.hdc, label_background);
  FillRect(draw->nmcd.hdc, &label_rect, fill_brush);

  const COLORREF match_text_color = RGB(0, 97, 0);
  const COLORREF match_background = RGB(198, 239, 206);
  const auto draw_segment = [&](size_t begin, size_t end, COLORREF color) {
    if (begin >= end) return;
    const LONG left = text_x(begin);
    const LONG right = text_x(end);
    if (left >= right) return;
    SetTextColor(draw->nmcd.hdc, color);
    RECT text_rect{left, label_rect.top, right, label_rect.bottom};
    DrawTextW(draw->nmcd.hdc, label.data() + begin, static_cast<int>(end - begin), &text_rect,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  };

  SetDCBrushColor(draw->nmcd.hdc, match_background);
  size_t start = 0;
  size_t match = utf::FindNoCaseOrdinal(label, search_filter, start);
  while (match != std::wstring_view::npos) {
    const size_t match_end = match + search_filter.size();
    const LONG match_left = text_x(match);
    const LONG match_right = text_x(match_end);
    RECT match_rect{
        std::max<LONG>(label_rect.left, match_left),
        label_rect.top,
        std::min<LONG>(label_rect.right, match_right),
        label_rect.bottom
    };
    if (match_rect.left < match_rect.right && match_rect.top < match_rect.bottom) {
      draw_segment(start, match, normal_text_color);
      FillRect(draw->nmcd.hdc, &match_rect, fill_brush);
      draw_segment(match, match_end, match_text_color);
    } else {
      draw_segment(start, match_end, normal_text_color);
    }
    start = match_end;
    match = utf::FindNoCaseOrdinal(label, search_filter, start);
  }
  draw_segment(start, label.size(), normal_text_color);
  RestoreDC(draw->nmcd.hdc, saved);
  return CDRF_DODEFAULT;
}

}  // namespace ibstart::ui::presentation
