#include "core/launcher/parameter_options.hpp"
#include "core/launcher/parameter_help.hpp"
#include "core/launcher/command_builder.hpp"

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace ibstart::launcher {
namespace {
struct CommandSyntax { std::wstring_view command, syntax; unsigned primary_required; uint32_t versions; };
struct OptionSyntax {
  std::wstring_view command, key, syntax;
  OptionStyle style;
  bool value_optional;
  unsigned position;
  uint32_t versions;
};
struct OptionChoice { std::wstring_view command, option, value; uint32_t versions; };
struct PresenceRule { std::wstring_view command, triggers, needed; uint32_t versions; };
struct ConflictRule { std::wstring_view command, first, second; uint32_t versions; };
struct QuoteRule { std::wstring_view command, option; uint32_t versions; };
struct CommandConflictRule { std::wstring_view first, second; uint32_t versions; };
#include "core/launcher/parameter_grammar.inc"
struct Explanation { std::wstring_view key, text; };
const Explanation explanations[] = {
#include "core/launcher/parameter_option_help.inc"
};
bool Equal(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}
uint32_t VersionBit(std::wstring_view version) {
  const auto versions = LaunchParameterVersions();
  const auto family = ParameterVersionFamily(version);
  const auto found = std::find(versions.begin(), versions.end(), family);
  return found == versions.end() ? 0 : uint32_t{1} << static_cast<unsigned>(found - versions.begin());
}
bool HasChoice(std::wstring_view choices, std::wstring_view value) {
  while (!choices.empty()) {
    const auto end = choices.find(L'|');
    if (Equal(choices.substr(0, end), value)) return true;
    if (end == std::wstring_view::npos) break;
    choices.remove_prefix(end + 1);
  }
  return false;
}
std::wstring ChoicesForBit(std::wstring_view command, std::wstring_view option, uint32_t bit) {
  const auto source = Equal(command, L"/CompareCfg") && Equal(option, L"-SecondConfigurationType") ?
      L"-FirstConfigurationType" : option;
  std::wstring result;
  for (const auto& entry : option_choices) {
    if (!Equal(command, entry.command) || !Equal(source, entry.option) || !(entry.versions & bit) || HasChoice(result, entry.value)) continue;
    if (!result.empty()) result += L'|';
    result += entry.value;
  }
  return result;
}
std::vector<std::wstring_view> PrimaryOperandNames(std::wstring_view command, uint32_t bit) {
  const auto found = std::find_if(std::begin(command_syntax), std::end(command_syntax),
      [&](const auto& entry) { return Equal(command, entry.command) && (entry.versions & bit); });
  if (found == std::end(command_syntax)) return {};
  auto syntax = found->syntax;
  if (Equal(command, L"/DisableExtensions") && syntax.find(L"ExtName1") != std::wstring_view::npos)
    return {L"имена расширений через запятую"};
  if (syntax.size() <= command.size()) return {};
  syntax.remove_prefix(command.size());
  const auto start = syntax.find_first_not_of(L" \t");
  if (start == std::wstring_view::npos) return {};
  syntax.remove_prefix(start);
  if (syntax.starts_with(L"[<")) syntax.remove_prefix(1);
  std::vector<std::wstring_view> names;
  while (syntax.starts_with(L"<")) {
    const auto end = syntax.find(L'>');
    if (end == std::wstring_view::npos) return {};
    auto name = syntax.substr(1, end - 1);
    // Historical LoadConfigFiles headings copy the dump-directory label.
    if (Equal(command, L"/LoadConfigFiles") && name == L"каталог выгрузки") name = L"каталог загрузки";
    names.push_back(name);
    syntax.remove_prefix(end + 1);
    const auto next = syntax.find_first_not_of(L" \t");
    if (next == std::wstring_view::npos) break;
    syntax.remove_prefix(next);
  }
  return names;
}

std::wstring SinglePrimaryOperand(std::wstring_view value) {
  if (value.empty()) return {};
  // Existing editors may supply a complete explicitly quoted operand.
  // Decode it once before serializing, rather than quote the quote marks.
  const auto first = value.find_first_not_of(L" \t\r\n");
  if (first != std::wstring_view::npos && value[first] == L'"') {
    const auto tokens = TokenizeCommandArguments(value);
    if (tokens.size() != 1) throw std::invalid_argument("Only one primary operand is allowed in this field.");
    return QuoteWindowsArgument(tokens.front().value,
        tokens.front().value.starts_with(L"/") || tokens.front().value.starts_with(L"-"));
  }
  return QuoteWindowsArgument(value, value.starts_with(L"/") || value.starts_with(L"-"));
}
struct OptionFragment { size_t begin{}, end{}; std::wstring value; bool has_value{false}; bool ambiguous{false}; };
void RequireSeparateParent(std::wstring_view body, std::wstring_view command) {
  const auto tokens = TokenizeCommandArguments(body);
  if (tokens.empty() || !Equal(tokens.front().value, command))
    throw std::invalid_argument("Attached parent operands require manual editing.");
}
std::vector<OptionFragment> Fragments(std::wstring_view text, const NestedParameterDefinition& option) {
  const auto tokens = TokenizeCommandArguments(text);
  std::vector<OptionFragment> result;
  if (option.key.empty()) {
    const auto choices = tokens.empty() ? std::wstring{} : ChoicesForBit(tokens.front().value, L"", option.profile);
    if (!choices.empty()) {
      bool unknown_option = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (HasChoice(choices, token.value) || (i == 1 && !token.value.starts_with(L"-"))) {
          // A future option may own this token as its value. Never rewrite
          // it as a transport merely because the spelling matches a choice.
          result.push_back({token.begin, token.end, token.value, true, unknown_option || (token.quoted && i != 1)});
        } else if (!token.quoted && token.value.starts_with(L"-")) {
          const bool known = std::any_of(std::begin(option_syntax), std::end(option_syntax), [&](const auto& entry) {
            return Equal(tokens.front().value, entry.command) && (entry.versions & option.profile) && Equal(token.value, entry.key);
          });
          if (!known) unknown_option = true;
        }
      }
      return result;
    }
    size_t end = 1;
    while (end < tokens.size() && (tokens[end].quoted || !tokens[end].value.starts_with(L"-"))) ++end;
    if (end > 1) result.push_back({tokens[1].begin, tokens[end - 1].end,
        std::wstring(text.substr(tokens[1].begin, tokens[end - 1].end - tokens[1].begin)), true});
    return result;
  }
  for (size_t i = 1; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (token.quoted) continue;
    std::wstring value;
    bool has_value = false;
    bool matched = Equal(token.value, option.key);
    if (!matched && token.value.size() > option.key.size() &&
        Equal(std::wstring_view(token.value).substr(0, option.key.size()), option.key)) {
      const bool longer_switch = std::any_of(std::begin(option_syntax), std::end(option_syntax), [&](const auto& known) {
        if (!Equal(tokens.front().value, known.command) || known.key.size() <= option.key.size() ||
            token.value.size() < known.key.size() || !Equal(std::wstring_view(token.value).substr(0, known.key.size()), known.key)) return false;
        if (token.value.size() == known.key.size()) return true;
        const auto suffix = token.value[known.key.size()];
        return known.style == OptionStyle::attached || (known.style == OptionStyle::equals && suffix == L'=') ||
            (known.style == OptionStyle::colon && suffix == L':');
      });
      if (longer_switch) continue;
      const auto tail = std::wstring_view(token.value).substr(option.key.size());
      if (option.style == OptionStyle::attached) { value = tail; matched = has_value = true; }
      else if ((option.style == OptionStyle::equals && tail.front() == L'=') ||
          (option.style == OptionStyle::colon && tail.front() == L':')) { value = tail.substr(1); matched = has_value = true; }
    }
    if (!matched) continue;
    size_t end = token.end;
    if (option.style != OptionStyle::flag && value.empty() && i + 1 < tokens.size()) {
      const auto& next = tokens[i + 1];
      // A sibling switch is never consumed as a missing value. Negative numbers
      // and explicitly quoted values are still valid operands.
      const bool sibling = !next.quoted && !next.value.empty() && (next.value.front() == L'/' ||
          (next.value.front() == L'-' && next.value.size() > 1 && !std::iswdigit(next.value[1])));
      if (!sibling) { value = next.value; end = next.end; has_value = true; ++i; }
    }
    result.push_back({token.begin, end, std::move(value), has_value});
  }
  return result;
}

std::vector<NestedParameterDefinition> OptionsForBit(std::wstring_view command, uint32_t bit) {
  std::vector<NestedParameterDefinition> result;
  for (const auto& entry : option_syntax) {
    if (!Equal(command, entry.command) || !(entry.versions & bit) ||
        std::any_of(result.begin(), result.end(), [&](const auto& item) { return Equal(item.key, entry.key); })) continue;
    result.push_back({entry.key, entry.syntax, entry.style, entry.value_optional, entry.position, bit});
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.position < right.position; });
  return result;
}

