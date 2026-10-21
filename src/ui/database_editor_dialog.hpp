#pragma once

#include "core/domain/model.hpp"
#include "core/catalog/catalog_metadata_service.hpp"

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::ui::dialog {

enum class DatabaseConnectionKind { file, web, server };

struct DatabaseEditorData {
  std::wstring name;
  std::wstring connect;
  std::wstring id;
  std::wstring folder;
  std::wstring order_in_list;
  std::wstring order_in_tree;
  std::wstring version;
  std::wstring default_version;
  std::wstring app;
  std::wstring default_app;
  std::wstring wa;
  std::wstring external;
  std::wstring locale;
  std::wstring client_connection_speed;
  std::wstring app_arch;
  std::wstring additional_parameters;
  std::wstring configuration_id;
  std::wstring configuration_version;
  DatabaseConnectionKind kind{DatabaseConnectionKind::server};
  bool operator==(const DatabaseEditorData&) const = default;
};

[[nodiscard]] std::optional<DatabaseEditorData> EditDatabase(HWND owner, std::wstring_view title,
    DatabaseEditorData initial, const std::vector<domain::PlatformInstallation>& platforms,
    catalog::CatalogMetadataService* metadata = nullptr);
[[nodiscard]] DatabaseEditorData DatabaseEditorDataFromEntry(const domain::Entry& entry);
void ApplyDatabaseEditorData(domain::Entry& entry, const DatabaseEditorData& data);

}  // namespace ibstart::ui::dialog
