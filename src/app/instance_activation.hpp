#pragma once

#include <Windows.h>

#include <cstddef>

namespace ibstart::app {

inline constexpr ULONG_PTR kLaunchCopyData = 0x49425354;
inline constexpr std::size_t kMaximumLaunchIdLength = 256;

constexpr bool IsValidLaunchIdLength(std::size_t length) noexcept {
  return length <= kMaximumLaunchIdLength;
}

inline bool IsValidLaunchCopyData(const COPYDATASTRUCT* data) noexcept {
  if (!data || data->dwData != kLaunchCopyData || !data->lpData || data->cbData < sizeof(wchar_t) ||
      data->cbData % sizeof(wchar_t) != 0 || data->cbData > (kMaximumLaunchIdLength + 1) * sizeof(wchar_t)) {
    return false;
  }
  const auto* value = static_cast<const wchar_t*>(data->lpData);
  const std::size_t length = data->cbData / sizeof(wchar_t);
  return value[length - 1] == L'\0';
}

}  // namespace ibstart::app