bool AnyPresent(std::wstring_view names, std::wstring_view body, const std::vector<NestedParameterDefinition>& options) {
  return std::any_of(options.begin(), options.end(), [&](const auto& option) {
    return HasChoice(names, option.key) && !Fragments(body, option).empty();
  });
}

std::wstring ReadableNames(std::wstring_view names) {
  std::wstring result;
  while (!names.empty()) {
    const auto end = names.find(L'|');
    if (!result.empty()) result += L" или ";
    result += names.substr(0, end);
    if (end == std::wstring_view::npos) break;
    names.remove_prefix(end + 1);
  }
  return result;
}

// Reorder whole option/value spans, retaining gaps, quoted bytes, primary
// operands and unknown tokens. Repeated groups must be edited manually.
std::wstring OrderOptions(std::wstring_view body, const ParameterDefinition& command,
    const std::vector<NestedParameterDefinition>& options) {
  struct Item { OptionFragment fragment; unsigned position; };
  std::vector<Item> items;
  for (const auto& option : options) {
    const auto found = Fragments(body, option);
    if (found.size() > 1) throw std::invalid_argument("Repeated option groups require manual editing.");
    if (!found.empty()) items.push_back({found.front(), option.position});
  }
  // The first of these two selectors wins according to the documentation.
  // Never reverse a manually selected precedence when changing another field.
  if (Equal(command.key, L"/LoadConfigFromFiles") &&
      AnyPresent(L"-files", body, options) && AnyPresent(L"-listFile", body, options))
    throw std::invalid_argument("Input selector precedence requires manual editing.");
  if (Equal(command.key, L"/CreateDistributionFiles") &&
      AnyPresent(L"-f", body, options) && AnyPresent(L"-v", body, options))
    throw std::invalid_argument("Several source groups require manual editing.");
  std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) { return left.fragment.begin < right.fragment.begin; });
  auto ordered = items;
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.position < right.position; });
  std::wstring result;
  size_t copied = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& slot = items[i].fragment;
    const auto& value = ordered[i].fragment;
    if (slot.begin < copied) throw std::invalid_argument("Overlapping option fragments.");
    result += body.substr(copied, slot.begin - copied);
    result += body.substr(value.begin, value.end - value.begin);
    copied = slot.end;
  }
  result += body.substr(copied);
  return result;
}
}  // namespace

