#pragma once

#include <optional>
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

// Parses the fields of the GitHub "latest release" response that IBStart uses.
[[nodiscard]] Release ParseLatestReleaseResponse(std::string_view response);

// Requests the latest published stable release for the IBStart GitHub repository. A missing
// stable release is reported as std::nullopt; transport and malformed-response errors throw.
[[nodiscard]] std::optional<Release> FetchLatestRelease();

}  // namespace ibstart::update
