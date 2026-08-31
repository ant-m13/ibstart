#include "ui/tree_view_controller.hpp"

#include "app/resource.h"
#include "core/connection/connection_string.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace ibstart::ui {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
          static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
LPARAM CatalogItemData(size_t section_index) {
  if (section_index == catalog::kInvalidSectionIndex) return 0;
  return static_cast<LPARAM>(catalog::kCatalogItemDataBase + section_index);
}

bool IsCatalogItemData(LPARAM data) {
  return data >= static_cast<LPARAM>(catalog::kCatalogItemDataBase);
}

std::wstring ReadTreeItemText(HWND tree, HTREEITEM item) {
  if (!tree || !item) return {};
  std::wstring text(256, L'\0');
  for (;;) {
    TVITEMW data{};
    data.mask = TVIF_TEXT;
    data.hItem = item;
    data.pszText = text.data();
    data.cchTextMax = static_cast<int>(text.size());
    if (!TreeView_GetItem(tree, &data)) return {};

    const auto end = std::find(text.begin(), text.end(), L'\0');
    if (end != text.end() && end != text.end() - 1) {
      text.erase(end, text.end());
      return text;
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()) / 2) return {};
    text.resize(text.size() * 2, L'\0');
  }
}

std::optional<size_t> CatalogSectionIndex(const catalog::Catalog& database_catalog, const domain::Entry* entry) {
  if (!entry) return std::nullopt;
  for (size_t index = 0; index < database_catalog.document().sections.size(); ++index) {
    if (&database_catalog.document().sections[index].entry == entry) return index;
  }
  return std::nullopt;
}

HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HICON CreateWebDatabaseIcon() {
  constexpr int size = 20;
  HDC screen = GetDC(nullptr);
  HDC color = screen ? CreateCompatibleDC(screen) : nullptr;
  HDC mask = screen ? CreateCompatibleDC(screen) : nullptr;
  HBITMAP color_bitmap = screen ? CreateCompatibleBitmap(screen, size, size) : nullptr;
  HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, nullptr);
  HICON icon{};
  if (color && mask && color_bitmap && mask_bitmap) {
    const auto previous_color = SelectObject(color, color_bitmap);
    RECT bounds{0, 0, size, size};
    FillRect(color, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const HBRUSH globe_brush = CreateSolidBrush(RGB(225, 245, 255));
    const HPEN globe_pen = CreatePen(PS_SOLID, 2, RGB(0, 112, 156));
    const auto previous_brush = SelectObject(color, globe_brush);
    const auto previous_pen = SelectObject(color, globe_pen);
    Ellipse(color, 2, 2, 18, 18);
    SelectObject(color, GetStockObject(HOLLOW_BRUSH));
    Ellipse(color, 6, 2, 14, 18);
    MoveToEx(color, 3, 10, nullptr);
    LineTo(color, 17, 10);
    MoveToEx(color, 5, 6, nullptr);
    LineTo(color, 15, 6);
    MoveToEx(color, 5, 14, nullptr);
    LineTo(color, 15, 14);
    SelectObject(color, previous_brush);
    SelectObject(color, previous_pen);
    DeleteObject(globe_brush);
    DeleteObject(globe_pen);
    SelectObject(color, previous_color);

    const auto previous_mask = SelectObject(mask, mask_bitmap);
    PatBlt(mask, 0, 0, size, size, WHITENESS);
    const HBRUSH mask_brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const HPEN mask_pen = static_cast<HPEN>(GetStockObject(BLACK_PEN));
    const auto previous_mask_brush = SelectObject(mask, mask_brush);
    const auto previous_mask_pen = SelectObject(mask, mask_pen);
    Ellipse(mask, 1, 1, 19, 19);
    SelectObject(mask, previous_mask_brush);
    SelectObject(mask, previous_mask_pen);
    SelectObject(mask, previous_mask);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color_bitmap;
    info.hbmMask = mask_bitmap;
    icon = CreateIconIndirect(&info);
  }
  if (color) DeleteDC(color);
  if (mask) DeleteDC(mask);
  if (color_bitmap) DeleteObject(color_bitmap);
  if (mask_bitmap) DeleteObject(mask_bitmap);
  if (screen) ReleaseDC(nullptr, screen);
  return icon;
}

}  // namespace