uint32_t ParameterVersionProfile(std::wstring_view version) {
  return VersionBit(version);
}

std::wstring_view DocumentedParameterSyntax(std::wstring_view command, std::wstring_view version) {
  const auto bit = VersionBit(version);
  const auto found = std::find_if(std::begin(command_syntax), std::end(command_syntax),
      [&](const auto& entry) { return Equal(command, entry.command) && (entry.versions & bit); });
  return found == std::end(command_syntax) ? std::wstring_view{} : found->syntax;
}

std::vector<NestedParameterDefinition> ParameterNestedOptions(std::wstring_view command, std::wstring_view version) {
  return OptionsForBit(command, VersionBit(version));
}

std::wstring ParameterPrimaryOperandLabel(std::wstring_view command, std::wstring_view version) {
  std::wstring label;
  for (const auto name : PrimaryOperandNames(command, VersionBit(version))) {
    if (!label.empty()) label += L"; ";
    label += name;
  }
  return label;
}

size_t ParameterPrimaryOperandCount(std::wstring_view command, std::wstring_view version) {
  return PrimaryOperandNames(command, VersionBit(version)).size();
}

bool NestedParameterRequired(std::wstring_view command, std::wstring_view option,
    std::wstring_view version, std::wstring_view text) {
  const auto bit = VersionBit(version);
  const auto options = OptionsForBit(command, bit);
  std::wstring_view body;
  const auto catalog = LaunchParameterCatalog();
  const auto def = std::find_if(catalog.begin(), catalog.end(), [&](const auto& entry) { return Equal(command, entry.key); });
  if (def != catalog.end()) {
    const auto parents = FindParameterFragments(text, *def);
    if (parents.size() == 1) body = text.substr(parents.front().begin, parents.front().end - parents.front().begin);
  }
  if (option.empty()) {
    const auto grammar = std::find_if(std::begin(command_syntax), std::end(command_syntax),
        [&](const auto& item) { return Equal(command, item.command) && (item.versions & bit); });
    return grammar != std::end(command_syntax) && grammar->primary_required &&
        !(Equal(command, L"/LoadConfigFromFiles") && AnyPresent(L"-Archive", body, options));
  }
  return std::any_of(std::begin(presence_rules), std::end(presence_rules), [&](const auto& rule) {
    return Equal(command, rule.command) && (rule.versions & bit) && Equal(option, rule.needed) &&
        (rule.triggers.empty() || AnyPresent(rule.triggers, body, options));
  });
}

