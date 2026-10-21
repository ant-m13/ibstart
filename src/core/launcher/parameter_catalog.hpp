#pragma once

#include "core/domain/model.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::launcher {

enum class ParameterValueKind { flag, text, choice, signed_switch, file, reference };

struct ParameterDefinition {
  std::wstring_view key;
  std::wstring_view syntax;
  std::wstring_view description;
  std::wstring_view category;
  ParameterValueKind kind{ParameterValueKind::reference};
  std::wstring_view choices;  // Pipe-separated, without translated display labels.
  std::wstring_view note;
  int modes{3};  // Enterprise = 1, Designer = 2; both = 3.
  bool thick_only{false};
  std::wstring_view conflict_group;
  std::wstring_view option;  // Optional flag, e.g. -NoTruncate after /Out.
};

struct ParameterEditorContext {
  std::optional<domain::LaunchMode> mode;
  domain::ClientType client{domain::ClientType::automatic};
  std::wstring version;
  bool allow_connection_parameters{false};  // Standalone composition, without a selected database.
};

enum class ParameterAvailability { available, unavailable, unverified };

struct ParameterFragment {
  size_t begin{};
  size_t end{};
  std::wstring value;
  bool has_value{false};
  bool option{false};
  bool structured{false};
};

[[nodiscard]] std::span<const ParameterDefinition> LaunchParameterCatalog();
// Resolve the editor kind from the exact versioned syntax. Process modes and
// reference groups stay read-only; a documented command with no operands is a flag.
[[nodiscard]] ParameterDefinition ParameterEditorDefinition(
    const ParameterDefinition& definition, std::wstring_view version);
[[nodiscard]] std::vector<std::wstring> LaunchParameterVersions();
[[nodiscard]] std::wstring ParameterDocumentationUrl(std::wstring_view version);
[[nodiscard]] std::wstring ParameterVersionFamily(std::wstring_view version);
[[nodiscard]] std::wstring DefaultParameterReferenceVersion(std::wstring_view requested,
    std::span<const domain::PlatformInstallation> installations);
[[nodiscard]] std::wstring ParameterFirstDocumentedVersion(std::wstring_view key);
[[nodiscard]] std::wstring ParameterDocumentedVersions(std::wstring_view key);
[[nodiscard]] std::wstring ParameterHelpText(
    const ParameterDefinition& definition, const ParameterEditorContext& context);
[[nodiscard]] ParameterAvailability ParameterVersionAvailability(
    std::wstring_view key, std::wstring_view version);
[[nodiscard]] std::wstring ParameterChoicesForVersion(
    const ParameterDefinition& definition, std::wstring_view version);
[[nodiscard]] std::wstring ParameterContextNotice(
    const ParameterDefinition& definition, const ParameterEditorContext& context);
[[nodiscard]] std::vector<ParameterFragment> FindParameterFragments(
    std::wstring_view text, const ParameterDefinition& definition);
// Replaces exactly one recognized fragment, preserving all surrounding text.
// Rejects ambiguous duplicates and complex fragments rather than losing options.
[[nodiscard]] std::wstring SetLaunchParameter(std::wstring_view text,
    const ParameterDefinition& definition, std::wstring_view value, bool option = false);
// Add only the command token, leaving required values for subsequent editing.
// Existing commands are preserved without duplication; the draft is still validated.
[[nodiscard]] std::wstring AddLaunchParameterCommand(std::wstring_view text,
    const ParameterDefinition& definition, const ParameterEditorContext& context);
// Removes one unambiguous structured fragment, preserving surrounding text.
[[nodiscard]] std::wstring RemoveLaunchParameter(std::wstring_view text,
    const ParameterDefinition& definition);
[[nodiscard]] std::vector<std::wstring> ValidateParameterDraft(
    std::wstring_view text, const ParameterEditorContext& context = {});

}  // namespace ibstart::launcher
