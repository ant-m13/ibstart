#pragma once

#include "core/catalog/catalog.hpp"
#include "core/catalog/catalog_metadata_service.hpp"
#include "core/logging/logging.hpp"
#include "core/storage/storage.hpp"
#include "core/v8i/v8i_file_store.hpp"

#include <Windows.h>
#include <CommCtrl.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui {

class MainWindow {
 public:
  MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
      storage::Settings settings, std::optional<std::wstring> launch_id = std::nullopt);
  ~MainWindow();
  int Show(int show_command);
  void Activate();

 private:
  struct ContextMenuItem {
    UINT command{};
    HICON icon{};
    std::wstring text;
    std::wstring shortcut;
  };
  enum class NewDatabaseKind { file, server };
  struct UpdateCheckState;
  struct CacheOperationState;

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateControls();
  void Layout(int width, int height);
  void LoadCatalog(bool report_error = true);
  bool SaveCatalog();
  void PopulateTree();
  void PopulateTreeWithoutFlicker(std::wstring_view selected = {}, bool select_catalog_root = false);
  void AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter);
  void SortFolder(std::wstring_view folder, catalog::SortDirection direction);
  void ToggleFoldersFirstWhenSorting();
  [[nodiscard]] bool ItemMatches(const catalog::TreeItem& item, std::wstring_view filter) const;
  [[nodiscard]] bool ItemMatchesTagFilter(const catalog::TreeItem& item) const;
  void RefreshTagFilter();
  LRESULT DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const;
  LRESULT DrawDetailsList(NMLVCUSTOMDRAW* draw) const;
  bool MeasureContextMenuItem(MEASUREITEMSTRUCT* measure) const;
  bool DrawContextMenuItem(const DRAWITEMSTRUCT* draw) const;
  void ClearContextMenuItems() noexcept;
  void ClearMainMenuItems() noexcept;
  [[nodiscard]] std::wstring SelectedName() const;
  [[nodiscard]] bool SelectedItemIsRecentRoot() const;
  void BeginTreeDrag(HTREEITEM item, POINT tree_point);
  void UpdateTreeDrag(POINT window_point);
  void EndTreeDrag(POINT window_point);
  void CancelTreeDrag();
  [[nodiscard]] std::optional<size_t> CatalogPosition(std::wstring_view name, std::wstring_view parent) const;
  bool SelectTreeItem(std::wstring_view name);
  bool SelectCatalogRoot();
  void ShowTreeContextMenu(POINT screen);
  void ShowDetailsContextMenu(POINT screen);
  void CopySelectedDetail(bool include_name);
  void DisplaySelected();
  void LaunchSelected(domain::LaunchMode mode);
  void AddDatabase(NewDatabaseKind kind, std::wstring parent);
  void AddFileDatabase(std::wstring parent = {});
  void AddServerDatabase(std::wstring parent = {});
  void AddGroup(std::wstring parent = {});
  void EditSelected();
  void EditSelectedTags();
  void ConfigureTagColors();
  void AddTagToSelected(std::wstring tag);
  void AddNewTagToSelected();
  void DeleteSelected();
  void MoveSelected(int offset);
  void MoveSelectedToFolder();
  void ClearSelectedCache();
  [[nodiscard]] bool IsClearingCache() const;
  void ClearRecentBases();
  void CreateShortcut();
  void OpenSelectedFolder();
  void OpenList();
  void OpenStandardList();
  void OpenRecentList(size_t index);
  void RememberRecentList(const std::filesystem::path& path);
  void RefreshFileMenu();
  void RefreshMainMenuBar();
  void ToggleTagDisplay();
  void UpdateConnection();
  void SetStatus(std::wstring text);
  [[nodiscard]] std::wstring CatalogStatistics() const;
  void SetSimpleMode(bool enabled);
  void ToggleFavorite();
  void LaunchFavorite(size_t slot);
  void CheckForUpdates();
  void CompleteUpdateCheck();
  void CompleteCacheOperation();
  void ShowUpdateCheckError();
  void ShowAbout() const;
  [[nodiscard]] std::wstring NextName(std::wstring_view stem) const;
  void ReportUnhandledError(std::string_view message) noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  HWND search_{};
  HWND tag_filter_label_{};
  HWND tag_filter_{};
  HWND tree_{};
  HWND details_title_{};
  HWND details_subtitle_{};
  HWND details_{};
  HWND connection_{};
  HWND status_{};
  HWND enterprise_{};
  HWND designer_{};
  HWND edit_{};
  HWND cache_{};
  HWND shortcut_{};
  HWND remove_{};
  HFONT controls_font_{};
  HFONT button_font_{};
  HFONT details_title_font_{};
  HFONT details_subtitle_font_{};
  HFONT details_key_font_{};
  HIMAGELIST tree_images_{};
  HIMAGELIST drag_image_{};
  HMENU menu_{};
  HMENU file_menu_{};
  HMENU view_menu_{};
  HMENU help_menu_{};
  std::vector<HIMAGELIST> button_images_;
  std::vector<ContextMenuItem> context_menu_items_;
  std::vector<ContextMenuItem> main_menu_items_;
  std::vector<ContextMenuItem> file_menu_items_;
  std::filesystem::path executable_;
  storage::StorageLayout layout_;
  storage::Settings settings_;
  catalog::CatalogMetadataService catalog_state_;
  logging::Logger logger_;
  std::optional<v8i::V8iFileStore> store_;
  std::optional<catalog::Catalog> catalog_;
  storage::DatabaseTags tags_;
  storage::TagStyles tag_styles_;
  std::vector<domain::PlatformInstallation> platforms_;
  std::vector<std::wstring> filter_tags_;
  std::vector<std::wstring> filter_favorites_;
  std::optional<std::wstring> initial_launch_id_;
  bool suppress_search_refresh_{false};
  std::shared_ptr<UpdateCheckState> update_check_;
  std::shared_ptr<CacheOperationState> cache_operation_;
  std::wstring search_filter_;
  std::wstring dragging_name_;
  std::wstring drag_target_name_;
  bool drag_insert_after_{false};
  bool drag_to_root_{false};
};

}  // namespace ibstart::ui
