#pragma once

#include "core/domain/identifier.hpp"
#include "core/domain/model.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::configuration {

struct Definition {
  std::wstring id;
  std::wstring name;
  std::wstring title;
  std::wstring short_name;
  bool operator==(const Definition&) const = default;
};

struct Assignment {
  std::wstring catalog_path;
  std::wstring database_id;
  std::wstring configuration_id;
  std::wstring version;
  bool operator==(const Assignment&) const = default;
};

[[nodiscard]] std::vector<Definition> BuiltInDefinitions();
[[nodiscard]] std::wstring NormalizeCatalogPath(const std::filesystem::path& path);
[[nodiscard]] std::wstring NewId();
[[nodiscard]] bool ValidText(std::wstring_view value, size_t limit = 256);
[[nodiscard]] std::wstring Trim(std::wstring_view value);
[[nodiscard]] const Definition* Find(const std::vector<Definition>& definitions, std::wstring_view id);
[[nodiscard]] const Assignment* FindAssignment(const std::vector<Assignment>& assignments,
    std::wstring_view catalog_path, std::wstring_view database_id);
[[nodiscard]] std::wstring Label(const Definition& definition);
[[nodiscard]] std::wstring Badge(const std::vector<Definition>& definitions, const Assignment* assignment);
[[nodiscard]] bool Matches(const std::vector<Definition>& definitions, const Assignment* assignment,
    std::wstring_view query);

}  // namespace ibstart::configuration
