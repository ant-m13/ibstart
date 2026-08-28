#pragma once

#include "core/domain/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::v8i {

enum class Utf8Encoding { utf8, utf8_bom };

struct Section {
  std::vector<std::wstring> leading_lines;
  domain::Entry entry;
  std::vector<std::wstring> opaque_lines;
  // For every opaque line, records how many fields preceded it in the source.
  // Fields are only updated or appended, so this preserves comments and blank
  // lines relative to every original known or unknown key.
  std::vector<size_t> opaque_field_positions;
  // Each parsed line keeps the ending that followed that line in the source.
  // Empty endings belong to newly created lines or to a source line at EOF.
  std::wstring header_ending;
  std::vector<std::wstring> leading_line_endings;
  std::vector<std::wstring> field_line_endings;
  std::vector<std::wstring> opaque_line_endings;
};

class V8iDocument {
 public:
  static V8iDocument ParseUtf8(std::string_view bytes);
  [[nodiscard]] std::string SerializeUtf8() const;

  [[nodiscard]] Section* Find(std::wstring_view name);
  [[nodiscard]] const Section* Find(std::wstring_view name) const;
  Section& Add(std::wstring name);
  bool Remove(std::wstring_view name);
  bool RemoveAt(size_t index);

  Utf8Encoding encoding{Utf8Encoding::utf8_bom};
  std::wstring newline{L"\r\n"};
  // Source-level snapshot retained for diagnostics and compatibility. Parsed
  // line objects below Section and preamble retain their own endings so that
  // structural edits do not redistribute endings by output index.
  std::vector<std::wstring> line_endings;
  bool trailing_newline{true};
  std::vector<std::wstring> diagnostics;
  std::vector<std::wstring> preamble;
  std::vector<std::wstring> preamble_line_endings;
  std::vector<Section> sections;
};

}  // namespace ibstart::v8i
