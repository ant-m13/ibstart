#pragma once

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <vector>

#include "core/credentials/credentials.hpp"

namespace ibstart::catalog {
class Catalog;
}

namespace ibstart::ui::dialog {

[[nodiscard]] std::optional<std::vector<credentials::Credential>> EditCredentialManager(
    HWND owner, const std::vector<credentials::Credential>& records,
    const std::filesystem::path& catalog_path, const catalog::Catalog* catalog);

}  // namespace ibstart::ui::dialog
