#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <stop_token>
#include <vector>

namespace ibstart::cache {

struct CacheItem { std::filesystem::path path; uintmax_t bytes{}; };
struct ClearResult {
  uintmax_t files{};
  uintmax_t bytes{};
  bool active_one_c_process{};
  bool cancelled{};
  std::vector<std::wstring> errors;
};

struct ScanResult {
  std::vector<CacheItem> items;
  uintmax_t bytes{};
  bool complete{true};
  bool cancelled{};
  std::vector<std::wstring> errors;
};

// Scans only the cache locations derived from the database identifier.  A
// missing cache directory is a valid zero-sized result; an inaccessible item
// or a reparse point makes the result partial instead of silently producing an
// apparently exact value.
[[nodiscard]] ScanResult Scan(const domain::Database& database, std::stop_token stop = {});

// Returns no candidates when cancellation is requested. Callers that need to distinguish
// cancellation from an empty result should inspect the same stop token.
[[nodiscard]] std::vector<CacheItem> CandidatesFor(
    const domain::Database& database, std::stop_token stop = {});
[[nodiscard]] std::wstring FormatSize(uintmax_t bytes);
[[nodiscard]] bool HasActiveOneCProcess();
// Reparse points are never removed; a candidate containing one is rejected as a whole.
[[nodiscard]] ClearResult Clear(const std::vector<CacheItem>& candidates, std::stop_token stop = {});

}  // namespace ibstart::cache