TreeViewController::~TreeViewController() {
  if (images_) ImageList_Destroy(images_);
}

void TreeViewController::Attach(HWND tree, HINSTANCE instance) {
  if (images_) {
    if (tree_ && IsWindow(tree_)) TreeView_SetImageList(tree_, nullptr, TVSIL_NORMAL);
    ImageList_Destroy(images_);
    images_ = nullptr;
  }
  tree_ = tree;
  search_active_ = false;
  pre_search_expansion_states_.reset();
  if (!tree_) return;

  TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
  images_ = ImageList_Create(20, 20, ILC_COLOR32 | ILC_MASK, 6, 1);
  if (!images_) return;

  bool complete = true;
  const auto append_icon = [&](HICON icon) {
    if (!icon || ImageList_AddIcon(images_, icon) < 0) complete = false;
    if (icon) DestroyIcon(icon);
  };
  for (const int resource : {IDI_TREE_FILE_DATABASE, IDI_TREE_SERVER_DATABASE}) {
    append_icon(LoadResourceIcon(instance, resource, 20));
  }
  HICON web_icon = CreateWebDatabaseIcon();
  if (!web_icon) web_icon = LoadResourceIcon(instance, IDI_TREE_SERVER_DATABASE, 20);
  append_icon(web_icon);
  for (const int resource : {IDI_TREE_FOLDER, IDI_ACTION_FAVORITE, IDI_ACTION_REFRESH}) {
    append_icon(LoadResourceIcon(instance, resource, 20));
  }
  if (complete) {
    TreeView_SetImageList(tree_, images_, TVSIL_NORMAL);
  } else {
    ImageList_Destroy(images_);
    images_ = nullptr;
  }
}

void TreeViewController::Clear() const {
  if (tree_) TreeView_DeleteAllItems(tree_);
  search_active_ = false;
  pre_search_expansion_states_.reset();
}

void TreeViewController::Populate(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter, bool simple_mode) const {
  if (!tree_) return;

  // Keep existing HTREEITEM handles whenever possible.  Reconciliation is
  // important here: changing a filter or a tag should not destroy an
  // unrelated expanded branch just to recreate the same rows.
  const auto view_state = CaptureViewState();
  const bool has_search_filter = !search_filter.empty();
  const bool leaving_search = !has_search_filter && search_active_;
  ViewState restore_state = view_state;
  if (leaving_search && pre_search_expansion_states_) {
    restore_state.expansion_states = *pre_search_expansion_states_;
  }
  const bool can_suspend_drawing = IsWindow(tree_);
  if (can_suspend_drawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resume_drawing = [&] {
    if (!can_suspend_drawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };

  try {
    const auto catalog_items = presentation::FilterTreeItems(database_catalog, database_catalog.Tree(),
        search_filter, tag_filter, catalog_state.tags, filter_favorites);
    HTREEITEM catalog_root = FindTopLevelItem(kCatalogRootItemData);
    if (!catalog_root) {
      catalog_root = InsertCatalogRoot();
      if (catalog_root) TreeView_Expand(tree_, catalog_root, TVE_EXPAND);
    }
    if (catalog_root) {
      ReconcileChildren(database_catalog, catalog_items, catalog_root);
    }

    if (simple_mode) {
      for (const LPARAM item_data : {kFavoritesRootItemData, kRecentRootItemData}) {
        if (const HTREEITEM root = FindTopLevelItem(item_data)) TreeView_DeleteItem(tree_, root);
      }
    } else {
      ReconcileSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter, tag_filter,
          L"Избранное", catalog_state.favorites, kFavoriteImage, kFavoritesRootItemData, catalog_root, true);
      const HTREEITEM favorites_root = FindTopLevelItem(kFavoritesRootItemData);
      ReconcileSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter, tag_filter,
          L"Недавние", presentation::CollectRecentDatabaseNames(database_catalog, catalog_state.history),
          kRecentImage, kRecentRootItemData, favorites_root ? favorites_root : catalog_root, false);
    }

    RestoreViewState(restore_state, has_search_filter);
    if (has_search_filter && !search_active_) {
      pre_search_expansion_states_ = view_state.expansion_states;
    } else if (!has_search_filter) {
      pre_search_expansion_states_.reset();
    }
    search_active_ = has_search_filter;
  } catch (...) {
    resume_drawing();
    throw;
  }
  resume_drawing();
}

