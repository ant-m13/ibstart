#include "ui/tree_view_controller.hpp"

#include "app/resource.h"
#include "core/connection/connection_string.hpp"

#include <initializer_list>
#include <iterator>

namespace ibstart::ui {
namespace {

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

  // Rebuilding the native control destroys every HTREEITEM, including its
  // expansion and selection state.  Keep the old state while the replacement
  // hierarchy is created and suppress intermediate paints so users only see
  // the final tree.
  const auto expansion_states = CaptureExpansionStates();
  const HTREEITEM previous_selection = TreeView_GetSelection(tree_);
  const LPARAM selected_item_data = ItemData(previous_selection);
  const std::wstring selected_name = SelectedName();
  const HTREEITEM previous_first_visible =
      TreeView_GetNextItem(tree_, nullptr, TVGN_FIRSTVISIBLE);
  const LPARAM first_visible_data = ItemData(previous_first_visible);
  const std::wstring first_visible_name = first_visible_data == 0
      ? ItemName(previous_first_visible)
      : std::wstring();
  const bool can_suspend_drawing = IsWindow(tree_);
  if (can_suspend_drawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resume_drawing = [&] {
    if (!can_suspend_drawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };

  try {
    TreeView_DeleteAllItems(tree_);

    std::wstring catalog_root_name(kCatalogRootName);
    TVINSERTSTRUCTW catalog_root{};
    catalog_root.hParent = TVI_ROOT;
    catalog_root.hInsertAfter = TVI_LAST;
    catalog_root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_STATE;
    catalog_root.item.pszText = catalog_root_name.data();
    catalog_root.item.iImage = catalog_root.item.iSelectedImage = kFolderImage;
    catalog_root.item.lParam = kCatalogRootItemData;
    catalog_root.item.stateMask = TVIS_EXPANDED;
    catalog_root.item.state = TVIS_EXPANDED;
    const HTREEITEM catalog_root_handle = TreeView_InsertItem(tree_, &catalog_root);
    AddItems(database_catalog, catalog_state, filter_favorites, database_catalog.Tree(), catalog_root_handle,
        search_filter, tag_filter);
    if (catalog_root_handle) TreeView_Expand(tree_, catalog_root_handle, TVE_EXPAND);

    if (!simple_mode) {
      static_cast<void>(InsertSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter,
          tag_filter, L"Избранное", catalog_state.favorites, kFavoriteImage, kFavoritesRootItemData));
      const auto recent = presentation::CollectRecentDatabaseNames(database_catalog, catalog_state.history);
      static_cast<void>(InsertSpecialRoot(database_catalog, catalog_state, filter_favorites, search_filter,
          tag_filter, L"Недавние", recent, kRecentImage, kRecentRootItemData));
    }

    RestoreExpansionStates(expansion_states);
    if (!selected_name.empty()) {
      static_cast<void>(SelectItem(selected_name));
      // EnsureVisible can expand ancestors of the selected row.  Reapply the
      // captured state so a refresh never changes a branch the user collapsed.
      RestoreExpansionStates(expansion_states);
    } else if (selected_item_data != 0) {
      if (const HTREEITEM item = FindTopLevelItem(selected_item_data)) {
        TreeView_SelectItem(tree_, item);
        TreeView_EnsureVisible(tree_, item);
      }
    }

    // Keep the vertical viewport anchored to the same row where possible.
    // This prevents a refresh from appearing as a jump even when the tree is
    // too large to fit in the control.
    if (!first_visible_name.empty()) {
      if (const HTREEITEM item = FindItemByName(TreeView_GetRoot(tree_), first_visible_name)) {
        SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(item));
      }
    } else if (first_visible_data != 0) {
      if (const HTREEITEM item = FindTopLevelItem(first_visible_data)) {
        SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(item));
      }
    }
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
  const HTREEITEM previous_root = FindTopLevelItem(kRecentRootItemData);
  const bool previous_expanded = !previous_root ||
      (TreeView_GetItemState(tree_, previous_root, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
  const HTREEITEM first_visible = TreeView_GetNextItem(tree_, nullptr, TVGN_FIRSTVISIBLE);
  const bool restore_first_visible = first_visible &&
      BranchData(first_visible) != kRecentRootItemData && selected_recent.empty();
  const bool can_suspend_drawing = IsWindow(tree_);
  if (can_suspend_drawing) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
  const auto resume_drawing = [&] {
    if (!can_suspend_drawing) return;
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(tree_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  };
  try {
    // Build the replacement before deleting the current branch so the tree's
    // visible row count does not collapse and reset its vertical viewport.
    const HTREEITEM replacement_root = InsertSpecialRoot(database_catalog, catalog_state, filter_favorites,
        search_filter, tag_filter, L"Недавние", recent, kRecentImage, kRecentRootItemData);
    if (!replacement_root) {
      resume_drawing();
      return;
    }

    const HTREEITEM replacement_selection = selected_recent.empty() ? nullptr :
        FindItemByName(TreeView_GetChild(tree_, replacement_root), selected_recent);
    if (!selected_recent.empty() && !replacement_selection) {
      TreeView_DeleteItem(tree_, replacement_root);
      resume_drawing();
      return;
    }
    if (!previous_expanded && !replacement_selection) TreeView_Expand(tree_, replacement_root, TVE_COLLAPSE);
    if (replacement_selection) TreeView_SelectItem(tree_, replacement_selection);
    if (previous_root) TreeView_DeleteItem(tree_, previous_root);
    if (replacement_selection) TreeView_EnsureVisible(tree_, replacement_selection);
    else if (restore_first_visible) {
      SendMessageW(tree_, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, reinterpret_cast<LPARAM>(first_visible));
    }
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

void TreeViewController::AddItems(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view search_filter,
    const presentation::TreeTagFilter& tag_filter) const {
  for (const auto& item : items) {
    if (!presentation::MatchesSearchFilter(database_catalog, item, search_filter, catalog_state.tags) ||
        !presentation::MatchesTagFilter(
            database_catalog, item, tag_filter, catalog_state.tags, filter_favorites)) continue;
    TVINSERTSTRUCTW row{};
    row.hParent = parent;
    row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    row.item.pszText = const_cast<wchar_t*>(item.name.c_str());
    const auto* entry = database_catalog.Find(item.name);
    row.item.iImage = row.item.iSelectedImage = item.database ? DatabaseImage(entry) : kFolderImage;
    const HTREEITEM handle = TreeView_InsertItem(tree_, &row);
    if (!item.database) {
      AddItems(database_catalog, catalog_state, filter_favorites, item.children, handle, search_filter, tag_filter);
      if (!search_filter.empty()) TreeView_Expand(tree_, handle, TVE_EXPAND);
    }
  }
}

HTREEITEM TreeViewController::InsertSpecialRoot(const catalog::Catalog& database_catalog,
    const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
    std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter,
    std::wstring_view root_name, const std::vector<std::wstring>& names, int image, LPARAM item_data) const {
  std::wstring root_text(root_name);
  TVINSERTSTRUCTW root{};
  root.hParent = TVI_ROOT;
  root.hInsertAfter = TVI_LAST;
  root.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM;
  root.item.pszText = root_text.data();
  root.item.lParam = item_data;
  root.item.iImage = root.item.iSelectedImage = image;
  const HTREEITEM root_handle = TreeView_InsertItem(tree_, &root);
  if (!root_handle) return nullptr;

  bool any = false;
  for (const auto& name : names) {
    const auto* entry = database_catalog.Find(name);
    const catalog::TreeItem item{name, true, {}, {}};
    if (!entry || !entry->IsDatabase() ||
        !presentation::MatchesSearchFilter(database_catalog, item, search_filter, catalog_state.tags) ||
        !presentation::MatchesTagFilter(
            database_catalog, item, tag_filter, catalog_state.tags, filter_favorites)) continue;
    TVINSERTSTRUCTW row{};
    row.hParent = root_handle;
    row.hInsertAfter = TVI_LAST;
    row.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    row.item.pszText = const_cast<wchar_t*>(entry->name.c_str());
    row.item.iImage = row.item.iSelectedImage = DatabaseImage(entry);
    if (TreeView_InsertItem(tree_, &row)) any = true;
  }
  if (any) {
    TreeView_Expand(tree_, root_handle, TVE_EXPAND);
    return root_handle;
  }
  TreeView_DeleteItem(tree_, root_handle);
  return nullptr;
}

HTREEITEM TreeViewController::FindTopLevelItem(LPARAM item_data) const {
  if (!tree_) return nullptr;
  for (auto item = TreeView_GetRoot(tree_); item; item = TreeView_GetNextSibling(tree_, item)) {
    if (ItemData(item) == item_data) return item;
  }
  return nullptr;
}

HTREEITEM TreeViewController::FindItemByName(HTREEITEM item, std::wstring_view name) const {
  for (auto current = item; current; current = TreeView_GetNextSibling(tree_, current)) {
    if (ItemData(current) == 0 && ItemName(current) == name) return current;
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
