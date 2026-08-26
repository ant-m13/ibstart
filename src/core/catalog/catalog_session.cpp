#include "core/catalog/catalog_session.hpp"

#include "core/platform/platform_discovery.hpp"

#include <filesystem>
#include <utility>

namespace ibstart::catalog {

CatalogSession LoadSession(const std::filesystem::path& path,
    std::span<const std::filesystem::path> platform_search_paths) {
  CatalogSession session;
  session.platforms = platform::Discover(
      std::vector<std::filesystem::path>(platform_search_paths.begin(), platform_search_paths.end()));
  if (path.empty() || !std::filesystem::exists(path)) return session;

  session.store.emplace(path);
  session.catalog = Catalog(session.store->Read());
  session.loaded = true;
  return session;
}

}  // namespace ibstart::catalog
