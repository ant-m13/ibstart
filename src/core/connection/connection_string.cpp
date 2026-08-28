#include "core/connection/connection_string.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
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

bool IsHexDigit(wchar_t character) {
  return (character >= L'0' && character <= L'9') ||
      (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F');
}

bool IsUnreserved(wchar_t character) {
  return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'z') ||
      (character >= L'A' && character <= L'Z') || character == L'-' || character == L'.' ||
      character == L'_' || character == L'~';
}

bool IsForbiddenUrlCharacter(wchar_t character) {
  return character <= 0x1F || character == 0x7F || std::iswspace(character) != 0 ||
      character == L'\"' || character == L'<' || character == L'>' || character == L'\\';
}

bool HasForbiddenUrlCharacter(std::wstring_view value) {
  return std::any_of(value.begin(), value.end(), IsForbiddenUrlCharacter);
}

bool HasValidPercentEncoding(std::wstring_view value) {
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] != L'%') continue;
    if (index + 2 >= value.size() || !IsHexDigit(value[index + 1]) || !IsHexDigit(value[index + 2])) return false;
    index += 2;
  }
  return true;
}

bool HasValidPort(std::wstring_view value) {
  if (value.empty()) return false;
  unsigned int port = 0;
  for (const wchar_t character : value) {
    if (character < L'0' || character > L'9') return false;
    port = port * 10U + static_cast<unsigned int>(character - L'0');
    if (port > 65535U) return false;
  }
  return true;
}

bool HasValidIpv6Literal(std::wstring_view value) {
  if (value.empty()) return false;
  std::wstring address(value);
  const size_t zone_separator = address.find(L"%25");
  if (zone_separator != std::wstring::npos) {
    const std::wstring_view zone(address.data() + zone_separator + 3,
        address.size() - zone_separator - 3);
    if (zone.empty() || std::any_of(zone.begin(), zone.end(), [](wchar_t character) {
          return !IsUnreserved(character);
        })) return false;
    address.resize(zone_separator);
  } else if (address.find(L'%') != std::wstring::npos) {
    return false;
  }
  if (address.empty()) return false;
  IN6_ADDR parsed{};
  return InetPtonW(AF_INET6, address.c_str(), &parsed) == 1;
}

bool HasValidHttpAuthority(std::wstring_view authority) {
  if (authority.empty() || HasForbiddenUrlCharacter(authority) ||
      !HasValidPercentEncoding(authority) || authority.find(L';') != std::wstring_view::npos) return false;
  const auto user_separator = authority.find(L'@');
  std::wstring_view host_port = authority;
  if (user_separator != std::wstring_view::npos) {
    if (user_separator == 0 || authority.find(L'@', user_separator + 1) != std::wstring_view::npos) return false;
    const auto user_info = authority.substr(0, user_separator);
    if (user_info.find_first_of(L"[]") != std::wstring_view::npos) return false;
    host_port = authority.substr(user_separator + 1);
  }
  if (host_port.empty()) return false;
  if (host_port.front() == L'[') {
    const size_t closing = host_port.find(L']');
    if (closing <= 1 || !HasValidIpv6Literal(host_port.substr(1, closing - 1))) return false;
    if (closing + 1 == host_port.size()) return true;
    return host_port[closing + 1] == L':' && HasValidPort(host_port.substr(closing + 2));
  }
  if (host_port.find_first_of(L"[]") != std::wstring_view::npos) return false;
  const size_t port_separator = host_port.find(L':');
  if (port_separator != std::wstring_view::npos) {
    if (port_separator == 0 || host_port.find(L':', port_separator + 1) != std::wstring_view::npos ||
        !HasValidPort(host_port.substr(port_separator + 1))) return false;
    host_port = host_port.substr(0, port_separator);
  }
  return !host_port.empty() && host_port.find(L'@') == std::wstring_view::npos;
}

bool HasHttpScheme(std::wstring_view value, std::wstring_view scheme) {
  return value.size() >= scheme.size() && EqualNoCase(value.substr(0, scheme.size()), scheme);
}

