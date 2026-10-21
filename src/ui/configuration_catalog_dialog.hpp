#pragma once

#include "core/catalog/catalog_metadata_service.hpp"
#include <Windows.h>
#include <optional>
#include <string>

namespace ibstart::ui::dialog {
[[nodiscard]] std::optional<std::wstring> EditConfigurations(HWND owner, catalog::CatalogMetadataService& metadata,
    bool select = false, std::wstring initial_id = {});
}
