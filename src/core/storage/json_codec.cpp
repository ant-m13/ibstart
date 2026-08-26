#include "core/storage/json_codec.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ibstart::storage::json {
namespace {

bool IsWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void SkipWhitespace(std::string_view json, std::size_t& position) {
  while (position < json.size() && IsWhitespace(json[position])) ++position;
}

std::optional<std::string_view> ReadRawString(std::string_view json, std::size_t& position) {
  SkipWhitespace(json, position);
  if (position >= json.size() || json[position++] != '"') return std::nullopt;
  const std::size_t start = position;
  while (position < json.size()) {
    const char character = json[position++];
    if (character == '"') return json.substr(start, position - start - 1);
    if (character == '\\') {
      if (position >= json.size()) return std::nullopt;
      ++position;
    }
  }
  return std::nullopt;
}

bool SkipComposite(std::string_view json, std::size_t& position) {
  if (position >= json.size() || (json[position] != '[' && json[position] != '{')) return false;
  std::vector<char> closing;
  const auto push = [&closing](char opening) { closing.push_back(opening == '[' ? ']' : '}'); };
  push(json[position++]);
  bool quoted = false;
  while (position < json.size()) {
    const char character = json[position++];
    if (quoted) {
      if (character == '\\') {
        if (position >= json.size()) return false;
        ++position;
      } else if (character == '"') {
        quoted = false;
      }
      continue;
    }
    if (character == '"') quoted = true;
    else if (character == '[' || character == '{') push(character);
    else if (!closing.empty() && character == closing.back()) {
      closing.pop_back();
      if (closing.empty()) return true;
    } else if (character == ']' || character == '}') {
      return false;
    }
  }
  return false;
}

std::optional<Value> ReadValue(std::string_view json, std::size_t& position) {
  SkipWhitespace(json, position);
  if (position >= json.size()) return std::nullopt;
  if (json[position] == '"') {
    const auto raw = ReadRawString(json, position);
    if (!raw) return std::nullopt;
    return Value{ValueKind::string, *raw};
  }
  if (json[position] == '[' || json[position] == '{') {
    const std::size_t start = position;
    const ValueKind kind = json[position] == '[' ? ValueKind::array : ValueKind::object;
    if (!SkipComposite(json, position)) return std::nullopt;
    return Value{kind, json.substr(start, position - start)};
  }
  const std::size_t start = position;
  while (position < json.size() && json[position] != ',' && json[position] != ']' &&
      json[position] != '}' && !IsWhitespace(json[position])) ++position;
  if (position == start) return std::nullopt;
  return Value{ValueKind::scalar, json.substr(start, position - start)};
}

std::optional<Object> ReadObject(std::string_view json, std::size_t& position) {
  SkipWhitespace(json, position);
  if (position >= json.size() || json[position++] != '{') return std::nullopt;
  Object result;
  for (;;) {
    SkipWhitespace(json, position);
    if (position >= json.size()) return std::nullopt;
    if (json[position] == '}') {
      ++position;
      return result;
    }
    const auto raw_key = ReadRawString(json, position);
    const auto key = raw_key ? TryUnescape(*raw_key) : std::nullopt;
    if (!key) return std::nullopt;
    SkipWhitespace(json, position);
    if (position >= json.size() || json[position++] != ':') return std::nullopt;
    const auto value = ReadValue(json, position);
    if (!value) return std::nullopt;
    result.push_back({utf::ToUtf8(*key), *value});
    SkipWhitespace(json, position);
    if (position >= json.size()) return std::nullopt;
    const char separator = json[position++];
    if (separator == '}') return result;
    if (separator != ',') return std::nullopt;
  }
}

int HexDigit(char character) noexcept {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

}  // namespace

std::string Escape(std::wstring_view value) {
  std::string result;
  const auto utf8 = utf::ToUtf8(value);
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char unit : utf8) {
    if (unit == '\\') result += "\\\\";
    else if (unit == '"') result += "\\\"";
    else if (unit == '\n') result += "\\n";
    else if (unit == '\r') result += "\\r";
    else if (unit == '\t') result += "\\t";
    else if (unit == '\b') result += "\\b";
    else if (unit == '\f') result += "\\f";
    else if (unit < 0x20) {
      result += "\\u00";
      result.push_back(hex[unit >> 4]);
      result.push_back(hex[unit & 0x0F]);
    } else result.push_back(static_cast<char>(unit));
  }
  return result;
}

