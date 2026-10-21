#pragma once
#include <string>
#include <string_view>

namespace ibstart::launcher {
struct ParameterDefinition;
// Project-authored explanations compiled into the program, with versioned choices.
[[nodiscard]] std::wstring BuiltinParameterHelp(const ParameterDefinition& definition, std::wstring_view version);
[[nodiscard]] std::wstring_view ParameterExplanation(std::wstring_view key);
[[nodiscard]] std::wstring_view ParameterValueExplanation(std::wstring_view command, std::wstring_view value);
[[nodiscard]] std::wstring_view ContextParameterExplanation(std::wstring_view command,
    std::wstring_view option, std::wstring_view version);
}  // namespace ibstart::launcher
