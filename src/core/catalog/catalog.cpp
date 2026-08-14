#include "core/catalog/catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <set>
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
std::optional<long long> NumericOrder(std::wstring_view value) {
  if (value.empty()) return std::nullopt;
  const std::wstring text(value);
  wchar_t* end = nullptr;
  errno = 0;
  const long long order = std::wcstoll(text.c_str(), &end, 10);
  if (errno != 0 || end != text.c_str() + text.size()) return std::nullopt;
  return order;
}
bool LessEntry(const domain::Entry* left, const domain::Entry* right) {
  const auto leftOrder = left->ValueOr(L"OrderInList");
  const auto rightOrder = right->ValueOr(L"OrderInList");
  const auto leftNumeric = NumericOrder(leftOrder);
  const auto rightNumeric = NumericOrder(rightOrder);
  if (leftNumeric && rightNumeric && *leftNumeric != *rightNumeric) return *leftNumeric < *rightNumeric;
  if (leftNumeric.has_value() != rightNumeric.has_value()) return leftNumeric.has_value();
  if (!leftOrder.empty() && !rightOrder.empty() && leftOrder != rightOrder) return leftOrder < rightOrder;
  return _wcsicmp(left->name.c_str(), right->name.c_str()) < 0;
}
std::wstring Trim(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}
bool ValidParent(const v8i::V8iDocument& document, std::wstring_view parent) {
  if (parent.empty()) return true;
  const auto* section = document.Find(parent);
  return section != nullptr && section->entry.IsGroup();
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
    if (EqualNoCase(entry.ValueOr(L"Folder"), parent)) result.push_back(&entry);
  }
  std::sort(result.begin(), result.end(), LessEntry);
  return result;
}

std::vector<TreeItem> Catalog::Tree() const {
  std::vector<std::wstring> ancestors;
  const auto build = [&](auto&& self, std::wstring_view parent) -> std::vector<TreeItem> {
    std::vector<TreeItem> result;
    for (const auto* entry : ChildrenOf(parent)) {
      TreeItem item{entry->name, entry->IsDatabase(), std::wstring(parent), {}};
      const bool cycle = std::any_of(ancestors.begin(), ancestors.end(), [&](const auto& ancestor) { return EqualNoCase(ancestor, entry->name); });
      if (entry->IsGroup() && !cycle) { ancestors.push_back(entry->name); item.children = self(self, entry->name); ancestors.pop_back(); }
      result.push_back(std::move(item));
    }
    return result;
  };
  return build(build, L"");
}

bool Catalog::AddGroup(std::wstring name, std::wstring parent) {
  if (name.empty() || Find(name) != nullptr || !ValidParent(document_, parent)) return false;
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
  std::error_code error;
  if (name.empty() || Find(name) != nullptr || !ValidParent(document_, parent) ||
      !std::filesystem::is_regular_file(directory / L"1Cv8.1CD", error)) return false;
  auto& entry = document_.Add(std::move(name)).entry;
  entry.Set(L"Connect", QuoteConnectionPath(directory));
  entry.Set(L"ID", entry.name);
  if (!parent.empty()) entry.Set(L"Folder", std::move(parent));
  Renumber(entry.ValueOr(L"Folder"));
  return true;
}

bool Catalog::AddServerDatabase(std::wstring name, std::wstring connect, std::wstring parent) {
  if (name.empty() || connect.empty() || Find(name) != nullptr || !ValidParent(document_, parent)) return false;
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
  const auto* parentEntry = parent.empty() ? nullptr : Find(parent);
  if (!parent.empty() && (parentEntry == nullptr || parentEntry->IsDatabase())) return false;
  if (entry->IsGroup()) {
    const domain::Entry* current = parentEntry;
    std::set<std::wstring> visited;
    while (current) {
      if (EqualNoCase(current->name, entry->name)) return false;
      std::wstring key = current->name;
      std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
      if (!visited.insert(std::move(key)).second) return false;
      const auto next = current->ValueOr(L"Folder");
      current = next.empty() ? nullptr : Find(next);
    }
  }
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

std::optional<std::wstring> Catalog::WebUrl(std::wstring_view connect) {
  auto direct = Trim(connect);
  if (direct.size() >= 7 && (_wcsnicmp(direct.c_str(), L"http://", 7) == 0 ||
      (direct.size() >= 8 && _wcsnicmp(direct.c_str(), L"https://", 8) == 0))) return direct;

  size_t start = 0;
  bool quoted = false;
  for (size_t index = 0; index <= connect.size(); ++index) {
    const wchar_t character = index < connect.size() ? connect[index] : L';';
    if (character == L'"') quoted = !quoted;
    if (character != L';' || quoted) continue;
    const auto field = connect.substr(start, index - start);
    const size_t separator = field.find(L'=');
    if (separator != std::wstring_view::npos && EqualNoCase(Trim(field.substr(0, separator)), L"ws")) {
      auto value = Trim(field.substr(separator + 1));
      if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
      if (value.size() >= 7 && (_wcsnicmp(value.c_str(), L"http://", 7) == 0 ||
          (value.size() >= 8 && _wcsnicmp(value.c_str(), L"https://", 8) == 0))) return value;
      return std::nullopt;
    }
    start = index + 1;
  }
  return std::nullopt;
}

bool Catalog::IsWebConnection(std::wstring_view connect) { return WebUrl(connect).has_value(); }

}  // namespace ibstart::catalog