std::wstring Unescape(std::string_view value) {
  std::wstring result;
  std::string raw;
  const auto flush_raw = [&] {
    if (!raw.empty()) {
      result += utf::FromUtf8(raw);
      raw.clear();
    }
  };
  const auto unicode_unit = [&](std::size_t offset) -> wchar_t {
    if (offset + 4 > value.size()) throw std::invalid_argument("Truncated JSON Unicode escape.");
    unsigned unit = 0;
    for (std::size_t digit = 0; digit < 4; ++digit) {
      const int parsed = HexDigit(value[offset + digit]);
      if (parsed < 0) throw std::invalid_argument("Invalid JSON Unicode escape.");
      unit = unit * 16 + static_cast<unsigned>(parsed);
    }
    return static_cast<wchar_t>(unit);
  };
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\') {
      if (index + 1 >= value.size()) throw std::invalid_argument("Truncated JSON escape.");
      flush_raw();
      const char next = value[++index];
      if (next == 'n') result.push_back(L'\n');
      else if (next == 'r') result.push_back(L'\r');
      else if (next == 't') result.push_back(L'\t');
      else if (next == 'b') result.push_back(L'\b');
      else if (next == 'f') result.push_back(L'\f');
      else if (next == '"' || next == '\\' || next == '/') result.push_back(static_cast<wchar_t>(next));
      else if (next == 'u') {
        const wchar_t high = unicode_unit(index + 1);
        index += 4;
        if (high >= 0xD800 && high <= 0xDBFF) {
          if (index + 6 >= value.size() || value[index + 1] != '\\' || value[index + 2] != 'u') {
            throw std::invalid_argument("Unpaired JSON high surrogate.");
          }
          const wchar_t low = unicode_unit(index + 3);
          if (low < 0xDC00 || low > 0xDFFF) throw std::invalid_argument("Invalid JSON surrogate pair.");
          result.push_back(high);
          result.push_back(low);
          index += 6;
        } else if (high >= 0xDC00 && high <= 0xDFFF) {
          throw std::invalid_argument("Unpaired JSON low surrogate.");
        } else {
          result.push_back(high);
        }
      } else {
        throw std::invalid_argument("Invalid JSON escape.");
      }
    } else {
      if (static_cast<unsigned char>(value[index]) < 0x20) throw std::invalid_argument("Unescaped JSON control character.");
      raw.push_back(value[index]);
    }
  }
  flush_raw();
  return result;
}

std::optional<std::wstring> TryUnescape(std::string_view value) {
  try {
    return Unescape(value);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<Object> RootObject(std::string_view json) {
  std::size_t position = 0;
  SkipWhitespace(json, position);
  auto result = ReadObject(json, position);
  if (!result) return std::nullopt;
  SkipWhitespace(json, position);
  return position == json.size() ? std::move(result) : std::nullopt;
}

const Value* ObjectValue(const Object& object, std::string_view key) {
  const auto found = std::find_if(object.begin(), object.end(), [&](const Property& property) {
    return property.key == key;
  });
  return found == object.end() ? nullptr : &found->value;
}

std::optional<std::wstring> ObjectString(const Object& object, std::string_view key) {
  const auto* value = ObjectValue(object, key);
  return value && value->kind == ValueKind::string ? TryUnescape(value->raw) : std::nullopt;
}

std::optional<long long> ObjectInteger(const Object& object, std::string_view key) {
  const auto* value = ObjectValue(object, key);
  if (!value || value->kind != ValueKind::scalar) return std::nullopt;
  long long result{};
  const auto [end, error] = std::from_chars(value->raw.data(), value->raw.data() + value->raw.size(), result);
  if (error != std::errc{} || end != value->raw.data() + value->raw.size()) return std::nullopt;
  return result;
}

std::optional<int> ObjectInt(const Object& object, std::string_view key) {
  const auto value = ObjectInteger(object, key);
  if (!value || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) return std::nullopt;
  return static_cast<int>(*value);
}

std::optional<std::vector<std::wstring>> StringArray(const Value* value) {
  if (!value || value->kind != ValueKind::array || value->raw.size() < 2) return std::nullopt;
  std::size_t position = 1;
  std::vector<std::wstring> result;
  for (;;) {
    SkipWhitespace(value->raw, position);
    if (position >= value->raw.size()) return std::nullopt;
    if (value->raw[position] == ']') return result;
    const auto raw_item = ReadRawString(value->raw, position);
    const auto item = raw_item ? TryUnescape(*raw_item) : std::nullopt;
    if (!item) return std::nullopt;
    result.push_back(*item);
    SkipWhitespace(value->raw, position);
    if (position >= value->raw.size()) return std::nullopt;
    const char separator = value->raw[position++];
    if (separator == ']') return result;
    if (separator != ',') return std::nullopt;
  }
}

void ForEachArrayObject(const Object& root, std::string_view array_key,
    const std::function<void(const Object&)>& visitor) {
  const auto* array = ObjectValue(root, array_key);
  if (!array || array->kind != ValueKind::array || array->raw.size() < 2) return;
  std::size_t position = 1;
  for (;;) {
    SkipWhitespace(array->raw, position);
    if (position >= array->raw.size() || array->raw[position] == ']') return;
    const auto item = ReadValue(array->raw, position);
    if (!item) return;
    if (item->kind == ValueKind::object) {
      std::size_t object_position = 0;
      if (const auto object = ReadObject(item->raw, object_position)) visitor(*object);
    }
    SkipWhitespace(array->raw, position);
    if (position >= array->raw.size() || array->raw[position] == ']') return;
    if (array->raw[position++] != ',') return;
  }
}

}  // namespace ibstart::storage::json
