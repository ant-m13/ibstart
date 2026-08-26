#pragma once

#include "core/catalog/catalog.hpp"
#include "core/catalog/catalog_metadata_service.hpp"
#include "core/logging/logging.hpp"
#include "core/storage/storage.hpp"
#include "core/v8i/v8i_file_store.hpp"
#include "ui/cache_clear_operation.hpp"
#include "ui/details_view_controller.hpp"
#include "ui/menu_controller.hpp"
#include "ui/owner_draw_menu.hpp"
#include "ui/tag_manager.hpp"
#include "ui/tree_view_controller.hpp"
#include "ui/update_check_operation.hpp"

#include <Windows.h>
#include <CommCtrl.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui {

namespace presentation {
struct TreeTagFilter;
}

class MainWindow {
 public:
  MainWindow(HINSTANCE instance, std::filesystem::path executable, storage::StorageLayout layout,
      storage::Settings settings, std::optional<std::wstring> launch_id = std::nullopt);
  ~MainWindow();
  int Show(int show_command);
  void Activate();

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateControls();
  void Layout(int width, int height);
  void LoadCatalog(bool report_error = true);
  bool SaveCatalog(catalog::Catalog candidate);
  void PopulateTree();
  void PopulateTreeWithoutFlicker(std::wstring_view selected = {}, bool select_catalog_root = false);
  void RefreshRecentTreeBranch(std::wstring_view selected_recent = {});
  void SortFolder(std::wstring_view folder, catalog::SortDirection direction);
  void ToggleFoldersFirstWhenSorting();
  void RefreshTagFilter();
  [[nodiscard]] presentation::TreeTagFilter CurrentTagFilter() const;
  LRESULT DrawTreeSearchMatches(NMTVCUSTOMDRAW* draw) const;
  bool MeasureContextMenuItem(MEASUREITEMSTRUCT* measure) const;
  bool DrawContextMenuItem(const DRAWITEMSTRUCT* draw) const;
  [[nodiscard]] const OwnerDrawMenuItem* FindMenuItem(ULONG_PTR item_data) const noexcept;
  void BeginTreeDrag(HTREEITEM item, POINT tree_point);
  void UpdateTreeDrag(POINT window_point);
  void EndTreeDrag(POINT window_point);
  void CancelTreeDrag();
  [[nodiscard]] std::optional<size_t> CatalogPosition(std::wstring_view name, std::wstring_view parent) const;
  void ShowTreeContextMenu(POINT screen);
  void ShowDetailsContextMenu(POINT screen);
  void CopySelectedDetail(bool include_name);
  void DisplaySelected();
  void LaunchSelected(domain::LaunchMode mode);
  void AddDatabase(std::wstring parent = {});
  void AddGroup(std::wstring parent = {});
  void EditSelected();
  void EditSelectedTags();
  void ConfigureTagColors();
  void AddTagToSelected(std::wstring tag);
  void AddNewTagToSelected();
  void ApplyTagResult(TagManager::Result result, std::wstring_view selected = {});
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
  bool ActivateCatalog(const std::filesystem::path& path);
  static void RememberRecentList(storage::Settings& settings, const std::filesystem::path& path);
  void RefreshFileMenu();
  void RefreshMainMenuBar();
  void ToggleTagDisplay();
  void SetStatus(std::wstring text);
  [[nodiscard]] std::wstring CatalogStatistics() const;
  void SetSimpleMode(bool enabled);
  void ToggleFavorite();
  void LaunchFavorite(size_t slot);
  void CheckForUpdates();
  void CompleteUpdateCheck();
  void CompleteCacheOperation();
  void BeginClose();
  void PollBackgroundOperations();
  [[nodiscard]] bool RefreshBackgroundPolling();
  void TryFinishClose();
  void StopAndJoinBackgroundThreads() noexcept;
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
  HIMAGELIST drag_image_{};
  std::vector<HIMAGELIST> button_images_;
  OwnerDrawMenuItems context_menu_items_;
  MenuController menus_;
  std::filesystem::path executable_;
  storage::StorageLayout layout_;
  storage::Settings settings_;
  catalog::CatalogMetadataService catalog_state_;
  logging::Logger logger_;
  TagManager tag_manager_;
  std::optional<v8i::V8iFileStore> store_;
  std::optional<catalog::Catalog> catalog_;
  std::vector<domain::PlatformInstallation> platforms_;
  std::vector<std::wstring> filter_tags_;
  std::vector<std::wstring> filter_favorites_;
  std::optional<std::wstring> initial_launch_id_;
  bool suppress_search_refresh_{false};
  TreeViewController tree_view_;
  DetailsViewController details_view_;
  background::UpdateCheckOperation update_check_;
  background::CacheClearOperation cache_operation_;
  std::wstring search_filter_;
  std::wstring dragging_name_;
  std::wstring drag_target_name_;
  bool drag_insert_after_{false};
  bool drag_to_root_{false};
  bool closing_{false};
};

}  // namespace ibstart::ui
