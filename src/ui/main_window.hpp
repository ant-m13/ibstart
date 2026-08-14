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
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateControls();
  void Layout(int width, int height);
  void LoadCatalog(bool report_error = true);
  void SaveCatalog();
  void PopulateTree();
  void AddTreeItems(const std::vector<catalog::TreeItem>& items, HTREEITEM parent, std::wstring_view filter);
  [[nodiscard]] bool ItemMatches(const catalog::TreeItem& item, std::wstring_view filter) const;
  [[nodiscard]] std::wstring SelectedName() const;
  void DisplaySelected();
  void LaunchSelected(domain::LaunchMode mode);
  void AddFileDatabase();
  void AddServerDatabase();
  void AddGroup();
  void EditSelected();
  void DeleteSelected();
  void ClearSelectedCache();
  void CreateShortcut();
  void OpenList();
  void SetStatus(std::wstring text);
  [[nodiscard]] std::wstring CatalogStatistics() const;
  void SetSimpleMode(bool enabled);
  void ToggleFavorite();
  void LaunchFavorite(size_t slot);
  void ShowAbout() const;
  [[nodiscard]] std::wstring NextName(std::wstring_view stem) const;
  void ReportUnhandledError(std::string_view message) noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  HWND search_{};
  HWND tree_{};
  HWND details_{};
  HWND status_{};
  HWND enterprise_{};
  HWND designer_{};
  HWND edit_{};
  HWND cache_{};
  HWND shortcut_{};
  HWND remove_{};
  std::filesystem::path executable_;
  storage::StorageLayout layout_;
  storage::Settings settings_;
  logging::Logger logger_;
  std::optional<v8i::V8iFileStore> store_;
  std::optional<catalog::Catalog> catalog_;
  std::vector<domain::PlatformInstallation> platforms_;
  std::optional<std::wstring> initial_launch_id_;
  std::wstring dragging_name_;
};

}  // namespace ibstart::ui
