#include "core/scanner/file_base_scanner.hpp"

#include <algorithm>

namespace ibstart::scanner {
namespace {
std::wstring Normalized(std::filesystem::path path) {
  std::error_code error;
  return std::filesystem::weakly_canonical(path, error).wstring();
}
}

std::vector<ScanResult> FindFileBases(const std::vector<std::filesystem::path>& roots,
    const catalog::Catalog& catalog, std::atomic_bool& cancelled, ProgressCallback progress) {
  std::vector<std::wstring> registered;
  for (const auto* entry : catalog.Databases()) {
    const auto connect = entry->ValueOr(L"Connect");
    const auto key = connect.find(L"File=\"");
    if (key != std::wstring::npos) {
      const auto start = key + 6; const auto end = connect.find(L'"', start);
      if (end != std::wstring::npos) registered.push_back(Normalized(connect.substr(start, end - start)));
    }
  }
  std::vector<ScanResult> found;
  ScanProgress state;
  for (const auto& root : roots) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end; it != end && !cancelled.load(); it.increment(error)) {
      if (error) { error.clear(); continue; }
      if (!it->is_directory(error)) continue;
      ++state.directories; state.current = it->path();
      if (std::filesystem::is_regular_file(it->path() / L"1Cv8.1CD", error)) {
        const auto directory = it->path();
        const auto normalized = Normalized(directory);
        found.push_back({directory, std::find(registered.begin(), registered.end(), normalized) != registered.end()});
        ++state.found; it.disable_recursion_pending();
      }
      if (progress && state.directories % 32 == 0) progress(state);
    }
  }
  if (progress) progress(state);
  return found;
}

}  // namespace ibstart::scanner
