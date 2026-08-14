#pragma once

#include "core/catalog/catalog.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>

namespace ibstart::scanner {

struct ScanProgress { std::filesystem::path current; uintmax_t directories{}; uintmax_t found{}; };
struct ScanResult { std::filesystem::path directory; bool already_registered{false}; };
using ProgressCallback = std::function<void(const ScanProgress&)>;

[[nodiscard]] std::vector<ScanResult> FindFileBases(const std::vector<std::filesystem::path>& roots,
    const catalog::Catalog& catalog, std::atomic_bool& cancelled, ProgressCallback progress = {});

}  // namespace ibstart::scanner