void TreeViewController::RefreshRecentBranch(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter,
    std::optional<size_t> selected_recent_section_index) const {
  if (!tree_) return;
  const auto recent = presentation::CollectRecentDatabaseNames(database_catalog, catalog_state.history);
  const auto view_state = CaptureViewState();
  const bool can_suspend_drawing = IsWindow(tree_);
  if (can_suspend_drawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resume_drawing = [&] {
    if (!can_suspend_drawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };
  try {
    ReconcileSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter, tag_filter,
        L"Недавние", recent, kRecentImage, kRecentRootItemData,
        FindTopLevelItem(kFavoritesRootItemData), false);
    RestoreViewState(view_state, !search_filter.empty());
    if (selected_recent_section_index) {
      if (const HTREEITEM item = FindItemInBranch(CatalogItemData(*selected_recent_section_index), kRecentRootItemData)) {
        TreeView_SelectItem(tree_, item);
        TreeView_EnsureVisible(tree_, item);
      }
    }
  } catch (...) {
    resume_drawing();
    throw;
  }
  resume_drawing();
}

std::wstring TreeViewController::ItemName(HTREEITEM item) const {
  return ReadTreeItemText(tree_, item);
}

LPARAM TreeViewController::ItemData(HTREEITEM item) const {
  if (!tree_ || !item) return 0;
  TVITEMW data{};
  data.mask = TVIF_PARAM;
  data.hItem = item;
  return TreeView_GetItem(tree_, &data) ? data.lParam : 0;
}

bool TreeViewController::IsVirtualBranch(HTREEITEM item) const {
  for (auto current = item; current; current = TreeView_GetParent(tree_, current)) {
    const LPARAM data = ItemData(current);
    if (data == kRecentRootItemData || data == kFavoritesRootItemData) return true;
  }
  return false;
}

LPARAM TreeViewController::BranchData(HTREEITEM item) const {
  for (auto current = item; current; current = TreeView_GetParent(tree_, current)) {
    const LPARAM data = ItemData(current);
    if (data == kRecentRootItemData || data == kFavoritesRootItemData || data == kCatalogRootItemData) return data;
  }
  return 0;
}

std::wstring TreeViewController::SelectedName() const {
  if (!tree_) return {};
  const HTREEITEM selected = TreeView_GetSelection(tree_);
  return IsCatalogItemData(ItemData(selected)) ? ItemName(selected) : L"";
}

std::optional<size_t> TreeViewController::SectionIndex(HTREEITEM item) const {
  const LPARAM data = ItemData(item);
  if (!IsCatalogItemData(data)) return std::nullopt;
  return static_cast<size_t>(data - static_cast<LPARAM>(catalog::kCatalogItemDataBase));
}

std::optional<size_t> TreeViewController::SelectedSectionIndex() const {
  return SectionIndex(tree_ ? TreeView_GetSelection(tree_) : nullptr);
}

std::optional<size_t> TreeViewController::SelectedSectionIndex(const catalog::Catalog& database_catalog) const {
  const auto selected_index = SelectedSectionIndex();
  if (!selected_index) return std::nullopt;
  const auto* entry = database_catalog.FindBySectionIndex(*selected_index);
  if (!entry || !EqualNoCase(SelectedName(), entry->name)) return std::nullopt;
  return selected_index;
}

bool TreeViewController::SelectedItemIsRecentRoot() const {
  return tree_ && ItemData(TreeView_GetSelection(tree_)) == kRecentRootItemData;
}

bool TreeViewController::SelectItem(std::wstring_view name) const {
  if (!tree_ || name.empty()) return false;
  const HTREEITEM item = FindItemByName(TreeView_GetRoot(tree_), name);
  if (!item) return false;
  TreeView_SelectItem(tree_, item);
  TreeView_EnsureVisible(tree_, item);
  return true;
}

bool TreeViewController::SelectItem(size_t section_index) const {
  if (!tree_ || section_index == catalog::kInvalidSectionIndex) return false;
  const HTREEITEM item = FindItemByData(TreeView_GetRoot(tree_), CatalogItemData(section_index));
  if (!item) return false;
  TreeView_SelectItem(tree_, item);
  TreeView_EnsureVisible(tree_, item);
  return true;
}

bool TreeViewController::SelectItemInBranch(std::wstring_view name, LPARAM branch_data) const {
  if (!tree_ || name.empty()) return false;
  const HTREEITEM item = FindItemInBranch(name, branch_data);
  if (!item) return false;
  TreeView_SelectItem(tree_, item);
  TreeView_EnsureVisible(tree_, item);
  return true;
}

bool TreeViewController::SelectCatalogRoot() const {
  const HTREEITEM item = FindTopLevelItem(kCatalogRootItemData);
  if (!item) return false;
  TreeView_SelectItem(tree_, item);
  TreeView_EnsureVisible(tree_, item);
  return true;
}

TreeViewController::ExpansionStates TreeViewController::CaptureExpansionStates() const {
  ExpansionStates result;
  const auto collect = [&](const auto& self, HTREEITEM item) -> void {
    for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
      const auto child = TreeView_GetChild(tree_, current);
      if (child) {
        const auto name = ItemName(current);
        if (!name.empty()) {
          result.emplace(name,
              (TreeView_GetItemState(tree_, current, TVIS_EXPANDED) & TVIS_EXPANDED) != 0);
        }
        self(self, child);
      }
    }
  };
  if (tree_) collect(collect, TreeView_GetRoot(tree_));
  return result;
}

