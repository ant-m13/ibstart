#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace ibstart::platform {

[[nodiscard]] std::vector<std::filesystem::path> StandardSearchRoots();
// Returns the thin-client executable for a platform executable only when the
// target is a regular, valid Windows executable file.
[[nodiscard]] std::optional<std::filesystem::path> FindThinClient(
    const std::filesystem::path& platform_executable);
[[nodiscard]] std::vector<domain::PlatformInstallation> Discover(
    const std::vector<std::filesystem::path>& user_roots = {}, bool include_system_sources = true);

}  // namespace ibstart::platform