std::wstring NestedParameterRequirement(std::wstring_view command, std::wstring_view option, std::wstring_view version) {
  const auto bit = VersionBit(version);
  std::wstring text;
  for (const auto& rule : presence_rules) {
    if (!Equal(command, rule.command) || !(rule.versions & bit) || !HasChoice(rule.needed, option)) continue;
    if (!text.empty()) text += L" ";
    const bool alternative = rule.needed.find(L'|') != std::wstring_view::npos;
    if (rule.triggers.empty()) text += alternative ? L"Обязателен один из вариантов: " + ReadableNames(rule.needed) + L"." : L"Обязательный параметр.";
    else text += (alternative ? L"Один из вариантов (" + ReadableNames(rule.needed) + L") требуется" : std::wstring(L"Параметр требуется")) +
        L" при использовании группы: " + ReadableNames(rule.triggers) + L".";
  }
  return text;
}

std::wstring_view NestedParameterExplanation(std::wstring_view command, std::wstring_view option, std::wstring_view version) {
  if (const auto detail = ContextParameterExplanation(command, option, version); !detail.empty()) return detail;
  if (Equal(command, L"/SetPredefinedDataUpdate") && Equal(option, L"-Auto"))
    return L"Значение по умолчанию. При реструктуризации главного узла выбирается -UpdateAutomatically, периферийного — -DoNotUpdateAutomatically.";
  if (Equal(command, L"/RegServer") && Equal(option, L"-Auto"))
    return L"При наличии прав регистрирует компоненты для всех пользователей, иначе — для текущего пользователя, без диалогов.";
  if (Equal(command, L"/HttpsCert") && Equal(option, L"-auto"))
    return L"Автоматически выбирает подходящий клиентский сертификат из выбранного системного источника.";
  if ((Equal(command, L"/HttpsCert") || Equal(command, L"/HttpsCA")) && Equal(option, L"-file"))
    return L"Файл клиентского сертификата либо сертификатов удостоверяющих центров. Имеет приоритет над системным источником; -none отключает и этот источник.";
  if (Equal(command, L"/UILogRecorder") && Equal(option, L"-File"))
    return L"Файл журнала действий после окончания записи, если клиент не подключён к менеджеру тестирования.";
  if ((Equal(command, L"/CreateDistributive") || Equal(command, L"/CreateDistributivePackage")) && Equal(option, L"-File"))
    return L"Файл описания комплекта поставки, используемый для её формирования.";
  if (Equal(command, L"/SignCfg") && Equal(option, L"-File"))
    return L"Исходный файл cfe при выборе -ConfigurationType File.";
  if (Equal(command, L"/SignCfg") && Equal(option, L"-Name"))
    return L"Имя расширения для источника ExtensionConfiguration, ExtensionDBConfiguration или ExtensionConfigurationRepository.";
  if (Equal(command, L"/ConfigurationRepositorySetLabel") && Equal(option, L"-name"))
    return L"Имя метки версии хранилища; сама опция -name перед этим значением необязательна.";
  if (Equal(command, L"/ConfigurationRepositoryCopyUsers") && Equal(option, L"-Path"))
    return L"Адрес исходного хранилища, из которого копируются пользователи.";
  if (Equal(command, L"/HttpsNSS") && Equal(option, L"-path"))
    return L"Каталог хранилища сертификатов NSS.";
  if (Equal(command, L"/DumpConfigToFiles") && Equal(option, L"-force"))
    return L"Вместе с -update выполняет полную выгрузку, если версия формата отличается от записанной в ConfigDumpInfo.xml. Без -update не используется.";
  if (Equal(command, L"/ManageCfgSupport") && Equal(option, L"-force"))
    return L"Разрешает снятие с поддержки даже для конфигурации с запрещёнными изменениями; без этой опции такой случай завершается ошибкой.";
  if (Equal(command, L"/ConfigurationRepositoryUpdateCfg") && Equal(option, L"-force"))
    return L"Подтверждает получение новых и удаление отсутствующих объектов при обновлении из хранилища; без подтверждения такие действия не выполняются.";
  if (Equal(command, L"/ConfigurationRepositoryUnLock") && Equal(option, L"-force"))
    return L"Определяет принудительное освобождение объектов при наличии локальных изменений.";
  if (Equal(command, L"/ConfigurationRepositoryCommit") && Equal(option, L"-force"))
    return L"Определяет принудительное помещение изменений при обнаружении ссылок на удалённые объекты.";
  if (Equal(command, L"/CheckModules") || Equal(command, L"/CheckConfig")) {
    if (Equal(option, L"-Server")) return L"Проверяет серверный контекст модулей.";
  } else if (Equal(option, L"-Server"))
    return L"Выполняет операцию на сервере клиент-серверной базы; ограничения фаз фонового обновления определяются платформой.";
  if (const auto text = ParameterValueExplanation(command, option); !text.empty()) return text;
  const auto found = std::find_if(std::begin(explanations), std::end(explanations),
      [&](const auto& entry) { return Equal(option, entry.key); });
  return found == std::end(explanations) ? std::wstring_view{} : found->text;
}

