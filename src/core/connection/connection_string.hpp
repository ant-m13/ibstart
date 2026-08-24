#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::connection {

// A 1C Connect value is a semicolon-delimited list of key/value fragments.
// Quoted values may contain semicolons; callers can preserve unknown fragments
// when they rebuild a connection string for an editor.
[[nodiscard]] std::wstring Trim(std::wstring_view value);
[[nodiscard]] std::vector<std::wstring> Split(std::wstring_view connect);
[[nodiscard]] std::optional<std::wstring> Value(std::wstring_view connect, std::wstring_view key);
[[nodiscard]] std::wstring ValueOrEmpty(std::wstring_view connect, std::wstring_view key);
[[nodiscard]] std::wstring QuoteValue(std::wstring value);

[[nodiscard]] std::optional<std::wstring> WebUrl(std::wstring_view connect);
[[nodiscard]] bool IsBareWebUrl(std::wstring_view connect);

}  // namespace ibstart::connection
