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
  // One entry per physical line ending in the source, preserving mixed files.
  // New lines created by an editor use newline when this sequence is exhausted.
  std::vector<std::wstring> line_endings;
  bool trailing_newline{true};
  std::vector<std::wstring> diagnostics;
  std::vector<std::wstring> preamble;
  std::vector<Section> sections;
};

}  // namespace ibstart::v8i
