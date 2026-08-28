#pragma once

#include <Windows.h>

#include <string_view>

namespace ibstart::domain {

// Catalog identifiers are ordinal strings: case is ignored, while every
// other character (including braces and punctuation) remains significant.
inline bool EqualIdentifier(std::wstring_view left, std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      (left.empty() || CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
          right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL);
}

struct IdentifierLess {
  using is_transparent = void;

  bool operator()(std::wstring_view left, std::wstring_view right) const noexcept {
    if (left.empty() || right.empty()) return left.size() < right.size();
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
  }
};

}  // namespace ibstart::domain
