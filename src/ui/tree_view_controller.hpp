#pragma once

#include "core/catalog/catalog.hpp"
#include "core/storage/storage.hpp"
#include "ui/tree_presentation.hpp"

#include <CommCtrl.h>
#include <Windows.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui {

class TreeViewController final {
 public:
  static constexpr LPARAM kRecentRootItemData = 1;
  static constexpr LPARAM kFavoritesRootItemData = 2;
  static constexpr LPARAM kCatalogRootItemData = 3;
  static constexpr std::wstring_view kCatalogRootName = L"Информационные базы";

  using ExpansionStates = std::map<std::wstring, bool>;

  TreeViewController() = default;
  ~TreeViewController();

  TreeViewController(const TreeViewController&) = delete;
  TreeViewController& operator=(const TreeViewController&) = delete;

  void Attach(HWND tree, HINSTANCE instance);
  void Clear() const;
  void Populate(const catalog::Catalog& database_catalog, const storage::CatalogState& catalog_state,
      const std::vector<std::wstring>& filter_favorites, std::wstring_view search_filter,
      const presentation::TreeTagFilter& tag_filter, bool simple_mode) const;
  void RefreshRecentBranch(const catalog::Catalog& database_catalog, const storage::CatalogState& catalog_state,
      const std::vector<std::wstring>& filter_favorites, std::wstring_view search_filter,
      const presentation::TreeTagFilter& tag_filter, std::wstring_view selected_recent) const;

  [[nodiscard]] std::wstring ItemName(HTREEITEM item) const;
  [[nodiscard]] LPARAM ItemData(HTREEITEM item) const;
  [[nodiscard]] bool IsVirtualBranch(HTREEITEM item) const;
  [[nodiscard]] LPARAM BranchData(HTREEITEM item) const;
  [[nodiscard]] std::wstring SelectedName() const;
  [[nodiscard]] std::optional<size_t> SectionIndex(HTREEITEM item) const;
  [[nodiscard]] std::optional<size_t> SelectedSectionIndex() const;
  [[nodiscard]] bool SelectedItemIsRecentRoot() const;
  [[nodiscard]] bool SelectItem(std::wstring_view name) const;
  [[nodiscard]] bool SelectCatalogRoot() const;
  [[nodiscard]] ExpansionStates CaptureExpansionStates() const;
  void RestoreExpansionStates(const ExpansionStates& states) const;

 private:
  struct ViewState {
    ExpansionStates expansion_states;
    std::wstring selected_name;
    LPARAM selected_item_data{};
    LPARAM selected_branch_data{};
    std::wstring first_visible_name;
    LPARAM first_visible_data{};
    LPARAM first_visible_branch_data{};
  };

  enum TreeImage : int {
    kFileDatabaseImage,
    kServerDatabaseImage,
    kWebDatabaseImage,
    kFolderImage,
    kFavoriteImage,
    kRecentImage,
  };

  void AddItems(const catalog::Catalog& database_catalog, const std::vector<catalog::TreeItem>& items,
      HTREEITEM parent, bool expand_for_search) const;
  void ReconcileChildren(const catalog::Catalog& database_catalog,
      const std::vector<catalog::TreeItem>& items, HTREEITEM parent, bool expand_for_search) const;
  void ReconcileSpecialRoot(const catalog::Catalog& database_catalog,
      const storage::CatalogState& catalog_state, const std::vector<std::wstring>& filter_favorites,
      std::wstring_view search_filter, const presentation::TreeTagFilter& tag_filter,
      std::wstring_view root_name, const std::vector<std::wstring>& names, int image, LPARAM item_data,
      HTREEITEM insert_after) const;
  [[nodiscard]] HTREEITEM InsertCatalogRoot() const;
  [[nodiscard]] ViewState CaptureViewState() const;
  void RestoreViewState(const ViewState& state) const;
  void UpdateTreeItem(HTREEITEM handle, const catalog::Catalog& database_catalog,
      const catalog::TreeItem& item) const;
  void DeleteChildren(HTREEITEM parent) const;
  [[nodiscard]] HTREEITEM FindTopLevelItem(LPARAM item_data) const;
  [[nodiscard]] HTREEITEM FindItemInBranch(std::wstring_view name, LPARAM branch_data) const;
  [[nodiscard]] HTREEITEM FindItemByName(HTREEITEM item, std::wstring_view name) const;
  [[nodiscard]] static int DatabaseImage(const domain::Entry* entry);

  HWND tree_{};
  HIMAGELIST images_{};
};

}  // namespace ibstart::ui
