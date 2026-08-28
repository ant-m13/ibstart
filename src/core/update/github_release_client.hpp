#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ibstart::update::transport {

struct VersionAssetEndpoint {
  std::wstring_view host;
  std::uint16_t port{443};
  std::wstring_view path;
  bool secure{true};
  bool use_default_proxy{true};
};

// Downloads a plain-text version asset from the supplied endpoint. The
// endpoint form keeps the transport independently testable without GitHub.
[[nodiscard]] std::optional<std::string> FetchVersionAsset(
    const VersionAssetEndpoint& endpoint, std::stop_token stop = {});

// Downloads the plain-text version asset of the latest stable GitHub Release.
// A missing release/asset or cooperative cancellation is represented by nullopt;
// transport and HTTP errors throw std::runtime_error.
[[nodiscard]] std::optional<std::string> FetchLatestVersionAsset(std::stop_token stop = {});

}  // namespace ibstart::update::transport
