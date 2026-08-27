#pragma once

#include "core/catalog/catalog.hpp"
#include "core/catalog/catalog_metadata_service.hpp"
#include "core/storage/storage.hpp"

#include <CommCtrl.h>
#include <Windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ibstart::ui {

class DetailsViewController final {
 public:
  struct Controls {
    HWND title{};
    HWND subtitle{};
    HWND details{};
    HWND connection{};
    HWND enterprise{};
    HWND designer{};
    HWND edit{};
    HWND cache{};
    HWND shortcut{};
    HWND remove{};
  };

  void Attach(Controls controls, HFONT key_font) noexcept;
  void Display(const catalog::Catalog* database_catalog,
      const catalog::CatalogMetadataService* catalog_metadata, std::wstring_view selected_name,
      std::optional<size_t> selected_section_index, bool catalog_root_selected, bool simple_mode,
      bool cache_operation_active) const;
  [[nodiscard]] LRESULT Draw(NMLVCUSTOMDRAW* draw, const storage::TagStyles& tag_styles) const;
  [[nodiscard]] std::wstring Text(int row, int column) const;

 private:
  void UpdateConnection(const domain::Entry* entry) const;

  Controls controls_;
  HFONT key_font_{};
};

}  // namespace ibstart::ui
