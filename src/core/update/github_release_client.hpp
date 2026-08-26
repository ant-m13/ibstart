#pragma once

#include <optional>
#include <stop_token>
#include <string>

namespace ibstart::update::transport {

// Downloads the plain-text version asset of the latest stable GitHub Release.
// A missing release/asset or cooperative cancellation is represented by nullopt;
// transport and HTTP errors throw std::runtime_error.
[[nodiscard]] std::optional<std::string> FetchLatestVersionAsset(std::stop_token stop = {});

}  // namespace ibstart::update::transport
