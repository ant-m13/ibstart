#pragma once

#include <Windows.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace ibstart::utf {

inline std::wstring FromUtf8(std::string_view input) {
  if (input.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (size <= 0) throw std::runtime_error("Invalid UTF-8 input");
  std::wstring output(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), size);
  return output;
}

inline std::string ToUtf8(std::wstring_view input) {
  if (input.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) throw std::runtime_error("Cannot convert Unicode input to UTF-8");
  std::string output(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
  return output;
}

inline std::wstring LastErrorMessage(DWORD error = GetLastError()) {
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring message = length && buffer ? std::wstring(buffer, length) : L"Ошибка Windows " + std::to_wstring(error);
  if (buffer) LocalFree(buffer);
  return message;
}

}  // namespace ibstart::utf
