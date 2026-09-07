#pragma once

#include "core/catalog/catalog.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ibstart::credentials {

enum class TargetKind { database, group };

struct Target {
  std::filesystem::path catalog_path;
  TargetKind kind{TargetKind::database};
  std::wstring entry_id;
  std::wstring last_known_name;
  bool operator==(const Target&) const = default;
};

enum class ScopeMode { all, selected, invalid };

struct Credential {
  std::wstring id, title, user_name, password;
  ScopeMode scope{ScopeMode::all};
  std::vector<Target> targets;
  bool operator==(const Credential&) const = default;
};

[[nodiscard]] std::filesystem::path NormalizeCatalogPath(const std::filesystem::path&);
[[nodiscard]] bool SameCatalogPath(const std::filesystem::path&, const std::filesystem::path&);
[[nodiscard]] Target MakeTarget(const std::filesystem::path&, const domain::Entry&);
[[nodiscard]] bool TargetMatches(const Target&, const std::filesystem::path&, const domain::Entry&);
[[nodiscard]] std::vector<Credential> Applicable(const std::vector<Credential>&, const std::filesystem::path&,
    const catalog::Catalog&, const domain::Database&);
void RenameTargets(std::vector<Credential>&, const std::filesystem::path&, const domain::Entry& before,
    const domain::Entry& after);
[[nodiscard]] std::wstring Validate(const Credential&, const std::vector<Credential>&);

}  // namespace ibstart::credentials
