#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace ibstart::storage {
namespace {

std::wstring Env(std::wstring_view name) {
  const DWORD size = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
  if (!size) return {};
  std::wstring result(size, L'\0');
  if (GetEnvironmentVariableW(std::wstring(name).c_str(), result.data(), size) == 0) return {};
  result.resize(size - 1);
  return result;
}

}  // namespace

StorageLayout ResolveLayout(const std::filesystem::path& executable_path) {
  const auto marker = executable_path.parent_path() / L"IBStart.portable";
  std::error_code error;
  if (std::filesystem::is_regular_file(marker, error) && std::filesystem::file_size(marker, error) == 0) {
    return {executable_path.parent_path() / L"data", true};
  }
  const auto local = Env(L"LOCALAPPDATA");
  if (local.empty()) throw std::runtime_error("LOCALAPPDATA is unavailable; cannot determine IBStart data directory.");
  return {std::filesystem::path(local) / L"IBStart", false};
}

std::optional<std::filesystem::path> FindStandardIbases() {
  const auto appData = Env(L"APPDATA");
  const auto localData = Env(L"LOCALAPPDATA");
  std::vector<std::filesystem::path> candidates;
  if (!appData.empty()) {
    candidates.emplace_back(std::filesystem::path(appData) / L"1C" / L"1CEStart" / L"ibases.v8i");
    candidates.emplace_back(std::filesystem::path(appData) / L"1C" / L"1Cv8" / L"ibases.v8i");
  }
  if (!localData.empty()) candidates.emplace_back(std::filesystem::path(localData) / L"1C" / L"1CEStart" / L"ibases.v8i");
  for (const auto& candidate : candidates) if (std::filesystem::is_regular_file(candidate)) return candidate;
  return std::nullopt;
}

void EnsureWritable(const StorageLayout& layout) {
  std::error_code error;
  std::filesystem::create_directories(layout.root / L"logs", error);
  if (error) throw std::runtime_error("IBStart data directory cannot be created: " + error.message());
  const auto probe = layout.root / (L".write_probe." + std::to_wstring(GetCurrentProcessId()));
  std::ofstream output(probe, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("IBStart data directory is not writable: " + utf::ToUtf8(layout.root.wstring()));
  output.close();
  std::filesystem::remove(probe, error);
}

}  // namespace ibstart::storage
