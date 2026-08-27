#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::connection {

// A 1C Connect value is a semicolon-delimited list of key/value fragments.
// The raw fragment is retained alongside its decoded key/value view so editors
// can preserve unknown fragments when they rebuild a connection string.
struct Fragment {
  std::wstring raw;
  std::wstring key;
  std::wstring value;
  bool has_equals{false};
};

struct ParseResult {
  std::vector<Fragment> fragments;
  std::vector<std::wstring> diagnostics;
};

enum class ConnectionKind { file, web, server };

[[nodiscard]] std::wstring Trim(std::wstring_view value);
[[nodiscard]] ParseResult Parse(std::wstring_view connect);
[[nodiscard]] std::vector<std::wstring> Split(std::wstring_view connect);
[[nodiscard]] std::optional<std::wstring> Value(std::wstring_view connect, std::wstring_view key);
[[nodiscard]] std::wstring ValueOrEmpty(std::wstring_view connect, std::wstring_view key);
[[nodiscard]] std::wstring QuoteValue(std::wstring value);
[[nodiscard]] std::wstring BuildConnection(ConnectionKind kind, std::wstring_view original,
    std::wstring_view file, std::wstring_view web, std::wstring_view server,
    std::wstring_view reference);
[[nodiscard]] bool IsValidHttpUrl(std::wstring_view value);

[[nodiscard]] std::optional<std::wstring> WebUrl(std::wstring_view connect);
[[nodiscard]] bool IsBareWebUrl(std::wstring_view connect);

}  // namespace ibstart::connection
