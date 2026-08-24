#include "core/connection/connection_string.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace ibstart::connection {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool IsHttpUrl(std::wstring_view value) {
  constexpr std::wstring_view kHttp = L"http://";
  constexpr std::wstring_view kHttps = L"https://";
  return (value.size() >= kHttp.size() && EqualNoCase(value.substr(0, kHttp.size()), kHttp)) ||
      (value.size() >= kHttps.size() && EqualNoCase(value.substr(0, kHttps.size()), kHttps));
}

std::wstring UnquoteValue(std::wstring value) {
  value = Trim(value);
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

}  // namespace

std::wstring Trim(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

std::vector<std::wstring> Split(std::wstring_view connect) {
  std::vector<std::wstring> result;
  size_t begin = 0;
  bool quoted = false;
  for (size_t index = 0; index <= connect.size(); ++index) {
    const wchar_t character = index < connect.size() ? connect[index] : L';';
    if (character == L'"') quoted = !quoted;
    if (character != L';' || quoted) continue;
    auto part = Trim(connect.substr(begin, index - begin));
    if (!part.empty()) result.push_back(std::move(part));
    begin = index + 1;
  }
  return result;
}

std::optional<std::wstring> Value(std::wstring_view connect, std::wstring_view key) {
  for (const auto& part : Split(connect)) {
    const size_t separator = part.find(L'=');
    if (separator == std::wstring::npos || !EqualNoCase(Trim(std::wstring_view(part).substr(0, separator)), key)) continue;
    return UnquoteValue(part.substr(separator + 1));
  }
  return std::nullopt;
}

std::wstring ValueOrEmpty(std::wstring_view connect, std::wstring_view key) {
  return Value(connect, key).value_or(L"");
}

std::wstring QuoteValue(std::wstring value) {
  std::replace(value.begin(), value.end(), L'"', L'\'');
  return L"\"" + value + L"\"";
}

std::optional<std::wstring> WebUrl(std::wstring_view connect) {
  auto direct = Trim(connect);
  if (const size_t separator = direct.find(L';'); separator != std::wstring::npos) {
    direct = Trim(std::wstring_view(direct).substr(0, separator));
  }
  if (IsHttpUrl(direct)) return direct;

  const auto web = Value(connect, L"WS");
  if (web && IsHttpUrl(*web)) return web;
  return std::nullopt;
}

bool IsBareWebUrl(std::wstring_view connect) {
  const auto url = WebUrl(connect);
  return url.has_value() && EqualNoCase(Trim(connect), *url);
}

}  // namespace ibstart::connection
