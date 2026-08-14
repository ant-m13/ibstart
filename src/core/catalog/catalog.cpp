#include "core/catalog/catalog.hpp"

#include <algorithm>
#include <cwchar>
#include <stdexcept>
#include <utility>

namespace ibstart::domain {
namespace {
bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}
}

const Field* Entry::Find(std::wstring_view key) const {
  const auto found = std::find_if(fields.begin(), fields.end(), [&](const Field& field) { return EqualNoCase(field.key, key); });
  return found == fields.end() ? nullptr : &*found;
}

Field* Entry::Find(std::wstring_view key) {
  return const_cast<Field*>(std::as_const(*this).Find(key));
}

std::wstring Entry::ValueOr(std::wstring_view key, std::wstring_view fallback) const {
  const auto* field = Find(key);
  return field == nullptr ? std::wstring(fallback) : field->value;
}

void Entry::Set(std::wstring_view key, std::wstring value) {
  if (auto* field = Find(key)) field->value = std::move(value);
  else fields.push_back({std::wstring(key), std::move(value)});
}

bool Entry::IsDatabase() const { return Find(L"Connect") != nullptr; }
bool Entry::IsGroup() const { return !IsDatabase(); }
}  // namespace ibstart::domain

namespace ibstart::catalog {
namespace {
bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}
bool LessEntry(const domain::Entry* left, const domain::Entry* right) {
  const auto leftOrder = left->ValueOr(L"OrderInList");
  const auto rightOrder = right->ValueOr(L"OrderInList");
  if (!leftOrder.empty() && !rightOrder.empty() && leftOrder != rightOrder) return leftOrder < rightOrder;
  return _wcsicmp(left->name.c_str(), right->name.c_str()) < 0;
}
}  // namespace

Catalog::Catalog(v8i::V8iDocument document) : document_(std::move(document)) {}

std::vector<const domain::Entry*> Catalog::Databases() const {
  std::vector<const domain::Entry*> result;
  for (const auto& section : document_.sections) if (section.entry.IsDatabase()) result.push_back(&section.entry);
  return result;
}

domain::Entry* Catalog::Find(std::wstring_view name) {
  auto* section = document_.Find(name);
  return section == nullptr ? nullptr : &section->entry;
}

const domain::Entry* Catalog::Find(std::wstring_view name) const {
  const auto* section = document_.Find(name);
  return section == nullptr ? nullptr : &section->entry;
}

domain::Database Catalog::DatabaseFor(std::wstring_view name) const {
  const auto* entry = Find(name);
  if (entry == nullptr || !entry->IsDatabase()) throw std::invalid_argument("The requested catalog entry is not a database.");
  domain::Database result;
  result.name = entry->name;
  result.id = entry->ValueOr(L"ID", entry->name);
  result.connect = entry->ValueOr(L"Connect");
  result.folder = entry->ValueOr(L"Folder");
  result.order_in_list = entry->ValueOr(L"OrderInList");
  result.version = entry->ValueOr(L"Version");
  result.app = entry->ValueOr(L"App");
  result.default_app = entry->ValueOr(L"DefaultApp");
  result.wa = entry->ValueOr(L"WA");
  result.external = entry->ValueOr(L"External");
  result.locale = entry->ValueOr(L"Locale");
  result.client_connection_speed = entry->ValueOr(L"ClientConnectionSpeed");
  result.additional_parameters = entry->ValueOr(L"AdditionalParameters");
  constexpr std::wstring_view known[] = {L"Connect", L"ID", L"Folder", L"OrderInList", L"Version", L"App", L"DefaultApp", L"WA", L"External", L"Locale", L"ClientConnectionSpeed", L"AdditionalParameters"};
  for (const auto& field : entry->fields) {
    if (std::none_of(std::begin(known), std::end(known), [&](std::wstring_view key) { return EqualNoCase(key, field.key); })) result.unknown_fields.push_back(field);
  }
  return result;
}

std::vector<const domain::Entry*> Catalog::ChildrenOf(std::wstring_view parent) const {
  std::vector<const domain::Entry*> result;
  for (const auto& section : document_.sections) {
    const auto& entry = section.entry;
    if (entry.ValueOr(L"Folder") == parent) result.push_back(&entry);
  }
  std::sort(result.begin(), result.end(), LessEntry);
  return result;
}

