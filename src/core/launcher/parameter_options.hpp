#pragma once
#include "core/launcher/parameter_catalog.hpp"
#include <cstdint>

namespace ibstart::launcher {
enum class OptionStyle { flag, space, attached, equals, colon };
struct NestedParameterDefinition {
  std::wstring_view key, syntax;
  OptionStyle style{};
  bool value_optional{};
  // Position and exact selected profile are supplied by ParameterNestedOptions.
  unsigned position{};
  uint32_t profile{};
};
[[nodiscard]] std::wstring_view DocumentedParameterSyntax(std::wstring_view command, std::wstring_view version);
// Exact profile used by option editors; unknown families return zero.
[[nodiscard]] uint32_t ParameterVersionProfile(std::wstring_view version);
[[nodiscard]] std::wstring ParameterPrimaryOperandLabel(std::wstring_view command, std::wstring_view version);
// Number of positional values represented by the primary editor in this profile.
[[nodiscard]] size_t ParameterPrimaryOperandCount(std::wstring_view command, std::wstring_view version);
[[nodiscard]] std::vector<NestedParameterDefinition> ParameterNestedOptions(std::wstring_view command, std::wstring_view version);
[[nodiscard]] bool NestedParameterRequired(std::wstring_view command, std::wstring_view option,
    std::wstring_view version, std::wstring_view text);
[[nodiscard]] std::wstring NestedParameterRequirement(std::wstring_view command, std::wstring_view option,
    std::wstring_view version);
[[nodiscard]] std::wstring_view NestedParameterExplanation(std::wstring_view command, std::wstring_view option,
    std::wstring_view version = {});
[[nodiscard]] std::wstring NestedParameterChoices(std::wstring_view command, std::wstring_view option,
    std::wstring_view version = L"8.5.1");
[[nodiscard]] std::vector<std::wstring_view> NestedParameterValues(
    std::wstring_view command, std::wstring_view option, std::wstring_view version);
[[nodiscard]] std::wstring_view NestedValueExplanation(std::wstring_view value);
[[nodiscard]] std::vector<std::wstring> ValidateNestedParameters(std::wstring_view text, const ParameterEditorContext& context);
[[nodiscard]] std::optional<std::wstring> ReadNestedParameter(std::wstring_view text,
    const ParameterDefinition& command, const NestedParameterDefinition& option);
// Changes one nested option and orders known options for the selected profile,
// preserving parent operands, sibling values, gaps and unknown tokens.
// Repeated parent commands/options are ambiguous and cannot be edited structurally.
// A single primary value is quoted automatically; multi-value primary fields
// retain their explicit command-line syntax. An already quoted single value is accepted.
[[nodiscard]] std::wstring SetNestedParameter(std::wstring_view text, const ParameterDefinition& command,
    const NestedParameterDefinition& option, std::wstring_view value, bool enabled = true);
}  // namespace ibstart::launcher
