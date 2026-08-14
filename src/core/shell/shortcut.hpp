#pragma once

#include <filesystem>
#include <string>

namespace ibstart::shell {

void CreateDesktopShortcut(const std::filesystem::path& executable, std::wstring_view database_id, std::wstring_view display_name);

}  // namespace ibstart::shell
