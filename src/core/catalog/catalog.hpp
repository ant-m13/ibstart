#pragma once

#include "core/domain/model.hpp"
#include "core/v8i/v8i_document.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ibstart::catalog {

struct TreeItem {
  std::wstring name;
  bool database{false};
  std::wstring parent;
  std::vector<TreeItem> children;
};

// A direct http(s) URL is a legacy but supported form of Connect.  It has no
// connection key such as WS= and must not be preserved as an unknown field
// when an editor rewrites it into the keyed form.
[[nodiscard]] bool IsBareWebConnection(std::wstring_view connect);
[[nodiscard]] bool MatchesSearchText(const domain::Entry& entry, std::wstring_view query);

class Catalog {
 public:
  explicit Catalog(v8i::V8iDocument document = {});

  [[nodiscard]] const v8i::V8iDocument& document() const noexcept { return document_; }
  [[nodiscard]] v8i::V8iDocument& document() noexcept { return document_; }
  [[nodiscard]] std::vector<TreeItem> Tree() const;
  [[nodiscard]] std::vector<const domain::Entry*> Databases() const;
  [[nodiscard]] domain::Entry* Find(std::wstring_view name);
  [[nodiscard]] const domain::Entry* Find(std::wstring_view name) const;
  [[nodiscard]] std::wstring ParentOf(std::wstring_view name) const;
  [[nodiscard]] domain::Database DatabaseFor(std::wstring_view name) const;

  bool AddGroup(std::wstring name, std::wstring parent = {});
  bool AddFileDatabase(std::wstring name, const std::filesystem::path& directory, std::wstring parent = {});
  bool AddServerDatabase(std::wstring name, std::wstring connect, std::wstring parent = {});
  bool RenameDatabase(std::wstring_view name, std::wstring new_name);
  bool RenameGroup(std::wstring_view name, std::wstring new_name);
  bool Remove(std::wstring_view name);
  bool Move(std::wstring_view name, std::wstring parent, size_t position);
  bool MoveBy(std::wstring_view name, int offset);
  // Replaces the saved order of every direct child of parent.  The caller must
  // supply each child exactly once; this prevents a partial reorder from
  // silently changing the placement of entries that are not currently shown.
  bool SetChildOrder(std::wstring_view parent, const std::vector<std::wstring>& names);
  void Renumber(std::wstring_view parent);
  [[nodiscard]] static std::optional<std::wstring> WebUrl(std::wstring_view connect);
  [[nodiscard]] static bool IsWebConnection(std::wstring_view connect);

 private:
  [[nodiscard]] std::vector<const domain::Entry*> ChildrenOf(std::wstring_view parent) const;
  [[nodiscard]] static std::wstring QuoteConnectionPath(const std::filesystem::path& path);

  v8i::V8iDocument document_;
};

}  // namespace ibstart::catalog