std::vector<TreeItem> Catalog::Tree() const {
  const auto build = [&](auto&& self, std::wstring_view parent) -> std::vector<TreeItem> {
    std::vector<TreeItem> result;
    for (const auto* entry : ChildrenOf(parent)) {
      TreeItem item{entry->name, entry->IsDatabase(), std::wstring(parent), {}};
      if (entry->IsGroup()) item.children = self(self, entry->name);
      result.push_back(std::move(item));
    }
    return result;
  };
  return build(build, L"");
}

bool Catalog::AddGroup(std::wstring name, std::wstring parent) {
  if (name.empty() || Find(name) != nullptr) return false;
  auto& entry = document_.Add(std::move(name)).entry;
  if (!parent.empty()) entry.Set(L"Folder", std::move(parent));
  Renumber(entry.ValueOr(L"Folder"));
  return true;
}

std::wstring Catalog::QuoteConnectionPath(const std::filesystem::path& path) {
  std::wstring value = path.wstring();
  std::replace(value.begin(), value.end(), L'"', L'\'');
  return L"File=\"" + value + L"\"";
}

bool Catalog::AddFileDatabase(std::wstring name, const std::filesystem::path& directory, std::wstring parent) {
  if (name.empty() || Find(name) != nullptr || !std::filesystem::exists(directory / L"1Cv8.1CD")) return false;
  auto& entry = document_.Add(std::move(name)).entry;
  entry.Set(L"Connect", QuoteConnectionPath(directory));
  entry.Set(L"ID", entry.name);
  if (!parent.empty()) entry.Set(L"Folder", std::move(parent));
  Renumber(entry.ValueOr(L"Folder"));
  return true;
}

bool Catalog::AddServerDatabase(std::wstring name, std::wstring connect, std::wstring parent) {
  if (name.empty() || connect.empty() || Find(name) != nullptr) return false;
  auto& entry = document_.Add(std::move(name)).entry;
  entry.Set(L"Connect", std::move(connect));
  entry.Set(L"ID", entry.name);
  if (!parent.empty()) entry.Set(L"Folder", std::move(parent));
  Renumber(entry.ValueOr(L"Folder"));
  return true;
}

bool Catalog::Remove(std::wstring_view name) {
  const auto* entry = Find(name);
  if (entry == nullptr) return false;
  const std::wstring parent = entry->ValueOr(L"Folder");
  if (entry->IsGroup()) {
    const auto children = ChildrenOf(entry->name);
    for (const auto* child : children) Find(child->name)->Set(L"Folder", parent);
  }
  const bool removed = document_.Remove(name);
  if (removed) Renumber(parent);
  return removed;
}

bool Catalog::Move(std::wstring_view name, std::wstring parent, size_t position) {
  auto* entry = Find(name);
  if (entry == nullptr || (entry->IsGroup() && EqualNoCase(entry->name, parent))) return false;
  if (!parent.empty() && (Find(parent) == nullptr || Find(parent)->IsDatabase())) return false;
  const std::wstring oldParent = entry->ValueOr(L"Folder");
  entry->Set(L"Folder", std::move(parent));
  auto siblings = ChildrenOf(entry->ValueOr(L"Folder"));
  siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [&](const domain::Entry* candidate) { return candidate == entry; }), siblings.end());
  siblings.insert(siblings.begin() + std::min(position, siblings.size()), entry);
  for (size_t index = 0; index < siblings.size(); ++index) const_cast<domain::Entry*>(siblings[index])->Set(L"OrderInList", std::to_wstring(index + 1));
  if (!EqualNoCase(oldParent, entry->ValueOr(L"Folder"))) Renumber(oldParent);
  return true;
}

void Catalog::Renumber(std::wstring_view parent) {
  const auto children = ChildrenOf(parent);
  for (size_t index = 0; index < children.size(); ++index) const_cast<domain::Entry*>(children[index])->Set(L"OrderInList", std::to_wstring(index + 1));
}

bool Catalog::IsWebConnection(std::wstring_view connect) {
  return connect.starts_with(L"http://") || connect.starts_with(L"https://") || connect.find(L"ws=") != std::wstring_view::npos;
}

}  // namespace ibstart::catalog
