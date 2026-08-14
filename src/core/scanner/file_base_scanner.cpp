#include "core/scanner/file_base_scanner.hpp"

#include <algorithm>
#include <cwctype>
#include <set>

namespace ibstart::scanner {
namespace {
std::wstring Normalized(std::filesystem::path path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error).wstring();
  if (error) normalized = path.lexically_normal().wstring();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return normalized;
}
}

std::vector<ScanResult> FindFileBases(const std::vector<std::filesystem::path>& roots,
    const catalog::Catalog& catalog, std::atomic_bool& cancelled, ProgressCallback progress) {
  std::vector<std::wstring> registered;
  for (const auto* entry : catalog.Databases()) {
    const auto connect = entry->ValueOr(L"Connect");
    std::wstring normalizedConnect = connect;
    std::transform(normalizedConnect.begin(), normalizedConnect.end(), normalizedConnect.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    const auto key = normalizedConnect.find(L"file=\"");
    if (key != std::wstring::npos) {
      const auto start = key + 6; const auto end = connect.find(L'"', start);
      if (end != std::wstring::npos) registered.push_back(Normalized(connect.substr(start, end - start)));
    }
  }
  std::vector<ScanResult> found;
  std::set<std::wstring> knownResults;
  ScanProgress state;
  const auto addResult = [&](const std::filesystem::path& directory) {
    const auto normalized = Normalized(directory);
    if (!knownResults.insert(normalized).second) return;
    found.push_back({directory, std::find(registered.begin(), registered.end(), normalized) != registered.end()});
    ++state.found;
  };
  for (const auto& root : roots) {
    if (cancelled.load()) break;
    std::error_code error;
    if (std::filesystem::is_regular_file(root / L"1Cv8.1CD", error)) addResult(root);
    error.clear();
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end; it != end && !cancelled.load(); it.increment(error)) {
      if (error) { error.clear(); continue; }
      if (!it->is_directory(error)) { error.clear(); continue; }
      ++state.directories; state.current = it->path();
      if (std::filesystem::is_regular_file(it->path() / L"1Cv8.1CD", error)) {
        addResult(it->path());
        it.disable_recursion_pending();
      }
      error.clear();
      if (progress && state.directories % 32 == 0) progress(state);
    }
  }
  if (progress) progress(state);
  return found;
}

}  // namespace ibstart::scanner
