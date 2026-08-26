#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::storage::json {

enum class ValueKind { string, scalar, array, object };

struct Value {
  ValueKind kind{};
  std::string_view raw;
};

struct Property {
  std::string key;
  Value value;
};

using Object = std::vector<Property>;

[[nodiscard]] std::string Escape(std::wstring_view value);
[[nodiscard]] std::optional<std::wstring> TryUnescape(std::string_view value);
[[nodiscard]] std::optional<Object> RootObject(std::string_view json);
[[nodiscard]] const Value* ObjectValue(const Object& object, std::string_view key);
[[nodiscard]] std::optional<std::wstring> ObjectString(const Object& object, std::string_view key);
[[nodiscard]] std::optional<long long> ObjectInteger(const Object& object, std::string_view key);
[[nodiscard]] std::optional<int> ObjectInt(const Object& object, std::string_view key);
[[nodiscard]] std::optional<std::vector<std::wstring>> StringArray(const Value* value);
void ForEachArrayObject(const Object& root, std::string_view array_key,
    const std::function<void(const Object&)>& visitor);

}  // namespace ibstart::storage::json
