#include "core/connection/connection_string.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <utility>

namespace ibstart::connection {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool HasValidHttpAuthority(std::wstring_view authority) {
  if (authority.empty() || std::any_of(authority.begin(), authority.end(), [](wchar_t character) {
        return character < L' ' || character == L'\"' || character == L'<' || character == L'>' || character == L'\\';
      })) return false;
  const auto user_separator = authority.rfind(L'@');
  const auto host = authority.substr(user_separator == std::wstring_view::npos ? 0 : user_separator + 1);
  if (host.empty()) return false;
  if (host.front() == L'[') {
    const auto closing = host.find(L']');
    if (closing <= 1) return false;
    if (closing + 1 == host.size()) return true;
    return host[closing + 1] == L':' && closing + 2 < host.size();
  }
  const auto port_separator = host.find(L':');
  if (port_separator == 0) return false;
  return port_separator == std::wstring_view::npos || port_separator + 1 < host.size();
}

bool HasHttpScheme(std::wstring_view value, std::wstring_view scheme) {
  return value.size() >= scheme.size() && EqualNoCase(value.substr(0, scheme.size()), scheme);
}

bool IsHttpUrl(std::wstring_view value) {
  constexpr std::wstring_view kHttp = L"http://";
  constexpr std::wstring_view kHttps = L"https://";
  const auto scheme = HasHttpScheme(value, kHttp) ? kHttp : HasHttpScheme(value, kHttps) ? kHttps : std::wstring_view();
  if (scheme.empty()) return false;
  const auto remainder = value.substr(scheme.size());
  const auto authority_end = remainder.find_first_of(L"/?#");
  return HasValidHttpAuthority(remainder.substr(0, authority_end));
}

std::wstring DecodeQuotedValue(std::wstring_view value, std::vector<std::wstring>& diagnostics) {
  const auto trimmed = Trim(value);
  if (trimmed.empty() || trimmed.front() != L'"') return trimmed;

  size_t closing = std::wstring_view::npos;
  for (size_t index = 1; index < trimmed.size(); ++index) {
    if (trimmed[index] != L'"') continue;
    if (index + 1 < trimmed.size() && trimmed[index + 1] == L'"') {
      ++index;
      continue;
    }
    size_t next = index + 1;
    while (next < trimmed.size() && std::iswspace(trimmed[next])) ++next;
    if (next == trimmed.size()) {
      closing = index;
      break;
    }
  }
  if (closing == std::wstring_view::npos) {
    diagnostics.push_back(L"Значение Connect содержит незакрытую кавычку.");
    return std::wstring(trimmed);
  }

  std::wstring result;
  for (size_t index = 1; index < closing; ++index) {
    if (trimmed[index] == L'"' && index + 1 < closing && trimmed[index + 1] == L'"') {
      result.push_back(L'"');
      ++index;
    } else if (trimmed[index] == L'\\' && index + 2 < closing &&
        trimmed[index + 1] == L'"' && trimmed[index + 2] != L'"') {
      result.push_back(L'"');
      ++index;
    } else {
      result.push_back(trimmed[index]);
    }
  }
  return result;
}

bool QuoteIsEscapedAtBoundary(std::wstring_view text, size_t index) {
  size_t next = index + 1;
  while (next < text.size() && std::iswspace(text[next])) ++next;
  if (next == text.size() || text[next] == L';') return false;
  size_t backslashes = 0;
  for (size_t position = index; position > 0 && text[position - 1] == L'\\'; --position) ++backslashes;
  return backslashes % 2 != 0;
}

}  // namespace

