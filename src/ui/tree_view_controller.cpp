#include "ui/tree_view_controller.hpp"

#include "app/resource.h"
#include "core/connection/connection_string.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>

namespace ibstart::ui {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
          static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
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
}

void TreeViewController::Populate(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter, bool simple_mode) const {
  if (!tree_) return;

  // Keep existing HTREEITEM handles whenever possible.  Reconciliation is
  // important here: changing a filter or a tag should not destroy an
  // unrelated expanded branch just to recreate the same rows.
  const auto view_state = CaptureViewState();
  const bool can_suspend_drawing = IsWindow(tree_);
  if (can_suspend_drawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resume_drawing = [&] {
    if (!can_suspend_drawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };

  try {
    const auto catalog_items = database_catalog.Tree();
    HTREEITEM catalog_root = FindTopLevelItem(kCatalogRootItemData);
    if (!catalog_root) {
      catalog_root = InsertCatalogRoot();
      if (catalog_root) TreeView_Expand(tree_, catalog_root, TVE_EXPAND);
    }
    if (catalog_root) {
      ReconcileChildren(database_catalog, catalog_state, filter_favorites, catalog_items, catalog_root,
          search_filter, tag_filter);
    }

    if (simple_mode) {
      for (const LPARAM item_data : {kFavoritesRootItemData, kRecentRootItemData}) {
        if (const HTREEITEM root = FindTopLevelItem(item_data)) TreeView_DeleteItem(tree_, root);
      }
    } else {
      ReconcileSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter, tag_filter,
          L"Избранное", catalog_state.favorites, kFavoriteImage, kFavoritesRootItemData, catalog_root);
      const HTREEITEM favorites_root = FindTopLevelItem(kFavoritesRootItemData);
      ReconcileSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter, tag_filter,
          L"Недавние", presentation::CollectRecentDatabaseNames(database_catalog, catalog_state.history),
          kRecentImage, kRecentRootItemData, favorites_root ? favorites_root : catalog_root);
    }

    RestoreViewState(view_state);
  } catch (...) {
    resume_drawing();
    throw;
  }
  resume_drawing();
}

void TreeViewController::RefreshRecentBranch(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter,
    std::wstring_view selected_recent) const {
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
        FindTopLevelItem(kFavoritesRootItemData));
    RestoreViewState(view_state);
    if (!selected_recent.empty()) static_cast<void>(SelectItem(selected_recent));
  } catch (...) {
    resume_drawing();
    throw;
  }
  resume_drawing();
}

std::wstring TreeViewController::ItemName(HTREEITEM item) const {
  if (!tree_ || !item) return {};
  wchar_t text[512]{};
  TVITEMW data{};
  data.mask = TVIF_TEXT;
  data.hItem = item;
  data.pszText = text;
  data.cchTextMax = static_cast<int>(std::size(text));
  return TreeView_GetItem(tree_, &data) ? text : L"";
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
  return ItemData(selected) == 0 ? ItemName(selected) : L"";
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
  if (result.first_visible_data == 0) result.first_visible_name = ItemName(first_visible);
  return result;
}

void TreeViewController::RestoreViewState(const ViewState& state) const {
  if (!tree_) return;
  RestoreExpansionStates(state.expansion_states);
  bool selection_restored = state.selected_name.empty() && state.selected_item_data == 0;
  if (!state.selected_name.empty()) {
    if (const HTREEITEM item = FindItemInBranch(state.selected_name, state.selected_branch_data)) {
      TreeView_SelectItem(tree_, item);
      TreeView_EnsureVisible(tree_, item);
      selection_restored = true;
    }
    // EnsureVisible can expand ancestors of the selected row.  Reapply the
    // captured state so a refresh never changes a branch the user collapsed.
    RestoreExpansionStates(state.expansion_states);
  } else if (state.selected_item_data != 0) {
    if (const HTREEITEM item = FindTopLevelItem(state.selected_item_data)) {
      TreeView_SelectItem(tree_, item);
      TreeView_EnsureVisible(tree_, item);
      selection_restored = true;
    }
  }
  if (!selection_restored) TreeView_SelectItem(tree_, nullptr);

  // Keep the vertical viewport anchored to the same row where possible. This
  // prevents an update from looking like a jump in a large tree.
  if (!state.first_visible_name.empty()) {
    if (const HTREEITEM item = FindItemInBranch(state.first_visible_name, state.first_visible_branch_data)) {
      SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(item));
    }
  } else if (state.first_visible_data != 0) {
    if (const HTREEITEM item = FindTopLevelItem(state.first_visible_data)) {
      SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(item));
    }
  }
}

bool TreeViewController::MatchesFilters(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    const catalog::TreeItem& item, std::wstring_view search_filter,
    const presentation::TreeTagFilter& tag_filter) const {
  return presentation::MatchesSearchFilter(database_catalog, item, search_filter, catalog_state.tags) &&
      presentation::MatchesTagFilter(database_catalog, item, tag_filter, catalog_state.tags, filter_favorites);
}

