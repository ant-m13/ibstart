#include "core/credentials/credentials.hpp"

#include "core/catalog/catalog.hpp"
#include "core/domain/identifier.hpp"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace ibstart::credentials {
namespace {
bool Blank(std::wstring_view s) { return std::all_of(s.begin(), s.end(), [](wchar_t c) { return std::iswspace(c) != 0; }); }
std::wstring ActualId(const domain::Entry& e) { return e.ValueOr(L"ID"); }
bool SameEntry(const Target& t, const domain::Entry& e) {
  const auto id = ActualId(e);
  if (!t.entry_id.empty()) return !id.empty() && domain::EqualIdentifier(t.entry_id, id);
  return id.empty() && !t.last_known_name.empty() && domain::EqualIdentifier(t.last_known_name, e.name);
}
}

std::filesystem::path NormalizeCatalogPath(const std::filesystem::path& input) {
  if (input.empty()) return {};
  auto text = input.wstring();
  if (text.find(L'\0') != std::wstring::npos) return {};
  if (text.size() >= 8 && text.compare(0, 8, L"\\\\?\\UNC\\") == 0) text = L"\\\\" + text.substr(8);
  else if (text.size() >= 4 && text.compare(0, 4, L"\\\\?\\") == 0) text = text.substr(4);
  std::error_code ec;
  auto p = std::filesystem::absolute(std::filesystem::path(text), ec);
  if (ec) return {};
  p = p.lexically_normal();
  return p;
}
bool SameCatalogPath(const std::filesystem::path& a, const std::filesystem::path& b) {
  const auto na = NormalizeCatalogPath(a).wstring(), nb = NormalizeCatalogPath(b).wstring();
  return !na.empty() && !nb.empty() && domain::EqualIdentifier(na, nb);
}
Target MakeTarget(const std::filesystem::path& path, const domain::Entry& entry) {
  return {NormalizeCatalogPath(path), entry.IsDatabase() ? TargetKind::database : TargetKind::group,
      ActualId(entry), entry.name};
}
bool TargetMatches(const Target& target, const std::filesystem::path& path, const domain::Entry& entry) {
  return SameCatalogPath(target.catalog_path, path) &&
      ((target.kind == TargetKind::database) == entry.IsDatabase()) && SameEntry(target, entry);
}

std::vector<Credential> Applicable(const std::vector<Credential>& input, const std::filesystem::path& path,
    const catalog::Catalog& catalog, const domain::Database& database) {
  const auto* selected = catalog.FindById(database.id);
  if (!selected || !selected->IsDatabase()) return {};
  std::vector<std::pair<int, const Credential*>> ranked;
  for (const auto& credential : input) {
    int rank = credential.scope == ScopeMode::all ? 1 : 0;
    if (credential.scope == ScopeMode::selected) {
      for (const auto& target : credential.targets) {
        if (!SameCatalogPath(target.catalog_path, path)) continue;
        if (target.kind == TargetKind::database && TargetMatches(target, path, *selected)) rank = std::max(rank, 3);
        if (target.kind == TargetKind::group) {
          const auto matches = std::count_if(catalog.document().sections.begin(), catalog.document().sections.end(),
              [&](const auto& section) { return section.entry.IsGroup() && TargetMatches(target, path, section.entry); });
          if (matches != 1) continue;
          std::vector<std::wstring> visited;
          auto parent = catalog.ParentOf(selected->name);
          while (!parent.empty() && std::none_of(visited.begin(), visited.end(), [&](const auto& value) {
                   return domain::EqualIdentifier(value, parent);
                 })) {
            visited.push_back(parent);
            const auto* group = catalog.Find(parent);
            if (!group) break;
            if (TargetMatches(target, path, *group)) { rank = std::max(rank, 2); break; }
            parent = catalog.ParentOf(group->name);
          }
        }
      }
    }
    if (rank) ranked.push_back({rank, &credential});
  }
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return domain::IdentifierLess{}(a.second->title, b.second->title);
  });
  std::vector<Credential> result;
  for (const auto& [rank, value] : ranked) {
    if (std::none_of(result.begin(), result.end(), [&](const auto& x) { return domain::EqualIdentifier(x.id, value->id); })) result.push_back(*value);
  }
  return result;
}

void RenameTargets(std::vector<Credential>& values, const std::filesystem::path& path,
    const domain::Entry& before, const domain::Entry& after) {
  for (auto& credential : values) for (auto& target : credential.targets)
    if (TargetMatches(target, path, before)) {
      target.last_known_name = after.name;
      target.entry_id = ActualId(after);
    }
}

std::wstring Validate(const Credential& value, const std::vector<Credential>& all) {
  if (Blank(value.title) || value.title.empty()) return L"Требуется название.";
  if (Blank(value.user_name) || value.user_name.empty()) return L"Требуется имя пользователя.";
  if (value.id.empty()) return L"Требуется идентификатор учетных данных.";
  if (value.scope == ScopeMode::invalid) return L"Недопустимая область учетных данных.";
  if (value.scope == ScopeMode::selected && value.targets.empty()) return L"Для выбранной области нужны цели.";
  for (const auto& other : all) if (!domain::EqualIdentifier(other.id, value.id) && domain::EqualIdentifier(other.title, value.title)) return L"Названия учетных данных должны быть уникальны.";
  for (const auto& target : value.targets) if (NormalizeCatalogPath(target.catalog_path).empty() || target.last_known_name.empty() ||
      target.entry_id.find(L'\0') != std::wstring::npos || target.last_known_name.find(L'\0') != std::wstring::npos) {
    return L"Недопустимая цель учетных данных.";
  }
  if (value.id.find(L'\0') != std::wstring::npos || value.title.find(L'\0') != std::wstring::npos ||
      value.user_name.find(L'\0') != std::wstring::npos || value.password.find(L'\0') != std::wstring::npos) {
    return L"Учетные данные содержат недопустимый символ.";
  }
  return {};
}
}  // namespace ibstart::credentials
