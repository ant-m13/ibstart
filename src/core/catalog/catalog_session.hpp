#pragma once

#include "core/catalog/catalog.hpp"
#include "core/v8i/v8i_file_store.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ibstart::catalog {

// A loaded catalog together with the file store and platform installations
// required by the launch workflow.  An empty session represents a missing
// ibases.v8i file while retaining the discovered platform list.
struct CatalogSession {
  std::optional<v8i::V8iFileStore> store;
  Catalog catalog;
  std::vector<domain::PlatformInstallation> platforms;
  bool loaded{false};
};

[[nodiscard]] CatalogSession LoadSession(const std::filesystem::path& path,
    std::span<const std::filesystem::path> platform_search_paths);

}  // namespace ibstart::catalog
