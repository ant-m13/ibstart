#pragma once

#include "core/storage/storage.hpp"

#include <Windows.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace ibstart::ui::dialog {

struct ApplicationSettingsResult {
  storage::Settings settings;
  bool reset_window_layout{false};
};

using CredentialsEditor = std::function<std::optional<std::vector<credentials::Credential>>(
    HWND, const std::vector<credentials::Credential>&)>;
using TagsEditor = std::function<void(HWND)>;
using RecentBasesCleaner = std::function<void(HWND)>;
using ProfileAction = std::function<bool(HWND)>;

[[nodiscard]] std::optional<ApplicationSettingsResult> EditApplicationSettings(
    HWND owner, storage::Settings initial, std::filesystem::path profile_root, bool portable,
    CredentialsEditor edit_credentials, TagsEditor edit_tags, RecentBasesCleaner clear_recent_bases,
    ProfileAction export_profile, ProfileAction import_profile);

}  // namespace ibstart::ui::dialog