void TreeViewController::RestoreExpansionStates(const ExpansionStates& states) const {
  const auto restore = [&](const auto& self, HTREEITEM item) -> void {
    for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
      const auto child = TreeView_GetChild(tree_, current);
      if (!child) continue;
      const auto name = ItemName(current);
      if (const auto found = states.find(name); found != states.end()) {
        TreeView_Expand(tree_, current, found->second ? TVE_EXPAND : TVE_COLLAPSE);
      }
      self(self, child);
    }
  };
  if (tree_) restore(restore, TreeView_GetRoot(tree_));
}

TreeViewController::ViewState TreeViewController::CaptureViewState() const {
  ViewState result;
  if (!tree_) return result;
  result.expansion_states = CaptureExpansionStates();
  const HTREEITEM selection = TreeView_GetSelection(tree_);
  result.selected_item_data = ItemData(selection);
  result.selected_branch_data = BranchData(selection);
  result.selected_name = SelectedName();
  const HTREEITEM first_visible = TreeView_GetNextItem(tree_, nullptr, TVGN_FIRSTVISIBLE);
  result.first_visible_data = ItemData(first_visible);
  result.first_visible_branch_data = BranchData(first_visible);
  if (IsCatalogItemData(result.first_visible_data)) result.first_visible_name = ItemName(first_visible);
  return result;
}