std::wstring NestedParameterChoices(std::wstring_view command, std::wstring_view option, std::wstring_view version) {
  return ChoicesForBit(command, option, VersionBit(version));
}

std::vector<std::wstring_view> NestedParameterValues(std::wstring_view command, std::wstring_view option, std::wstring_view version) {
  const auto bit = VersionBit(version);
  std::vector<std::wstring_view> result;
  const auto source_option = Equal(option, L"-SecondConfigurationType") ? L"-FirstConfigurationType" : option;
  for (const auto& item : option_choices)
    if (Equal(command, item.command) && Equal(source_option, item.option) && (item.versions & bit) &&
        std::find(result.begin(), result.end(), item.value) == result.end()) result.push_back(item.value);
  return result;
}

std::wstring_view NestedValueExplanation(std::wstring_view value) {
  static const Explanation meanings[] = {
      {L"ObjectNotEditable", L"объект поставщика недоступен для редактирования"},
      {L"ObjectIsEditableSupportEnabled", L"объект можно изменять, сохраняя поддержку"},
      {L"ObjectNotSupported", L"объект снимается с поддержки"},
      {L"MainConfiguration", L"основная редактируемая конфигурация"},
      {L"DBConfiguration", L"конфигурация, применённая к базе данных"},
      {L"VendorConfiguration", L"конфигурация поставщика"},
      {L"ExtensionConfiguration", L"редактируемая конфигурация расширения"},
      {L"ExtensionDBConfiguration", L"конфигурация расширения, применённая к базе данных"},
      {L"ConfigurationRepository", L"конфигурация из хранилища"},
      {L"ExtensionConfigurationRepository", L"конфигурация из хранилища расширения"},
      {L"File", L"конфигурация или расширение из файла"},
      {L"ByObjectName", L"сопоставление по именам объектов; значение по умолчанию"},
      {L"ByObjectIDs", L"сопоставление по внутренним идентификаторам объектов"},
      {L"Brief", L"краткий отчёт"}, {L"Full", L"полный отчёт"},
      {L"txt", L"текстовый документ"}, {L"mxl", L"табличный документ"},
      {L"Plain", L"плоская структура файлов"}, {L"Hierarchical", L"иерархическая структура файлов"},
      {L"ConfigurationPatch", L"подпись оперативного исправления конфигурации; значение по умолчанию"},
      {L"Copyright", L"подпись для защиты авторских прав и контроля неизменности"},
      {L"ReadOnly", L"просмотр хранилища"}, {L"LockObjects", L"захват объектов"},
      {L"ManageConfigurationVersions", L"управление составом версий"}, {L"Administration", L"администрирование хранилища"},
      {L"disable", L"не завершать сеансы принудительно; значение по умолчанию"},
      {L"force", L"принудительно завершать сеансы"},
      {L"-tcp", L"отладка по протоколу TCP/IP"}, {L"-http", L"отладка по протоколу HTTP"},
      {L"+", L"разрешить выбранный механизм"}, {L"-", L"запретить выбранный механизм"},
  };
  const auto found = std::find_if(std::begin(meanings), std::end(meanings), [&](const auto& item) { return Equal(value, item.key); });
  return found == std::end(meanings) ? std::wstring_view{} : found->text;
}

