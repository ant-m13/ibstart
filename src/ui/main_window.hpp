#pragma once

#include "core/catalog/catalog.hpp"
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
  struct UpdateCheckState;

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateControls();
  void Layout(int width, int height);
  void LoadCatalog(bool report_error = true);
  void SaveCatalog();
  void PopulateTree();
  void AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter);
  [[nodiscard]] std::vector<catalog::TreeItem> SortedTree() const;
  void SortTreeItems(std::vector<catalog::TreeItem>& items, std::wstring_view parent) const;
  [[nodiscard]] storage::SortMode SortModeForFolder(std::wstring_view folder) const;
  void SetDefaultSortMode(storage::SortMode mode);
  void SetFolderSortMode(std::wstring_view folder, std::optional<storage::SortMode> mode);
  [[nodiscard]] bool ItemMatches(const catalog::TreeItem& item, std::wstring_view filter) const;
  [[nodiscard]] bool ItemMatchesTagFilter(const catalog::TreeItem& item) const;
  void RefreshTagFilter();
  void RefreshSortControl();
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
  void ShowTreeContextMenu(POINT screen);
  void ShowDetailsContextMenu(POINT screen);
  void CopySelectedDetail(bool include_name);
  void DisplaySelected();
  void LaunchSelected(domain::LaunchMode mode);
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
  void ShowUpdateCheckError();
  void ShowAbout() const;
  [[nodiscard]] std::wstring NextName(std::wstring_view stem) const;
  void ReportUnhandledError(std::string_view message) noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  HWND search_{};
  HWND tag_filter_label_{};
  HWND tag_filter_{};
  HWND sort_label_{};
  HWND sort_mode_{};
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
  logging::Logger logger_;
  std::optional<v8i::V8iFileStore> store_;
  std::optional<catalog::Catalog> catalog_;
  storage::DatabaseTags tags_;
  storage::TagStyles tag_styles_;
  storage::SortSettings sort_settings_;
  storage::LastLaunchTimes last_launches_;
  std::vector<domain::PlatformInstallation> platforms_;
  std::vector<std::wstring> filter_tags_;
  std::vector<std::wstring> filter_favorites_;
  std::optional<std::wstring> initial_launch_id_;
  std::shared_ptr<UpdateCheckState> update_check_;
  std::wstring search_filter_;
  std::wstring dragging_name_;
  std::wstring drag_target_name_;
  bool drag_insert_after_{false};
  bool drag_to_root_{false};
};

}  // namespace ibstart::ui