bool IsHttpUrl(std::wstring_view value) {
  constexpr std::wstring_view kHttp = L"http://";
  constexpr std::wstring_view kHttps = L"https://";
  if (value.empty() || HasForbiddenUrlCharacter(value) || !HasValidPercentEncoding(value)) return false;
  const auto scheme = HasHttpScheme(value, kHttp) ? kHttp : HasHttpScheme(value, kHttps) ? kHttps : std::wstring_view();
  if (scheme.empty()) return false;
  const auto remainder = value.substr(scheme.size());
  const auto authority_end = remainder.find_first_of(L"/?#");
  return HasValidHttpAuthority(remainder.substr(0, authority_end));
}

bool IsQuotedFragment(std::wstring_view raw) {
  const auto trimmed = Trim(raw);
  return trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"';
}

bool IsAmbiguousLegacyWeb(const ParseResult& parsed) {
  if (parsed.fragments.size() < 2 || parsed.fragments.front().has_equals ||
      !IsHttpUrl(parsed.fragments.front().value) || IsQuotedFragment(parsed.fragments.front().raw)) return false;
  const auto first = Trim(parsed.fragments.front().raw);
  if (first.find_first_of(L"?#") != std::wstring::npos) return true;
  return std::any_of(parsed.fragments.begin() + 1, parsed.fragments.end(), [](const auto& fragment) {
    return !fragment.has_equals;
  });
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

std::wstring_view SchemePrefix(std::wstring_view value) {
  constexpr std::wstring_view http = L"http://";
  constexpr std::wstring_view https = L"https://";
  if (value.size() >= http.size() && EqualNoCase(value.substr(0, http.size()), http)) return http;
  if (value.size() >= https.size() && EqualNoCase(value.substr(0, https.size()), https)) return https;
  return {};
}

std::size_t FindKeyValueSeparator(std::wstring_view value) {
  // An unkeyed legacy URL is allowed to contain '=' in its query string.
  // It is recognized by its scheme before looking for a key/value separator.
  if (!SchemePrefix(value).empty()) return std::wstring_view::npos;

  bool quoted = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == L'\"') {
      if (quoted && index + 1 < value.size() && value[index + 1] == L'\"') {
        ++index;
        continue;
      }
      if (quoted && QuoteIsEscapedAtBoundary(value, index)) continue;
      quoted = !quoted;
    } else if (value[index] == L'=' && !quoted) {
      return index;
    }
  }
  return std::wstring_view::npos;
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
      const size_t separator = FindKeyValueSeparator(trimmed);
      if (separator != std::wstring::npos) {
        fragment.has_equals = true;
        fragment.key = Trim(std::wstring_view(trimmed).substr(0, separator));
        fragment.value = DecodeQuotedValue(std::wstring_view(trimmed).substr(separator + 1), result.diagnostics);
      } else {
        // A direct legacy URL may itself be quoted when it contains a
        // semicolon. Decode it like a keyed value while retaining the raw
        // fragment for lossless serialization.
        fragment.value = DecodeQuotedValue(trimmed, result.diagnostics);
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
    else result.push_back(character);
  }
  result.push_back(L'"');
  return result;
}

std::wstring BuildConnection(ConnectionKind kind, std::wstring_view original,
    std::wstring_view file, std::wstring_view web, std::wstring_view server,
    std::wstring_view reference) {
  const auto parsed = Parse(original);
  if (!parsed.diagnostics.empty()) throw std::invalid_argument("Connect contains an unsafe quote sequence.");
  if (kind == ConnectionKind::web && !IsValidHttpUrl(web)) {
    throw std::invalid_argument("Connect contains an invalid web URL.");
  }

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
  const auto parsed = Parse(connect);
  if (!parsed.diagnostics.empty() || parsed.fragments.empty()) return std::nullopt;
  if (IsAmbiguousLegacyWeb(parsed)) return std::nullopt;
  if (!parsed.fragments.front().has_equals && IsHttpUrl(parsed.fragments.front().value)) {
    return parsed.fragments.front().value;
  }
  for (const auto& fragment : parsed.fragments) {
    if (fragment.has_equals && EqualNoCase(fragment.key, L"WS") && IsHttpUrl(fragment.value)) return fragment.value;
  }
  return std::nullopt;
}

bool IsBareWebUrl(std::wstring_view connect) {
  const auto parsed = Parse(connect);
  return parsed.diagnostics.empty() && parsed.fragments.size() == 1 &&
      !parsed.fragments.front().has_equals && IsHttpUrl(parsed.fragments.front().value);
}

}  // namespace ibstart::connection
