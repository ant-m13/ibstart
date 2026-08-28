#include "ui/tag_manager.hpp"

#include "core/domain/utf.hpp"
#include "ui/input_box.hpp"
#include "ui/tag_assignment_dialog.hpp"
#include "ui/tag_manager_dialog.hpp"
#include "ui/tree_presentation.hpp"

#include <algorithm>
#include <cwctype>
#include <exception>
#include <string_view>

namespace ibstart::ui {
namespace {

void Message(HWND owner, std::wstring_view text, UINT type = MB_OK | MB_ICONINFORMATION) {
  MessageBoxW(owner, std::wstring(text).c_str(), L"ИБ Старт", type);
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TrimText(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

}  // namespace

TagManager::TagManager(
    catalog::CatalogMetadataService& catalog_metadata, logging::Logger& logger) noexcept
    : catalog_metadata_(catalog_metadata), logger_(logger) {}

TagManager::Result TagManager::EditAssignment(HWND owner, const domain::Entry* entry) {
  if (!entry || !entry->IsDatabase()) {
    Message(owner, L"Выберите информационную базу для изменения тегов.", MB_OK | MB_ICONWARNING);
    return {};
  }

  const domain::Entry selected = *entry;
  const auto database_id = presentation::TagId(selected);
  const auto& metadata = catalog_metadata_.Read();
  const auto edited = dialog::EditTagAssignment(owner, presentation::TagsFor(metadata.tags, selected),
      metadata.tags, metadata.tag_styles);
  if (!edited) return {};

  try {
    catalog_metadata_.SetTags(database_id, *edited);
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка сохранения тегов: " + utf::FromUtf8(error.what()));
    Message(owner, L"Не удалось сохранить теги базы.", MB_OK | MB_ICONERROR);
    return {};
  }
  return {true, edited->empty() ? L"Теги базы очищены." :
      L"Теги базы сохранены: " + presentation::TagsText(*edited)};
}

TagManager::Result TagManager::Configure(HWND owner) {
  const auto& metadata = catalog_metadata_.Read();
  const auto updated = dialog::EditTagManager(owner, metadata.tags, metadata.tag_styles);
  if (!updated) return {};
  try {
    catalog_metadata_.ReplaceTagConfiguration(updated->tags, updated->styles);
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка сохранения настроек тегов: " + utf::FromUtf8(error.what()));
    Message(owner, L"Не удалось сохранить настройки тегов.", MB_OK | MB_ICONERROR);
    return {};
  }
  return {true, L"Настройки тегов сохранены."};
}

TagManager::Result TagManager::AddTag(HWND owner, const domain::Entry* entry, std::wstring tag) {
  tag = TrimText(tag);
  if (tag.empty() || !entry || !entry->IsDatabase()) return {};
  const domain::Entry selected = *entry;
  const auto database_id = presentation::TagId(selected);
  try {
    if (!catalog_metadata_.AddTag(database_id, tag)) {
      return {false, L"У базы уже есть тег «" + tag + L"»."};
    }
  } catch (const std::exception& error) {
    logger_.Error(L"Ошибка добавления тега: " + utf::FromUtf8(error.what()));
    Message(owner, L"Не удалось добавить тег базе.", MB_OK | MB_ICONERROR);
    return {};
  }
  return {true, L"Тег добавлен: " + presentation::TagsText(
      presentation::TagsFor(catalog_metadata_.Read().tags, selected))};
}

TagManager::Result TagManager::AddNewTag(HWND owner, const domain::Entry* entry) {
  if (!entry || !entry->IsDatabase()) {
    Message(owner, L"Выберите информационную базу для добавления тега.", MB_OK | MB_ICONWARNING);
    return {};
  }
  const domain::Entry selected = *entry;
  const auto entered = dialog::InputBox(owner, L"Новый тег", L"Название тега:", L"");
  if (!entered) return {};
  const auto requested = TrimText(*entered);
  if (requested.empty()) return {};
  const auto& metadata = catalog_metadata_.Read();
  const auto known = presentation::KnownTags(metadata.tags, metadata.tag_styles);
  const auto found = std::find_if(known.begin(), known.end(),
      [&](const auto& tag) { return EqualNoCase(tag, requested); });
  return AddTag(owner, &selected, found == known.end() ? requested : *found);
}

}  // namespace ibstart::ui
