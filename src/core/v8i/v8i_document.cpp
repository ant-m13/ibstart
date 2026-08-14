#include "core/v8i/v8i_document.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ibstart::v8i {
namespace {

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::vector<std::wstring> SplitLines(std::wstring_view text) {
  std::vector<std::wstring> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(L'\n', start);
    std::wstring line(text.substr(start, end == std::wstring_view::npos ? text.size() - start : end - start));
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return lines;
}

bool IsSectionHeader(std::wstring_view line) {
  return line.size() >= 2 && line.front() == L'[' && line.back() == L']';
}

}  // namespace

V8iDocument V8iDocument::ParseUtf8(std::string_view bytes) {
  V8iDocument document;
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    document.encoding = Utf8Encoding::utf8_bom;
    bytes.remove_prefix(3);
  } else {
    document.encoding = Utf8Encoding::utf8;
  }
  if (bytes.find("\r\n") == std::string_view::npos) document.newline = L"\n";

  const auto lines = SplitLines(utf::FromUtf8(bytes));
  Section* current = nullptr;
  std::vector<std::wstring> pending;
  for (const auto& line : lines) {
    if (IsSectionHeader(line)) {
      Section section;
      section.leading_lines = std::move(pending);
      section.entry.name = line.substr(1, line.size() - 2);
      document.sections.push_back(std::move(section));
      current = &document.sections.back();
      continue;
    }
    const size_t separator = line.find(L'=');
    if (current != nullptr && separator != std::wstring::npos && separator != 0) {
      current->entry.fields.push_back({line.substr(0, separator), line.substr(separator + 1)});
    } else if (current != nullptr) {
      current->opaque_lines.push_back(line);
    } else {
      pending.push_back(line);
    }
  }
  document.preamble = std::move(pending);
  return document;
}

std::string V8iDocument::SerializeUtf8() const {
  std::wstring output;
  const auto append_line = [&](std::wstring_view line) {
    output.append(line);
    output.append(newline);
  };
  for (const auto& line : preamble) append_line(line);
  for (const auto& section : sections) {
    for (const auto& line : section.leading_lines) append_line(line);
    append_line(L"[" + section.entry.name + L"]");
    for (const auto& field : section.entry.fields) append_line(field.key + L"=" + field.value);
    for (const auto& line : section.opaque_lines) append_line(line);
  }
  std::string bytes = utf::ToUtf8(output);
  if (encoding == Utf8Encoding::utf8_bom) bytes.insert(0, "\xEF\xBB\xBF");
  return bytes;
}

Section* V8iDocument::Find(std::wstring_view name) {
  const auto found = std::find_if(sections.begin(), sections.end(), [&](const Section& section) {
    return EqualNoCase(section.entry.name, name);
  });
  return found == sections.end() ? nullptr : &*found;
}

const Section* V8iDocument::Find(std::wstring_view name) const {
  return const_cast<V8iDocument*>(this)->Find(name);
}

Section& V8iDocument::Add(std::wstring name) {
  sections.push_back({{}, {std::move(name), {}}, {}});
  return sections.back();
}

bool V8iDocument::Remove(std::wstring_view name) {
  const auto initial = sections.size();
  std::erase_if(sections, [&](const Section& section) { return EqualNoCase(section.entry.name, name); });
  return sections.size() != initial;
}

}  // namespace ibstart::v8i
