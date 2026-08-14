#pragma once

#include "core/domain/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ibstart::v8i {

enum class Utf8Encoding { utf8, utf8_bom };

struct Section {
  std::vector<std::wstring> leading_lines;
  domain::Entry entry;
  std::vector<std::wstring> opaque_lines;
};

class V8iDocument {
 public:
  static V8iDocument ParseUtf8(std::string_view bytes);
  [[nodiscard]] std::string SerializeUtf8() const;

  [[nodiscard]] Section* Find(std::wstring_view name);
  [[nodiscard]] const Section* Find(std::wstring_view name) const;
  Section& Add(std::wstring name);
  bool Remove(std::wstring_view name);

  Utf8Encoding encoding{Utf8Encoding::utf8_bom};
  std::wstring newline{L"\r\n"};
  bool trailing_newline{true};
  std::vector<std::wstring> preamble;
  std::vector<Section> sections;
};

}  // namespace ibstart::v8i