void TreeViewController::UpdateTreeItem(HTREEITEM handle, const catalog::Catalog& database_catalog,
    const catalog::TreeItem& item) const {
  if (!tree_ || !handle) return;
  TVITEMW row{};
  row.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
  row.hItem = handle;
  row.pszText = const_cast<wchar_t*>(item.name.c_str());
  const auto* entry = database_catalog.Find(item.name);
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
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view search_filter,
    const presentation::TreeTagFilter& tag_filter) const {
  for (const auto& item : items) {
    if (!MatchesFilters(database_catalog, catalog_state, filter_favorites, item, search_filter, tag_filter)) continue;
    TVINSERTSTRUCTW row{};
    row.hParent = parent;
    row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    row.item.pszText = const_cast<wchar_t*>(item.name.c_str());
    const auto* entry = database_catalog.Find(item.name);
    row.item.iImage = row.item.iSelectedImage = item.database ? DatabaseImage(entry) : kFolderImage;
    const HTREEITEM handle = TreeView_InsertItem(tree_, &row);
    if (handle && !item.database) {
      AddItems(database_catalog, catalog_state, filter_favorites, item.children, handle, search_filter, tag_filter);
      if (!search_filter.empty()) TreeView_Expand(tree_, handle, TVE_EXPAND);
    }
  }
}

void TreeViewController::ReconcileChildren(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    const std::vector<catalog::TreeItem>& items, HTREEITEM parent,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter) const {
  if (!tree_ || !parent) return;

  std::vector<const catalog::TreeItem*> visible;
  visible.reserve(items.size());
  for (const auto& item : items) {
    if (MatchesFilters(database_catalog, catalog_state, filter_favorites, item, search_filter, tag_filter)) {
      visible.push_back(&item);
    }
  }

  std::vector<HTREEITEM> existing;
  for (HTREEITEM child = TreeView_GetChild(tree_, parent); child;
      child = TreeView_GetNextSibling(tree_, child)) {
    existing.push_back(child);
  }

  // TreeView has no move operation.  If an existing row changed relative
  // order, rebuild only this parent; additions and removals still reuse all
  // unaffected handles.
  std::vector<size_t> existing_positions;
  existing_positions.reserve(visible.size());
  for (const auto* item : visible) {
    for (size_t index = 0; index < existing.size(); ++index) {
      if (EqualNoCase(ItemName(existing[index]), item->name)) {
        existing_positions.push_back(index);
        break;
      }
    }
  }
  if (!std::is_sorted(existing_positions.begin(), existing_positions.end())) {
    DeleteChildren(parent);
    AddItems(database_catalog, catalog_state, filter_favorites, items, parent, search_filter, tag_filter);
    return;
  }

  std::vector<bool> used(existing.size(), false);
  HTREEITEM previous = nullptr;
  for (const auto* item : visible) {
    HTREEITEM handle = nullptr;
    size_t existing_index = existing.size();
    for (size_t index = 0; index < existing.size(); ++index) {
      if (!used[index] && EqualNoCase(ItemName(existing[index]), item->name)) {
        existing_index = index;
        handle = existing[index];
        break;
      }
    }
    if (!handle) {
      TVINSERTSTRUCTW row{};
      row.hParent = parent;
      row.hInsertAfter = previous ? previous : TVI_FIRST;
      row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
      row.item.pszText = const_cast<wchar_t*>(item->name.c_str());
      const auto* entry = database_catalog.Find(item->name);
      row.item.iImage = row.item.iSelectedImage = item->database ? DatabaseImage(entry) : kFolderImage;
      handle = TreeView_InsertItem(tree_, &row);
      if (!handle) continue;
    }
    if (existing_index < existing.size()) used[existing_index] = true;
    UpdateTreeItem(handle, database_catalog, *item);
    if (item->database) {
      DeleteChildren(handle);
    } else {
      ReconcileChildren(database_catalog, catalog_state, filter_favorites, item->children, handle,
          search_filter, tag_filter);
      if (!search_filter.empty()) TreeView_Expand(tree_, handle, TVE_EXPAND);
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
    HTREEITEM insert_after) const {
  if (!tree_) return;
  std::vector<catalog::TreeItem> items;
  items.reserve(names.size());
  for (const auto& name : names) {
    const auto* entry = database_catalog.Find(name);
    if (!entry || !entry->IsDatabase()) continue;
    catalog::TreeItem item{entry->name, true, {}, {}};
    if (MatchesFilters(database_catalog, catalog_state, filter_favorites, item, search_filter, tag_filter)) {
      items.push_back(std::move(item));
    }
  }

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
    ReconcileChildren(database_catalog, catalog_state, filter_favorites, items, root_handle,
        search_filter, tag_filter);
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
  ReconcileChildren(database_catalog, catalog_state, filter_favorites, items, root_handle,
      search_filter, tag_filter);
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

HTREEITEM TreeViewController::FindItemByName(HTREEITEM item, std::wstring_view name) const {
  for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
    if (ItemData(current) == 0 && EqualNoCase(ItemName(current), name)) return current;
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