std::vector<std::wstring> ValidateNestedParameters(std::wstring_view text, const ParameterEditorContext& context) {
  std::vector<std::wstring> errors;
  const auto catalog = LaunchParameterCatalog();
  for (const auto& rule : command_conflict_rules) {
    if (!(rule.versions & VersionBit(context.version))) continue;
    const auto present = [&](std::wstring_view key) {
      const auto def = std::find_if(catalog.begin(), catalog.end(), [&](const auto& item) { return Equal(item.key, key); });
      return def != catalog.end() && !FindParameterFragments(text, *def).empty();
    };
    if (present(rule.first) && present(rule.second))
      errors.push_back(L"Несовместимы команды " + std::wstring(rule.first) + L" и " + std::wstring(rule.second) +
          L". Области для /IBCheckAndRepair задаются локальным параметром -Z:.");
  }
  for (const auto& command : LaunchParameterCatalog()) {
    if (!command.choices.empty()) continue;
    const auto parents = FindParameterFragments(text, command);
    if (parents.empty()) continue;
    const auto options = ParameterNestedOptions(command.key, context.version);
    if (parents.size() > 1) continue;  // Preserve ambiguous manual fragments.
    const auto syntax = DocumentedParameterSyntax(command.key, context.version);
    if (syntax.empty()) continue;
    auto primary = syntax.substr(std::min(command.key.size(), syntax.size()));
    const auto first_option = primary.find_first_of(L"[-");
    primary = primary.substr(0, first_option);
    const auto body = text.substr(parents.front().begin, parents.front().end - parents.front().begin);
    const auto tokens = TokenizeCommandArguments(body);
    if (tokens.empty() || !Equal(tokens.front().value, command.key)) continue;
    const auto bit = VersionBit(context.version);
    const auto primary_choices = ChoicesForBit(command.key, L"", bit);
    if (!primary_choices.empty() && command.kind == ParameterValueKind::reference) {
      auto modes = Fragments(body, {L"", L"", OptionStyle::space, true, 0, bit});
      std::erase_if(modes, [](const auto& item) { return item.ambiguous; });
      if (modes.size() > 1)
        errors.push_back(L"Для " + std::wstring(command.key) + L" укажите только один вариант: " + ReadableNames(primary_choices) + L".");
      for (const auto& mode : modes)
        if (!HasChoice(primary_choices, mode.value))
          errors.push_back(L"Недопустимое значение для " + std::wstring(command.key) + L". Допустимы: " + ReadableNames(primary_choices) + L".");
      // The syntax explicitly permits omitting the transport. Do not invent
      // a required operand or assume which transport an installed client uses.
      if (Equal(command.key, L"/debug") && AnyPresent(L"-attach", body, options) && !modes.empty() &&
          (modes.size() != 1 || !Equal(modes.front().value, L"-http")))
        errors.push_back(L"Для /debug параметр -attach применяется только с режимом -http.");
    }
    const auto grammar = std::find_if(std::begin(command_syntax), std::end(command_syntax),
        [&](const auto& item) { return Equal(command.key, item.command) && (item.versions & bit); });
    std::vector<const PresenceRule*> active_rules;
    for (const auto& rule : presence_rules)
      if (Equal(command.key, rule.command) && (rule.versions & bit) &&
          (rule.triggers.empty() || AnyPresent(rule.triggers, body, options))) active_rules.push_back(&rule);
    const auto rule_position = [&](const auto* rule) {
      const auto option = std::find_if(options.begin(), options.end(), [&](const auto& item) { return HasChoice(rule->needed, item.key); });
      return option == options.end() ? options.size() : static_cast<size_t>(option - options.begin());
    };
    std::stable_sort(active_rules.begin(), active_rules.end(), [&](const auto* left, const auto* right) { return rule_position(left) < rule_position(right); });
    for (const auto* rule : active_rules) {
      if (!AnyPresent(rule->needed, body, options))
        errors.push_back(L"Для " + std::wstring(command.key) + L" требуется " + ReadableNames(rule->needed) + L".");
    }
    for (const auto& rule : conflict_rules) {
      if (Equal(command.key, rule.command) && (rule.versions & bit) &&
          AnyPresent(rule.first, body, options) && AnyPresent(rule.second, body, options))
        errors.push_back(L"Для " + std::wstring(command.key) + L" несовместимы " + std::wstring(rule.first) + L" и " + std::wstring(rule.second) + L".");
    }
    std::vector<std::wstring_view> unavailable;
    for (const auto& known : option_syntax) {
      if (!Equal(command.key, known.command) || std::any_of(options.begin(), options.end(),
          [&](const auto& item) { return Equal(item.key, known.key); }) ||
          std::any_of(unavailable.begin(), unavailable.end(), [&](const auto key) { return Equal(key, known.key); })) continue;
      if (!Fragments(body, {known.key, known.syntax, known.style, known.value_optional}).empty()) {
        unavailable.push_back(known.key);
        errors.push_back(L"Параметр " + std::wstring(known.key) + L" команды " + std::wstring(command.key) +
            L" отсутствует в справочнике выбранной версии.");
      }
    }
    size_t operands = 0;
    std::vector<OptionFragment> consumed;
    for (const auto& option : options) {
      const auto found = Fragments(body, option);
      consumed.insert(consumed.end(), found.begin(), found.end());
      // [-name] <label>: the spelling of the switch is optional, the label is not.
      if (Equal(command.key, L"/ConfigurationRepositorySetLabel") && Equal(option.key, L"-name"))
        operands += static_cast<size_t>(std::count_if(found.begin(), found.end(), [](const auto& item) { return item.has_value && !item.value.empty(); }));
    }
    for (size_t i = 1; i < tokens.size(); ++i) {
      const auto& token = tokens[i];
      if (!token.quoted && (token.value.starts_with(L"-") || token.value.starts_with(L"/"))) continue;
      if (std::none_of(consumed.begin(), consumed.end(), [&](const auto& item) { return token.begin >= item.begin && token.end <= item.end; })) ++operands;
    }
    const bool load_archive = Equal(command.key, L"/LoadConfigFromFiles") && AnyPresent(L"-Archive", body, options);
    if (!load_archive && grammar != std::end(command_syntax) && operands < grammar->primary_required)
      errors.push_back(L"Для " + std::wstring(command.key) + L" требуются основные аргументы." +
          (primary.empty() ? std::wstring{} : L" " + std::wstring(primary)));
    if (load_archive && operands)
      errors.push_back(L"Для /LoadConfigFromFiles укажите каталог загрузки или -Archive, одновременно они недопустимы.");
    for (const auto& option : options) {
      const auto found = Fragments(body, option);
      for (const auto& fragment : found) {
        if (option.style != OptionStyle::flag && !option.value_optional &&
            (!fragment.has_value || (fragment.value.empty() && !Equal(option.key, L"-Pwd") && !Equal(option.key, L"-PPwd"))))
          errors.push_back(L"Для " + std::wstring(command.key) + L" " + std::wstring(option.key) + L" требуется значение.");
        const auto choices = NestedParameterChoices(command.key, option.key, context.version);
        if (!choices.empty() && (fragment.has_value || !option.value_optional) && !HasChoice(choices, fragment.value))
          errors.push_back(L"Недопустимое значение " + std::wstring(option.key) + L" для " + std::wstring(command.key) + L".");
        if (Equal(option.key, L"-MinPasswordLenght") && !fragment.value.empty() &&
            !std::all_of(fragment.value.begin(), fragment.value.end(), [](wchar_t c) { return std::iswdigit(c) != 0; }))
          errors.push_back(L"Для -MinPasswordLenght требуется неотрицательное целое число.");
      }
    }
  }
  return errors;
}