void TreeViewController::RestoreViewState(const ViewState& state, bool expand_visible_branches) const {
  if (!tree_) return;
  RestoreExpansionStates(state.expansion_states);
  if (expand_visible_branches) ExpandVisibleBranches(TreeView_GetRoot(tree_));
  const auto matches_saved_name = [&](HTREEITEM item) {
    return item && (state.selected_name.empty() ||
        (IsCatalogItemData(ItemData(item)) && EqualNoCase(ItemName(item), state.selected_name)));
  };
  const auto select = [&](HTREEITEM item) {
    if (!matches_saved_name(item)) return false;
    TreeView_SelectItem(tree_, item);
    TreeView_EnsureVisible(tree_, item);
    return true;
  };

  bool selection_restored = state.selected_name.empty() && state.selected_item_data == 0;
  if (state.selected_item_data != 0) {
    selection_restored = select(FindItemInBranch(state.selected_item_data, state.selected_branch_data));
  }
  if (!selection_restored && !state.selected_name.empty()) {
    selection_restored = select(FindItemInBranch(state.selected_name, state.selected_branch_data));
  }
  // A database can disappear from a virtual branch because it was removed
  // from favorites or recent launches.  Keep the same database selected in
  // the ordinary catalog when it is still visible instead of leaving the
  // native control to choose an unrelated neighbor.
  if (!selection_restored && IsCatalogItemData(state.selected_item_data) &&
      (state.selected_branch_data == kRecentRootItemData || state.selected_branch_data == kFavoritesRootItemData)) {
    selection_restored = select(FindItemInBranch(state.selected_item_data, kCatalogRootItemData));
    if (!selection_restored && !state.selected_name.empty()) {
      selection_restored = select(FindItemInBranch(state.selected_name, kCatalogRootItemData));
    }
  }
  // EnsureVisible can expand ancestors of the selected row.  Reapply the
  // captured state so a refresh never changes a branch the user collapsed,
  // except while search deliberately exposes matching branches.
  if (selection_restored && !expand_visible_branches) RestoreExpansionStates(state.expansion_states);
  if (!selection_restored) TreeView_SelectItem(tree_, nullptr);

  // Keep the vertical viewport anchored to the same row where possible. This
  // prevents an update from looking like a jump in a large tree.
  const auto matches_first_visible_name = [&](HTREEITEM item) {
    return item && (state.first_visible_name.empty() ||
        (IsCatalogItemData(ItemData(item)) && EqualNoCase(ItemName(item), state.first_visible_name)));
  };
  HTREEITEM first_visible{};
  if (state.first_visible_data != 0) {
    const HTREEITEM item = FindItemInBranch(state.first_visible_data, state.first_visible_branch_data);
    if (matches_first_visible_name(item)) first_visible = item;
  }
  if (!first_visible && !state.first_visible_name.empty()) {
    first_visible = FindItemInBranch(state.first_visible_name, state.first_visible_branch_data);
  }
  if (first_visible) SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(first_visible));
}

void TreeViewController::ExpandVisibleBranches(HTREEITEM item) const {
  if (!tree_) return;
  // FilterTreeItems leaves only matching rows and their ancestor groups in
  // the control, so every child-bearing row here is part of a search result.
  for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
    const HTREEITEM child = TreeView_GetChild(tree_, current);
    if (!child) continue;
    TreeView_Expand(tree_, current, TVE_EXPAND);
    ExpandVisibleBranches(child);
  }
}

void TreeViewController::UpdateTreeItem(HTREEITEM handle, const catalog::Catalog& database_catalog,
    const catalog::TreeItem& item) const {
  if (!tree_ || !handle) return;
  TVITEMW row{};
  row.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
  row.hItem = handle;
  row.pszText = const_cast<wchar_t*>(item.name.c_str());
  const auto* entry = database_catalog.FindBySectionIndex(item.section_index);
  row.lParam = CatalogItemData(item.section_index);
  row.iImage = row.iSelectedImage = item.database ? DatabaseImage(entry) : kFolderImage;
  TreeView_SetItem(tree_, &row);
}

void TreeViewController::DeleteChildren(HTREEITEM parent) const {
  if (!tree_ || !parent) return;
  for (HTREEITEM child = TreeView_GetChild(tree_, parent); child;) {
    const HTREEITEM next = TreeView_GetNextSibling(tree_, child);
    TreeView_DeleteItem(tree_, child);
    child = next;
  }
}

HTREEITEM TreeViewController::InsertCatalogRoot() const {
  if (!tree_) return nullptr;
  std::wstring root_text(kCatalogRootName);
  TVINSERTSTRUCTW root{};
  root.hParent = TVI_ROOT;
  root.hInsertAfter = TVI_FIRST;
  root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_STATE;
  root.item.pszText = root_text.data();
  root.item.iImage = root.item.iSelectedImage = kFolderImage;
  root.item.lParam = kCatalogRootItemData;
  root.item.stateMask = TVIS_EXPANDED;
  root.item.state = TVIS_EXPANDED;
  return TreeView_InsertItem(tree_, &root);
}

