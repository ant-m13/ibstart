#include "core/scanner/file_base_scanner.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <set>

namespace ibstart::scanner {
namespace {
bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::wstring Normalized(std::filesystem::path path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error).wstring();
  if (error) normalized = path.lexically_normal().wstring();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  return normalized;
}

std::wstring Trim(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

std::wstring ConnectionValue(std::wstring_view connect, std::wstring_view key) {
  size_t start = 0;
  bool quoted = false;
  for (size_t index = 0; index <= connect.size(); ++index) {
    const wchar_t character = index < connect.size() ? connect[index] : L';';
    if (character == L'"') quoted = !quoted;
    if (character != L';' || quoted) continue;
    const auto field = connect.substr(start, index - start);
    const size_t separator = field.find(L'=');
    if (separator != std::wstring_view::npos && EqualNoCase(Trim(field.substr(0, separator)), key)) {
      auto value = Trim(field.substr(separator + 1));
      if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
      return value;
    }
    start = index + 1;
  }
  return {};
}
}  // namespace

std::vector<ScanResult> FindFileBases(const std::vector<std::filesystem::path>& roots,
    const catalog::Catalog& catalog, std::atomic_bool& cancelled, ProgressCallback progress) {
  std::vector<std::wstring> registered;
  for (const auto* entry : catalog.Databases()) {
    const auto file = ConnectionValue(entry->ValueOr(L"Connect"), L"File");
    if (!file.empty()) registered.push_back(Normalized(file));
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
