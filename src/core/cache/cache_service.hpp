#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <stop_token>
#include <vector>

namespace ibstart::cache {

struct CacheItem { std::filesystem::path path; uintmax_t bytes{}; };
struct ClearResult { uintmax_t files{}; uintmax_t bytes{}; std::vector<std::wstring> errors; };

// Returns no candidates when cancellation is requested. Callers that need to distinguish
// cancellation from an empty result should inspect the same stop token.
[[nodiscard]] std::vector<CacheItem> CandidatesFor(
    const domain::Database& database, std::stop_token stop = {});
[[nodiscard]] std::wstring FormatSize(uintmax_t bytes);
[[nodiscard]] bool HasActiveOneCProcess();
[[nodiscard]] ClearResult Clear(const std::vector<CacheItem>& candidates);

}  // namespace ibstart::cache