void TreeViewController::AddItems(const catalog::Catalog& database_catalog,
    const std::vector<catalog::TreeItem>& items, HTREEITEM parent) const {
  for (const auto& item : items) {
    TVINSERTSTRUCTW row{};
    row.hParent = parent;
    row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
    row.item.pszText = const_cast<wchar_t*>(item.name.c_str());
    const auto* entry = database_catalog.FindBySectionIndex(item.section_index);
    row.item.lParam = CatalogItemData(item.section_index);
    row.item.iImage = row.item.iSelectedImage = item.database ? DatabaseImage(entry) : kFolderImage;
    const HTREEITEM handle = TreeView_InsertItem(tree_, &row);
    if (handle && !item.database) {
      AddItems(database_catalog, item.children, handle);
    }
  }
}

void TreeViewController::ReconcileChildren(const catalog::Catalog& database_catalog,
    const std::vector<catalog::TreeItem>& items, HTREEITEM parent) const {
  if (!tree_ || !parent) return;

  std::vector<const catalog::TreeItem*> visible;
  visible.reserve(items.size());
  for (const auto& item : items) visible.push_back(&item);

  std::vector<HTREEITEM> existing;
  for (HTREEITEM child = TreeView_GetChild(tree_, parent); child;
      child = TreeView_GetNextSibling(tree_, child)) {
    existing.push_back(child);
  }
  std::map<size_t, size_t> existing_by_index;
  for (size_t index = 0; index < existing.size(); ++index) {
    if (const auto section_index = SectionIndex(existing[index])) {
      existing_by_index.emplace(*section_index, index);
    }
  }

  // TreeView has no move operation.  If an existing row changed relative
  // order, rebuild only this parent; additions and removals still reuse all
  // unaffected handles.
  std::vector<size_t> existing_positions;
  existing_positions.reserve(visible.size());
  for (const auto* item : visible) {
    if (const auto found = existing_by_index.find(item->section_index); found != existing_by_index.end()) {
      existing_positions.push_back(found->second);
    }
  }
  if (!std::is_sorted(existing_positions.begin(), existing_positions.end())) {
    DeleteChildren(parent);
    AddItems(database_catalog, items, parent);
    return;
  }

  std::vector<bool> used(existing.size(), false);
  HTREEITEM previous = nullptr;
  for (const auto* item : visible) {
    HTREEITEM handle = nullptr;
    bool inserted = false;
    size_t existing_position = existing.size();
    if (const auto found = existing_by_index.find(item->section_index); found != existing_by_index.end() &&
        !used[found->second]) {
      existing_position = found->second;
      handle = existing[existing_position];
    }
    if (!handle) {
      TVINSERTSTRUCTW row{};
      row.hParent = parent;
      row.hInsertAfter = previous ? previous : TVI_FIRST;
      row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
      row.item.pszText = const_cast<wchar_t*>(item->name.c_str());
      const auto* entry = database_catalog.FindBySectionIndex(item->section_index);
      row.item.lParam = CatalogItemData(item->section_index);
      row.item.iImage = row.item.iSelectedImage = item->database ? DatabaseImage(entry) : kFolderImage;
      handle = TreeView_InsertItem(tree_, &row);
      if (!handle) continue;
      inserted = true;
    }
    if (existing_position < existing.size()) used[existing_position] = true;
    if (!inserted) UpdateTreeItem(handle, database_catalog, *item);
    if (item->database) {
      DeleteChildren(handle);
    } else {
      ReconcileChildren(database_catalog, item->children, handle);
    }
    previous = handle;
  }

  for (size_t index = existing.size(); index-- > 0;) {
    if (!used[index]) TreeView_DeleteItem(tree_, existing[index]);
  }
}

