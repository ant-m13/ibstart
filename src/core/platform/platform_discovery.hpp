#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <vector>

namespace ibstart::platform {

[[nodiscard]] std::vector<std::filesystem::path> StandardSearchRoots();
[[nodiscard]] std::vector<domain::PlatformInstallation> Discover(const std::vector<std::filesystem::path>& user_roots = {});

}  // namespace ibstart::platform
