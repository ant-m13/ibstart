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

struct SplitLinesResult {
  std::vector<std::wstring> lines;
  std::vector<std::wstring> endings;
};

SplitLinesResult SplitLines(std::wstring_view text) {
  SplitLinesResult result;
  size_t start = 0;
  for (size_t index = 0; index < text.size();) {
    const wchar_t character = text[index];
    if (character != L'\r' && character != L'\n') {
      ++index;
      continue;
    }

    result.lines.emplace_back(text.substr(start, index - start));
    if (character == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') {
      result.endings.emplace_back(L"\r\n");
      index += 2;
    } else {
      result.endings.emplace_back(1, character);
      ++index;
    }
    start = index;
  }
  if (start < text.size()) result.lines.emplace_back(text.substr(start));
  return result;
}

bool IsSectionHeader(std::wstring_view line) {
  return line.size() >= 2 && line.front() == L'[' && line.back() == L']';
}

struct SerializedLine {
  std::wstring text;
  std::wstring ending;
};

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
  const auto split = SplitLines(utf::FromUtf8(bytes));
  document.line_endings = split.endings;
  document.trailing_newline = !split.endings.empty() &&
      (split.lines.size() == split.endings.size());
  if (!split.endings.empty()) {
    document.newline = split.endings.front();
    if (std::any_of(split.endings.begin() + 1, split.endings.end(), [&](const auto& ending) {
          return ending != document.newline;
        })) {
      document.diagnostics.push_back(L"Файл содержит смешанные переводы строк; исходная последовательность сохранена.");
    }
  }

  const auto& lines = split.lines;
  Section* current = nullptr;
  std::vector<std::wstring> pending;
  std::vector<std::wstring> pending_line_endings;
  for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const auto& line = lines[line_index];
    const auto ending = line_index < split.endings.size() ? split.endings[line_index] : std::wstring();
    if (IsSectionHeader(line)) {
      Section section;
      if (current == nullptr) document.preamble = std::move(pending);
      if (current == nullptr) document.preamble_line_endings = std::move(pending_line_endings);
      section.entry.name = line.substr(1, line.size() - 2);
      section.header_ending = ending;
      document.sections.push_back(std::move(section));
      current = &document.sections.back();
      continue;
    }
    const size_t separator = line.find(L'=');
    if (current != nullptr && separator != std::wstring::npos && separator != 0) {
      current->entry.fields.push_back({line.substr(0, separator), line.substr(separator + 1)});
      current->field_line_endings.push_back(ending);
    } else if (current != nullptr) {
      current->opaque_lines.push_back(line);
      current->opaque_field_positions.push_back(current->entry.fields.size());
      current->opaque_line_endings.push_back(ending);
    } else {
      pending.push_back(line);
      pending_line_endings.push_back(ending);
    }
  }
  if (document.sections.empty()) {
    document.preamble = std::move(pending);
    document.preamble_line_endings = std::move(pending_line_endings);
  }
  return document;
}

std::string V8iDocument::SerializeUtf8() const {
  std::vector<SerializedLine> lines;
  const auto ending_at = [](const std::vector<std::wstring>& endings, size_t index) -> std::wstring_view {
    return index < endings.size() ? std::wstring_view(endings[index]) : std::wstring_view();
  };
  const auto append_line = [&](std::wstring_view line, std::wstring_view ending) {
    lines.push_back({std::wstring(line), std::wstring(ending)});
  };
  const auto append_lines = [&](const std::vector<std::wstring>& values,
      const std::vector<std::wstring>& endings) {
    for (size_t index = 0; index < values.size(); ++index) {
      append_line(values[index], ending_at(endings, index));
    }
  };
  append_lines(preamble, preamble_line_endings);
  for (const auto& section : sections) {
    append_lines(section.leading_lines, section.leading_line_endings);
    append_line(L"[" + section.entry.name + L"]", section.header_ending);
    if (section.opaque_field_positions.size() == section.opaque_lines.size()) {
      size_t opaque = 0;
      for (size_t field = 0; field <= section.entry.fields.size(); ++field) {
        while (opaque < section.opaque_lines.size() && section.opaque_field_positions[opaque] == field) {
          append_line(section.opaque_lines[opaque], ending_at(section.opaque_line_endings, opaque));
          ++opaque;
        }
        if (field < section.entry.fields.size()) {
          const auto& value = section.entry.fields[field];
          append_line(value.key + L"=" + value.value, ending_at(section.field_line_endings, field));
        }
      }
      while (opaque < section.opaque_lines.size()) {
        append_line(section.opaque_lines[opaque], ending_at(section.opaque_line_endings, opaque));
        ++opaque;
      }
    } else {
      // Preserve compatibility with programmatically constructed Section
      // values that predate the position metadata.
      for (size_t field = 0; field < section.entry.fields.size(); ++field) {
        const auto& value = section.entry.fields[field];
        append_line(value.key + L"=" + value.value, ending_at(section.field_line_endings, field));
      }
      append_lines(section.opaque_lines, section.opaque_line_endings);
    }
  }
  std::wstring output;
  const size_t ending_count = trailing_newline ? lines.size() : (lines.empty() ? 0 : lines.size() - 1);
  for (size_t index = 0; index < lines.size(); ++index) {
    output.append(lines[index].text);
    if (index < ending_count) {
      output.append(lines[index].ending.empty() ? newline : lines[index].ending);
    }
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
  sections.push_back({{}, {std::move(name), {}}, {}, {}});
  return sections.back();
}

bool V8iDocument::Remove(std::wstring_view name) {
  const auto found = std::find_if(sections.begin(), sections.end(), [&](const Section& section) {
    return EqualNoCase(section.entry.name, name);
  });
  if (found == sections.end()) return false;
  sections.erase(found);
  return true;
}

bool V8iDocument::RemoveAt(size_t index) {
  if (index >= sections.size()) return false;
  sections.erase(sections.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

}  // namespace ibstart::v8i
