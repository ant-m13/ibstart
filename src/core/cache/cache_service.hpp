#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <vector>

namespace ibstart::cache {

struct CacheItem { std::filesystem::path path; uintmax_t bytes{}; };
struct ClearResult { uintmax_t files{}; uintmax_t bytes{}; std::vector<std::wstring> errors; };

[[nodiscard]] std::vector<CacheItem> CandidatesFor(const domain::Database& database);
[[nodiscard]] bool HasActiveOneCProcess();
[[nodiscard]] ClearResult Clear(const std::vector<CacheItem>& candidates);

}  // namespace ibstart::cache
