#include "core/catalog/configuration_catalog.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace ibstart::configuration {

std::vector<Definition> BuiltInDefinitions() {
  // IDs are part of the profile format. Never regenerate them on startup.
  return {
      {L"{c6e39f20-0800-4000-8000-000000000001}", L"БухгалтерияПредприятия", L"Бухгалтерия предприятия, редакция 3.0", L"БП 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000002}", L"БухгалтерияПредприятияКОРП", L"Бухгалтерия предприятия КОРП, редакция 3.0", L"БП КОРП 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000003}", L"БухгалтерияПредприятияБазовая", L"Бухгалтерия предприятия (базовая), редакция 3.0", L"БП базовая 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000004}", L"ЗарплатаИУправлениеПерсоналом", L"Зарплата и управление персоналом, редакция 3.1", L"ЗУП 3.1"},
      {L"{c6e39f20-0800-4000-8000-000000000005}", L"ЗарплатаИУправлениеПерсоналомКОРП", L"Зарплата и управление персоналом КОРП, редакция 3.1", L"ЗУП КОРП 3.1"},
      {L"{c6e39f20-0800-4000-8000-000000000006}", L"ЗарплатаИУправлениеПерсоналомБазовая", L"Зарплата и управление персоналом (базовая), редакция 3.1", L"ЗУП базовая 3.1"},
      {L"{c6e39f20-0800-4000-8000-000000000007}", L"УправлениеТорговлей", L"Управление торговлей, редакция 11", L"УТ11"},
      {L"{c6e39f20-0800-4000-8000-000000000008}", L"УправлениеТорговлейБазовая", L"Управление торговлей (базовая), редакция 11", L"УТ11 базовая"},
      {L"{c6e39f20-0800-4000-8000-000000000009}", L"УправлениеПредприятием", L"1С:ERP Управление предприятием 2", L"ERP 2"},
      {L"{c6e39f20-0800-4000-8000-000000000010}", L"УправлениеНебольшойФирмой", L"Управление нашей фирмой, редакция 3.0", L"УНФ 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000011}", L"УправлениеНебольшойФирмойБазовая", L"Управление нашей фирмой (базовая), редакция 3.0", L"УНФ базовая 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000012}", L"Розница", L"Розница, редакция 3.0", L"Розница 3.0"},
      {L"{c6e39f20-0800-4000-8000-000000000013}", L"РозницаБазовая", L"Розница (базовая), редакция 3.0", L"Розница базовая 3.0"},
  };
}

std::wstring NormalizeCatalogPath(const std::filesystem::path& path) {
  if (path.empty()) return {};
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) absolute = path;
  auto text = absolute.lexically_normal().wstring();
  std::replace(text.begin(), text.end(), L'/', L'\\');
  return text;
}

std::wstring NewId() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create configuration identifier.");
  wchar_t buffer[40]{};
  if (!StringFromGUID2(guid, buffer, 40)) throw std::runtime_error("Cannot format configuration identifier.");
  return buffer;
}

bool ValidText(std::wstring_view value, size_t limit) {
  return value.size() <= limit && std::none_of(value.begin(), value.end(), [](wchar_t c) { return c < L' ' || c == 0x7f; });
}

std::wstring Trim(std::wstring_view value) {
  while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
  while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
  return std::wstring(value);
}

const Definition* Find(const std::vector<Definition>& definitions, std::wstring_view id) {
  const auto it = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) { return domain::EqualIdentifier(d.id, id); });
  return it == definitions.end() ? nullptr : &*it;
}

const Assignment* FindAssignment(const std::vector<Assignment>& assignments, std::wstring_view path, std::wstring_view id) {
  const auto it = std::find_if(assignments.begin(), assignments.end(), [&](const auto& a) {
    return domain::EqualIdentifier(a.catalog_path, path) && domain::EqualIdentifier(a.database_id, id);
  });
  return it == assignments.end() ? nullptr : &*it;
}

std::wstring Label(const Definition& d) { return d.title.empty() ? d.name : d.title; }

std::wstring Badge(const std::vector<Definition>& definitions, const Assignment* a) {
  if (!a) return {};
  const auto* d = Find(definitions, a->configuration_id);
  auto text = d ? (d->short_name.empty() ? Label(*d) : d->short_name) : std::wstring{};
  if (!a->version.empty()) { if (!text.empty()) text += L" · "; text += a->version; }
  return text;
}

bool Matches(const std::vector<Definition>& definitions, const Assignment* a, std::wstring_view query) {
  if (!a) return false;
  const auto matches = [query](std::wstring_view text) { return utf::FindNoCaseOrdinal(text, query) != std::wstring_view::npos; };
  if (matches(a->version)) return true;
  const auto* d = Find(definitions, a->configuration_id);
  return d && (matches(d->name) || matches(d->title) || matches(d->short_name));
}

}  // namespace ibstart::configuration
