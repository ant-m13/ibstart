#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

namespace ibstart::platform {
namespace detail {

inline bool IsAsciiDigit(const wchar_t character) {
  return character >= L'0' && character <= L'9';
}

inline std::vector<std::wstring_view> VersionParts(std::wstring_view version) {
  std::vector<std::wstring_view> result;
  size_t start = 0;
  while (start < version.size()) {
    while (start < version.size() && !IsAsciiDigit(version[start])) ++start;
    if (start == version.size()) break;
    size_t end = start;
    while (end < version.size() && IsAsciiDigit(version[end])) ++end;
    result.push_back(version.substr(start, end - start));
    start = end;
  }
  return result;
}

inline std::wstring_view WithoutLeadingZeroes(std::wstring_view value) {
  while (value.size() > 1 && value.front() == L'0') value.remove_prefix(1);
  return value;
}

inline int CompareNumericParts(std::wstring_view left, std::wstring_view right) {
  left = WithoutLeadingZeroes(left);
  right = WithoutLeadingZeroes(right);
  if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
  if (left == right) return 0;
  return left < right ? -1 : 1;
}

}  // namespace detail

// Compares decimal version components without converting them to a fixed-width
// integer.  Platform builds are directory/registry values and are not bounded
// to 64 bits.
inline bool IsNewerVersion(std::wstring_view left, std::wstring_view right) {
  const auto leftParts = detail::VersionParts(left);
  const auto rightParts = detail::VersionParts(right);
  const size_t count = std::max(leftParts.size(), rightParts.size());
  for (size_t index = 0; index < count; ++index) {
    const auto leftPart = index < leftParts.size() ? leftParts[index] : std::wstring_view{L"0"};
    const auto rightPart = index < rightParts.size() ? rightParts[index] : std::wstring_view{L"0"};
    const int compared = detail::CompareNumericParts(leftPart, rightPart);
    if (compared != 0) return compared > 0;
  }
  return left > right;
}

}  // namespace ibstart::platform
