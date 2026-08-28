#include "core/catalog/catalog_session.hpp"

#include "core/domain/utf.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/windows_path.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace ibstart::catalog {

CatalogSession LoadSession(const std::filesystem::path& path,
    std::span<const std::filesystem::path> platform_search_paths) {
  if (!path.empty() && !windows_path::IsWithinLimit(path)) {
    throw std::invalid_argument(utf::ToUtf8(windows_path::LengthError(path)));
  }
  for (const auto& search_path : platform_search_paths) {
    if (!windows_path::IsWithinLimit(search_path)) {
      throw std::invalid_argument(utf::ToUtf8(windows_path::LengthError(search_path)));
    }
  }
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
