#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace ibstart::windows_path {

// Extended-length Windows paths are limited to 32,767 UTF-16 code units. The
// terminating null is not part of path.native(), so keep one code unit in
// reserve and reject paths at or beyond that boundary.
inline constexpr std::size_t kMaximumPathCharacters = 32767;

[[nodiscard]] inline bool IsWithinLimit(const std::filesystem::path& path) noexcept {
  return path.native().size() < kMaximumPathCharacters;
}

[[nodiscard]] inline std::wstring LengthError(const std::filesystem::path& path) {
  return L"Путь слишком длинный для Windows (" + std::to_wstring(path.native().size()) +
      L" символов; допустимо не более " + std::to_wstring(kMaximumPathCharacters - 1) +
      L"):\n\n" + path.wstring();
}

}  // namespace ibstart::windows_path