std::optional<std::wstring> ReadNestedParameter(std::wstring_view text,
    const ParameterDefinition& command, const NestedParameterDefinition& option) {
  const auto parents = FindParameterFragments(text, command);
  if (parents.size() > 1) throw std::invalid_argument("Repeated parent command.");
  if (parents.empty()) return std::nullopt;
  const auto& parent = parents.front();
  RequireSeparateParent(text.substr(parent.begin, parent.end - parent.begin), command.key);
  if (option.profile) static_cast<void>(OrderOptions(text.substr(parent.begin, parent.end - parent.begin), command, OptionsForBit(command.key, option.profile)));
  const auto found = Fragments(text.substr(parent.begin, parent.end - parent.begin), option);
  if (found.size() > 1 || std::any_of(found.begin(), found.end(), [](const auto& item) { return item.ambiguous; }))
    throw std::invalid_argument("Ambiguous nested option.");
  return found.empty() ? std::nullopt : std::optional<std::wstring>(found.front().value);
}

std::wstring SetNestedParameter(std::wstring_view text, const ParameterDefinition& command,
    const NestedParameterDefinition& option, std::wstring_view value, bool enabled) {
  if (enabled && option.style != OptionStyle::flag && !option.value_optional && value.empty() &&
      !Equal(option.key, L"-Pwd") && !Equal(option.key, L"-PPwd"))
    throw std::invalid_argument("A nested parameter value is required.");
  const auto allowed = ChoicesForBit(command.key, option.key, option.profile ? option.profile : VersionBit(L"8.5.1"));
  if (enabled && !allowed.empty() && !(value.empty() && option.value_optional && !option.key.empty()) && !HasChoice(allowed, value))
    throw std::invalid_argument("Invalid nested parameter value.");
  const auto parents = FindParameterFragments(text, command);
  if (parents.size() > 1) throw std::invalid_argument("Repeated parent command.");
  std::wstring body = parents.empty() ? std::wstring(command.key) :
      std::wstring(text.substr(parents.front().begin, parents.front().end - parents.front().begin));
  RequireSeparateParent(body, command.key);
  const auto found = Fragments(body, option);
  if (found.size() > 1 || std::any_of(found.begin(), found.end(), [](const auto& item) { return item.ambiguous; }))
    throw std::invalid_argument("Ambiguous nested option.");
  std::wstring replacement = enabled ? std::wstring(option.key) : L"";
  if (enabled && option.key.empty() && allowed.empty() &&
      PrimaryOperandNames(command.key, option.profile ? option.profile : VersionBit(L"8.5.1")).size() == 1) {
    replacement = SinglePrimaryOperand(value);
  } else if (enabled && option.key.empty()) {
    for (const auto& token : TokenizeCommandArguments(value))
      if (!token.quoted && (token.value.starts_with(L"/") || token.value.starts_with(L"-")) && !HasChoice(allowed, token.value))
        throw std::invalid_argument("Primary operands must not contain unquoted switches.");
    replacement = value;
  } else if (enabled && option.style != OptionStyle::flag && (!value.empty() || !option.value_optional)) {
    const wchar_t* separator = option.style == OptionStyle::space ? L" " :
        option.style == OptionStyle::equals ? L"=" : option.style == OptionStyle::colon ? L":" : L"";
    const bool quoted = std::any_of(std::begin(quote_rules), std::end(quote_rules), [&](const auto& rule) {
      return Equal(command.key, rule.command) && Equal(option.key, rule.option) &&
          (rule.versions & (option.profile ? option.profile : VersionBit(L"8.5.1")));
    });
    replacement += separator + (HasChoice(allowed, value) ? std::wstring(value) :
        QuoteWindowsArgument(value, quoted || value.starts_with(L"/") || value.starts_with(L"-")));
  }
  if (!found.empty()) body.replace(found.front().begin, found.front().end - found.front().begin, replacement);
  else if (enabled && option.key.empty()) body.insert(command.key.size(), L" " + replacement);
  else if (enabled) body += L" " + replacement;
  else return std::wstring(text);
  if (option.profile) body = OrderOptions(body, command, OptionsForBit(command.key, option.profile));
  std::wstring result(text);
  if (parents.empty()) {
    if (!result.empty() && !std::iswspace(result.back())) result += L' ';
    result += body;
  } else result.replace(parents.front().begin, parents.front().end - parents.front().begin, body);
  return result;
}
}  // namespace ibstart::launcher