std::wstring Trim(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

ParseResult Parse(std::wstring_view connect) {
  ParseResult result;
  size_t begin = 0;
  bool quoted = false;
  for (size_t index = 0; index <= connect.size(); ++index) {
    if (index < connect.size() && connect[index] == L'"') {
      if (quoted && index + 1 < connect.size() && connect[index + 1] == L'"') {
        ++index;
        continue;
      }
      if (quoted && QuoteIsEscapedAtBoundary(connect, index)) continue;
      quoted = !quoted;
    }
    if (index != connect.size() && (connect[index] != L';' || quoted)) continue;

    const auto raw = std::wstring(connect.substr(begin, index - begin));
    const auto trimmed = Trim(raw);
    if (!trimmed.empty()) {
      Fragment fragment;
      fragment.raw = raw;
      const size_t separator = trimmed.find(L'=');
      if (separator != std::wstring::npos) {
        fragment.has_equals = true;
        fragment.key = Trim(std::wstring_view(trimmed).substr(0, separator));
        fragment.value = DecodeQuotedValue(std::wstring_view(trimmed).substr(separator + 1), result.diagnostics);
      } else {
        fragment.value = trimmed;
      }
      result.fragments.push_back(std::move(fragment));
    }
    begin = index + 1;
  }
  if (quoted) result.diagnostics.push_back(L"Строка Connect содержит незакрытую кавычку.");
  return result;
}

std::vector<std::wstring> Split(std::wstring_view connect) {
  std::vector<std::wstring> result;
  for (auto& fragment : Parse(connect).fragments) {
    result.push_back(Trim(fragment.raw));
  }
  return result;
}

std::optional<std::wstring> Value(std::wstring_view connect, std::wstring_view key) {
  for (const auto& fragment : Parse(connect).fragments) {
    if (!fragment.has_equals || !EqualNoCase(fragment.key, key)) continue;
    return fragment.value;
  }
  return std::nullopt;
}

std::wstring ValueOrEmpty(std::wstring_view connect, std::wstring_view key) {
  return Value(connect, key).value_or(L"");
}

std::wstring QuoteValue(std::wstring value) {
  std::wstring result = L"\"";
  for (const wchar_t character : value) {
    if (character == L'"') result += L"\"\"";
    result.push_back(character);
  }
  result.push_back(L'"');
  return result;
}

std::wstring BuildConnection(ConnectionKind kind, std::wstring_view original,
    std::wstring_view file, std::wstring_view web, std::wstring_view server,
    std::wstring_view reference) {
  const auto parsed = Parse(original);
  if (!parsed.diagnostics.empty()) throw std::invalid_argument("Connect contains an unsafe quote sequence.");

  std::vector<std::wstring> replacement;
  if (kind == ConnectionKind::file) {
    replacement.push_back(L"File=" + QuoteValue(std::wstring(file)));
  } else if (kind == ConnectionKind::web) {
    replacement.push_back(L"WS=" + QuoteValue(std::wstring(web)));
  } else {
    replacement.push_back(L"Srvr=" + QuoteValue(std::wstring(server)));
    replacement.push_back(L"Ref=" + QuoteValue(std::wstring(reference)));
  }

  const auto is_connection_key = [](std::wstring_view key) {
    return EqualNoCase(key, L"File") || EqualNoCase(key, L"WS") ||
        EqualNoCase(key, L"Srvr") || EqualNoCase(key, L"Ref");
  };
  std::vector<std::wstring> preserved;
  preserved.reserve(parsed.fragments.size());
  std::optional<std::size_t> replacement_position;
  for (std::size_t index = 0; index < parsed.fragments.size(); ++index) {
    const auto& fragment = parsed.fragments[index];
    // A legacy direct URL is the first connection fragment. Replace it for
    // every target kind, not only when the target remains a web connection.
    const bool legacy_url = index == 0 && IsBareWebUrl(fragment.raw);
    if (legacy_url || is_connection_key(fragment.key)) {
      if (!replacement_position) replacement_position = preserved.size();
      continue;
    }
    preserved.push_back(fragment.raw);
  }

  const std::size_t insertion_position = replacement_position.value_or(0);
  std::wstring result;
  const auto append = [&result](std::wstring_view value) {
    if (value.empty()) return;
    if (!result.empty()) result.push_back(L';');
    result += value;
  };
  for (std::size_t index = 0; index <= preserved.size(); ++index) {
    if (index == insertion_position) {
      for (const auto& part : replacement) append(part);
    }
    if (index < preserved.size()) append(preserved[index]);
  }
  return result;
}

bool IsValidHttpUrl(std::wstring_view value) { return IsHttpUrl(Trim(value)); }

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
