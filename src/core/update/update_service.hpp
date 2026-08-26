#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ibstart::update {

struct Release {
  std::wstring version;
  std::wstring page_url;
};

// Returns a negative value when left is older than right, zero when the versions are equal,
// and a positive value when left is newer. Both versions must be SemVer strings, optionally
// prefixed with a single 'v'.
[[nodiscard]] int CompareVersions(std::wstring_view left, std::wstring_view right);

// Parses the plain-text IBStart.version asset published with a GitHub Release.
[[nodiscard]] Release ParseLatestVersionFile(std::string_view response);

// Downloads the version asset of the latest published stable GitHub Release. A missing
// stable release, missing asset, or requested cancellation is reported as std::nullopt;
// transport and malformed-response errors throw.
[[nodiscard]] std::optional<Release> FetchLatestRelease(std::stop_token stop = {});

}  // namespace ibstart::update
