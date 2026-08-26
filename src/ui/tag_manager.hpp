#pragma once

#include "core/catalog/catalog_metadata_service.hpp"
#include "core/domain/model.hpp"
#include "core/logging/logging.hpp"

#include <Windows.h>

#include <string>

namespace ibstart::ui {

class TagManager final {
 public:
  struct Result {
    bool changed{false};
    std::wstring status;
  };

  TagManager(catalog::CatalogMetadataService& catalog_metadata, logging::Logger& logger) noexcept;

  [[nodiscard]] Result EditAssignment(HWND owner, const domain::Entry* entry);
  [[nodiscard]] Result Configure(HWND owner);
  [[nodiscard]] Result AddTag(HWND owner, const domain::Entry* entry, std::wstring tag);
  [[nodiscard]] Result AddNewTag(HWND owner, const domain::Entry* entry);

 private:
  catalog::CatalogMetadataService& catalog_metadata_;
  logging::Logger& logger_;
};

}  // namespace ibstart::ui
