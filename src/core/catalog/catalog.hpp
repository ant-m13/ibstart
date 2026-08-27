#pragma once

#include "core/domain/model.hpp"
#include "core/v8i/v8i_document.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ibstart::catalog {

inline constexpr size_t kInvalidSectionIndex = std::numeric_limits<size_t>::max();
inline constexpr size_t kCatalogItemDataBase = 4;

struct TreeItem {
  std::wstring name;
  bool database{false};
  std::wstring parent;
  std::vector<TreeItem> children;
  size_t section_index{kInvalidSectionIndex};
};

struct ValidationDiagnostic {
  size_t section_index{kInvalidSectionIndex};
  std::wstring message;
  bool blocking{true};
};

enum class SortDirection { ascending, descending };

// A direct http(s) URL is a legacy but supported form of Connect.  It has no
// connection key such as WS= and must not be preserved as an unknown field
// when an editor rewrites it into the keyed form.
[[nodiscard]] bool IsBareWebConnection(std::wstring_view connect);
[[nodiscard]] bool MatchesSearchText(const domain::Entry& entry, std::wstring_view query);

class Catalog {
 public:
  explicit Catalog(v8i::V8iDocument document = {});

  [[nodiscard]] const v8i::V8iDocument& document() const noexcept { return document_; }
  [[nodiscard]] v8i::V8iDocument& document() noexcept { lookup_.reset(); return document_; }
  [[nodiscard]] const std::vector<ValidationDiagnostic>& diagnostics() const;
  [[nodiscard]] bool IsValid() const {
    return std::none_of(diagnostics().begin(), diagnostics().end(),
        [](const auto& diagnostic) { return diagnostic.blocking; });
  }
  [[nodiscard]] std::vector<TreeItem> Tree() const;
  [[nodiscard]] std::vector<const domain::Entry*> Databases() const;
  [[nodiscard]] domain::Entry* Find(std::wstring_view name);
  [[nodiscard]] const domain::Entry* Find(std::wstring_view name) const;
  [[nodiscard]] domain::Entry* FindBySectionIndex(size_t index);
  [[nodiscard]] const domain::Entry* FindBySectionIndex(size_t index) const;
  [[nodiscard]] const domain::Entry* FindById(std::wstring_view id) const;
  [[nodiscard]] std::wstring ParentOf(std::wstring_view name) const;
  [[nodiscard]] domain::Database DatabaseFor(std::wstring_view name) const;

  bool AddGroup(std::wstring name, std::wstring parent = {});
  bool AddFileDatabase(std::wstring name, const std::filesystem::path& directory, std::wstring parent = {});
  bool AddServerDatabase(std::wstring name, std::wstring connect, std::wstring parent = {});
  bool RenameDatabase(std::wstring_view name, std::wstring new_name);
  bool RenameGroup(std::wstring_view name, std::wstring new_name);
  bool Remove(std::wstring_view name);
  bool Remove(size_t section_index);
  bool Move(std::wstring_view name, std::wstring parent, size_t position);
  bool MoveBy(std::wstring_view name, int offset);
  // Sorts direct children of parent and records the resulting portable order
  // in both OrderInList and OrderInTree.  Folders can optionally form a
  // leading group while their own names still follow the requested direction.
  bool SortChildrenByName(std::wstring_view parent, SortDirection direction, bool folders_first);
  // Replaces the saved order of every direct child of parent.  The caller must
  // supply each child exactly once; this prevents a partial reorder from
  // silently changing the placement of entries that are not currently shown.
  bool SetChildOrder(std::wstring_view parent, const std::vector<std::wstring>& names);
  void Renumber(std::wstring_view parent);
  [[nodiscard]] static std::optional<std::wstring> WebUrl(std::wstring_view connect);
  [[nodiscard]] static bool IsWebConnection(std::wstring_view connect);

 private:
  struct CaseInsensitiveLess {
    using is_transparent = void;
    bool operator()(std::wstring_view left, std::wstring_view right) const noexcept;
  };
  struct LookupIndex {
    std::map<std::wstring, size_t, CaseInsensitiveLess> by_name;
    std::map<std::wstring, size_t, CaseInsensitiveLess> by_id;
    std::set<std::wstring, CaseInsensitiveLess> ambiguous_names;
    std::set<std::wstring, CaseInsensitiveLess> ambiguous_ids;
    std::vector<std::optional<size_t>> parent_indices;
    std::vector<bool> cycle_sections;
    std::vector<ValidationDiagnostic> diagnostics;
  };

  void EnsureLookup() const;

  [[nodiscard]] std::vector<const domain::Entry*> ChildrenOf(std::wstring_view parent) const;
  [[nodiscard]] static std::wstring QuoteConnectionPath(const std::filesystem::path& path);

  v8i::V8iDocument document_;
  mutable std::optional<LookupIndex> lookup_;
};

[[nodiscard]] std::wstring StableDatabaseId(const domain::Entry& entry);

}  // namespace ibstart::catalog
