#include "core/storage/storage.hpp"

#include "core/domain/utf.hpp"
#include "core/windows_path.hpp"

#include <Windows.h>

#include <fstream>
#include <string_view>
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

void EnsureSupportedPath(const std::filesystem::path& path, std::wstring_view description) {
  if (windows_path::IsWithinLimit(path)) return;
  throw std::runtime_error(utf::ToUtf8(std::wstring(description) + L": " + windows_path::LengthError(path)));
}

bool IsMissingPathError(const std::error_code& error) {
  return error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND;
}

}  // namespace

StorageLayout ResolveLayout(const std::filesystem::path& executable_path) {
  EnsureSupportedPath(executable_path, L"Путь к исполняемому файлу IBStart");
  const auto executable_directory = executable_path.parent_path();
  const auto marker = executable_path.parent_path() / L"IBStart.portable";
  EnsureSupportedPath(marker, L"Путь к маркеру portable-профиля");
  std::error_code error;
  if (std::filesystem::is_regular_file(marker, error)) {
    error.clear();
    const auto marker_size = std::filesystem::file_size(marker, error);
    if (error) {
      throw std::runtime_error("Cannot inspect portable profile marker: " + utf::ToUtf8(marker.wstring()) +
          ": " + error.message());
    }
    if (marker_size == 0) {
      const auto root = executable_directory / L"data";
      EnsureSupportedPath(root, L"Путь к portable-профилю");
      return {root, true};
    }
  } else if (error && !IsMissingPathError(error)) {
    throw std::runtime_error("Cannot inspect portable profile marker: " + utf::ToUtf8(marker.wstring()) +
        ": " + error.message());
  }
  const auto local = Env(L"LOCALAPPDATA");
  if (local.empty()) throw std::runtime_error("LOCALAPPDATA is unavailable; cannot determine IBStart data directory.");
  const auto root = std::filesystem::path(local) / L"IBStart";
  EnsureSupportedPath(root, L"Путь к профилю IBStart");
  return {root, false};
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
  for (const auto& candidate : candidates) {
    EnsureSupportedPath(candidate, L"Путь к стандартному списку ibases.v8i");
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;
  }
  return std::nullopt;
}

void EnsureWritable(const StorageLayout& layout) {
  EnsureSupportedPath(layout.root, L"Путь к профилю IBStart");
  const auto logs = layout.root / L"logs";
  EnsureSupportedPath(logs, L"Путь к журналам IBStart");
  std::error_code error;
  std::filesystem::create_directories(logs, error);
  if (error) throw std::runtime_error("IBStart data directory cannot be created: " + error.message());
  const auto probe = layout.root / (L".write_probe." + std::to_wstring(GetCurrentProcessId()));
  EnsureSupportedPath(probe, L"Путь к проверочному файлу профиля");
  std::ofstream output(probe, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("IBStart data directory is not writable: " + utf::ToUtf8(layout.root.wstring()));
  output.close();
  std::filesystem::remove(probe, error);
}

}  // namespace ibstart::storage