void TreeViewController::ReconcileSpecialRoot(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter,
    std::wstring_view root_name, const std::vector<std::wstring>& names, int image, LPARAM item_data,
    HTREEITEM insert_after, bool names_are_ids) const {
  if (!tree_) return;
  std::vector<catalog::TreeItem> raw_items;
  raw_items.reserve(names.size());
  for (const auto& name : names) {
    const auto* entry = names_are_ids ? database_catalog.FindById(name) : database_catalog.Find(name);
    if (!entry && names_are_ids) entry = database_catalog.Find(name);
    if (!entry || !entry->IsDatabase()) continue;
    const auto section_index = CatalogSectionIndex(database_catalog, entry);
    if (!section_index) continue;
    raw_items.push_back({entry->name, true, {}, {}, *section_index});
  }
  const auto items = presentation::FilterTreeItems(database_catalog, raw_items, search_filter, tag_filter,
      catalog_state.tags, filter_favorites);

  HTREEITEM root_handle = FindTopLevelItem(item_data);
  if (items.empty()) {
    if (root_handle) TreeView_DeleteItem(tree_, root_handle);
    return;
  }
  if (!root_handle) {
    std::wstring root_text(root_name);
    TVINSERTSTRUCTW root{};
    root.hParent = TVI_ROOT;
    root.hInsertAfter = insert_after ? insert_after : TVI_LAST;
    root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
    root.item.pszText = root_text.data();
    root.item.lParam = item_data;
    root.item.iImage = root.item.iSelectedImage = image;
    root_handle = TreeView_InsertItem(tree_, &root);
    if (!root_handle) return;
    ReconcileChildren(database_catalog, items, root_handle);
    TreeView_Expand(tree_, root_handle, TVE_EXPAND);
    return;
  }

  std::wstring root_text(root_name);
  TVITEMW root{};
  root.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
  root.hItem = root_handle;
  root.pszText = root_text.data();
  root.lParam = item_data;
  root.iImage = root.iSelectedImage = image;
  TreeView_SetItem(tree_, &root);
  ReconcileChildren(database_catalog, items, root_handle);
}

HTREEITEM TreeViewController::FindTopLevelItem(LPARAM item_data) const {
  if (!tree_) return nullptr;
  for (auto item = TreeView_GetRoot(tree_); item; item = TreeView_GetNextSibling(tree_, item)) {
    if (ItemData(item) == item_data) return item;
  }
  return nullptr;
}

HTREEITEM TreeViewController::FindItemInBranch(std::wstring_view name, LPARAM branch_data) const {
  if (!tree_) return nullptr;
  if (branch_data == 0) return FindItemByName(TreeView_GetRoot(tree_), name);
  const HTREEITEM branch = FindTopLevelItem(branch_data);
  return branch ? FindItemByName(TreeView_GetChild(tree_, branch), name) : nullptr;
}

HTREEITEM TreeViewController::FindItemInBranch(LPARAM item_data, LPARAM branch_data) const {
  if (!tree_) return nullptr;
  if (branch_data == 0) return FindItemByData(TreeView_GetRoot(tree_), item_data);
  const HTREEITEM branch = FindTopLevelItem(branch_data);
  if (!branch) return nullptr;
  if (ItemData(branch) == item_data) return branch;
  return FindItemByData(TreeView_GetChild(tree_, branch), item_data);
}

HTREEITEM TreeViewController::FindItemByData(HTREEITEM item, LPARAM item_data) const {
  for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
    if (ItemData(current) == item_data) return current;
    if (const auto child = FindItemByData(TreeView_GetChild(tree_, current), item_data)) return child;
  }
  return nullptr;
}

HTREEITEM TreeViewController::FindItemByName(HTREEITEM item, std::wstring_view name) const {
  for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
    if (IsCatalogItemData(ItemData(current)) && EqualNoCase(ItemName(current), name)) return current;
    if (const auto child = FindItemByName(TreeView_GetChild(tree_, current), name)) return child;
  }
  return nullptr;
}

int TreeViewController::DatabaseImage(const domain::Entry* entry) {
  if (!entry) return kServerDatabaseImage;
  const auto connect = entry->ValueOr(L"Connect");
  if (catalog::Catalog::IsWebConnection(connect)) return kWebDatabaseImage;
  if (connection::Value(connect, L"File")) return kFileDatabaseImage;
  return kServerDatabaseImage;
}

}  // namespace ibstart::ui
