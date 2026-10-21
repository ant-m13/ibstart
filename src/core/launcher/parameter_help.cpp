#include "core/launcher/parameter_help.hpp"
#include "core/launcher/parameter_catalog.hpp"
#include "core/launcher/parameter_options.hpp"

#include <algorithm>
#include <cwctype>

namespace ibstart::launcher {
namespace {
struct Explanation { std::wstring_view key, text; };
const Explanation explanations[] = {
#include "core/launcher/parameter_help.inc"
};
struct ChoiceExplanation { std::wstring_view key, value, text; };
const ChoiceExplanation choices[] = {
    {L"/SetPredefinedDataUpdate", L"-Auto", L"Режим по умолчанию. Для главного узла при реструктуризации используется -UpdateAutomatically, для периферийного — -DoNotUpdateAutomatically."},
    {L"/SetPredefinedDataUpdate", L"-UpdateAutomatically", L"При реструктуризации автоматически создаются новые предопределённые элементы и обновляются значения существующих."},
    {L"/SetPredefinedDataUpdate", L"-DoNotUpdateAutomatically", L"При реструктуризации предопределённые элементы не создаются и их значения не обновляются автоматически."},
    {L"/O", L"Normal", L"Обычная скорость соединения."},
    {L"/O", L"Low", L"Низкая скорость соединения."},
    {L"/TComp", L"-None", L"Передавать трафик без сжатия."},
    {L"/TComp", L"-Deflate", L"Сжимать данные алгоритмом deflate, применяемым в HTTP."},
    {L"/TComp", L"-SDC", L"Применять собственный алгоритм сжатия платформы."},
    {L"/TComp", L"-lz4", L"Использовать алгоритм LZ4."},
    {L"/TComp", L"-zstd", L"Использовать алгоритм Zstandard."},
    {L"/SLev", L"0", L"Передавать данные без защиты соединения."},
    {L"/SLev", L"1", L"Защищать соединение во время аутентификации."},
    {L"/SLev", L"2", L"Защищать соединение на протяжении всего сеанса."},
    {L"/AppArch", L"x86", L"Запускать только 32-разрядную платформу."},
    {L"/AppArch", L"x86_prt", L"Предпочитать 32-разрядную платформу, допуская 64-разрядную."},
    {L"/AppArch", L"x86_64", L"Запускать только 64-разрядную платформу."},
    {L"/AppArch", L"x86_64_prt", L"Предпочитать 64-разрядную платформу, допуская 32-разрядную."},
    {L"/MainWindowMode", L"-Normal", L"Обычное основное окно приложения."},
    {L"/MainWindowMode", L"-Workplace", L"Окно в режиме рабочего места."},
    {L"/MainWindowMode", L"-EmbeddedWorkplace", L"Рабочее место веб-клиента, встроенное в другую страницу."},
    {L"/MainWindowMode", L"-FullscreenWorkplace", L"Рабочее место на весь экран."},
    {L"/MainWindowMode", L"-Kiosk", L"Режим киоска с ограниченным доступом к интерфейсу."},
    {L"/Theme", L"-Dark", L"Тёмное оформление интерфейса."},
    {L"/Theme", L"-Light", L"Светлое оформление интерфейса."},
    {L"/AllowExecuteScheduledJobs", L"-Off", L"Отключить выполнение регламентных заданий файловой базы."},
    {L"/AllowExecuteScheduledJobs", L"-Force", L"Принудительно разрешить выполнение регламентных заданий файловой базы."},
    {L"/WA", L"+", L"Обязательное применение аутентификации пользователя базы средствами ОС; действует по умолчанию."},
    {L"/WA", L"-", L"Не использовать аутентификацию пользователя базы средствами ОС."},
    {L"/WSA", L"+", L"Принудительная аутентификация ОС на веб-сервере; действует по умолчанию."},
    {L"/WSA", L"-", L"Отключить аутентификацию ОС на веб-сервере."},
    {L"/OIDA", L"+", L"Разрешить аутентификацию через OpenID."},
    {L"/OIDA", L"-", L"Отключить аутентификацию через OpenID."},
    {L"/UseHwLicenses", L"+", L"Искать аппаратную лицензию для клиентского приложения."},
    {L"/UseHwLicenses", L"-", L"Не использовать аппаратные лицензии при этом запуске."},
    {L"/AppAutoCheckVersion", L"+", L"Разрешить стартеру 1С подбирать версию платформы; действует также при команде без знака."},
    {L"/AppAutoCheckVersion", L"-", L"Отключить автоматический подбор версии стартером 1С."},
    {L"/AppAutoInstallLastVersion", L"+", L"Разрешить автоматическую установку новых версий клиента стартером 1С."},
    {L"/AppAutoInstallLastVersion", L"-", L"Отключить автоматическую установку новых версий клиента стартером 1С."},
    {L"/DisableLocalSpeechToText", L"+", L"Запретить локальное распознавание речи; тот же результат даёт команда без знака режима."},
    {L"/DisableLocalSpeechToText", L"-", L"Разрешить локальное распознавание речи; это режим по умолчанию."},
};
bool Equal(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}
struct ContextExplanation { std::wstring_view command, option, text; uint32_t versions; };
const ContextExplanation context_details[] = {
#include "core/launcher/parameter_context_help.inc"
};
uint32_t ContextBit(std::wstring_view version) {
  const auto versions = LaunchParameterVersions();
  const auto profile = std::find(versions.begin(), versions.end(), ParameterVersionFamily(version));
  return profile == versions.end() ? 0 : uint32_t{1} << static_cast<unsigned>(profile - versions.begin());
}
void AppendContextNotes(std::wstring& text, std::wstring_view command, std::wstring_view version) {
  const auto bit = ContextBit(version);
  for (const auto& entry : context_details)
    if (Equal(command, entry.command) && entry.option.empty() && (entry.versions & bit))
      text += L"\r\n\r\n" + std::wstring(entry.text);
}
}  // namespace

std::wstring_view ContextParameterExplanation(std::wstring_view command, std::wstring_view option, std::wstring_view version) {
  const auto bit = ContextBit(version);
  const auto found = std::find_if(std::begin(context_details), std::end(context_details), [&](const auto& entry) {
    return Equal(command, entry.command) && Equal(option, entry.option) && (entry.versions & bit);
  });
  return found == std::end(context_details) ? std::wstring_view{} : found->text;
}

std::wstring_view ParameterExplanation(std::wstring_view key) {
  const auto item = std::find_if(std::begin(explanations), std::end(explanations),
      [&](const auto& entry) { return Equal(key, entry.key); });
  return item == std::end(explanations) ? std::wstring_view{} : item->text;
}

std::wstring_view ParameterValueExplanation(std::wstring_view command, std::wstring_view value) {
  const auto found = std::find_if(std::begin(choices), std::end(choices), [&](const auto& item) {
    return Equal(command, item.key) && Equal(value, item.value);
  });
  return found == std::end(choices) ? std::wstring_view{} : found->text;
}

std::wstring BuiltinParameterHelp(const ParameterDefinition& def, std::wstring_view version) {
  const auto values = ParameterChoicesForVersion(def, version);
  const auto syntax = DocumentedParameterSyntax(def.key, version);
  const auto nested = ParameterNestedOptions(def.key, version);
  std::wstring text = !syntax.empty() ? std::wstring(syntax) :
      def.choices.empty() || def.kind == ParameterValueKind::signed_switch ? std::wstring(def.syntax) :
      std::wstring(def.key) + (values.empty() ? L"" : L" <" + values + L">");
  text += L"\r\n\r\n";
  const auto family = ParameterVersionFamily(version);
  const auto explanation = Equal(def.key, L"/AppAutoCheckVersion") && (family == L"8.3.6" || family == L"8.3.7") ?
      std::wstring_view(L"Разрешает стартеру 1С автоматически подбирать версию платформы для базы. В выбранном справочнике команда описана без знака режима.") :
      ParameterExplanation(def.key);
  text += explanation.empty() ? def.description : explanation;
  AppendContextNotes(text, def.key, version);
  if (Equal(def.key, L"/LoadConfigFromFiles") && std::any_of(nested.begin(), nested.end(),
      [](const auto& option) { return Equal(option.key, L"-Format"); }))
    text += L"\r\n\r\nПолная загрузка: платформа сама распознаёт линейную или иерархическую организацию файлов."
        L"\r\nЧастичная загрузка: автоматического распознавания нет, формат необходимо задать явно параметром -Format.";
  else if (Equal(def.key, L"/LoadConfigFromFiles") && ParameterVersionFamily(version) == L"8.5.4")
    text += L"\r\n\r\nВ справочнике 8.5.4 есть расхождение: вводный текст упоминает -Format для частичной загрузки, "
        L"но в синтаксисе и перечне опций этот параметр отсутствует. Конструктор не добавляет его в профиль 8.5.4.";
  if (!nested.empty() && def.choices.empty()) {
    text += L"\r\n\r\nПараметры команды:";
    const auto primary_values = NestedParameterValues(def.key, L"", version);
    if (!primary_values.empty()) {
      text += L"\r\n\r\n• " + ParameterPrimaryOperandLabel(def.key, version) +
          (NestedParameterRequired(def.key, L"", version, L"") ? L"\r\n  Обязательное значение. Выберите один вариант." :
                                                               L"\r\n  Необязательное значение. Выберите один вариант.");
      for (const auto value : primary_values)
        text += L"\r\n    " + std::wstring(value) + L" — " + std::wstring(NestedValueExplanation(value)) + L".";
    }
    for (const auto& option : nested) {
      text += L"\r\n\r\n• " + std::wstring(option.syntax) + L"\r\n  " +
          std::wstring(NestedParameterExplanation(def.key, option.key, version));
      if (const auto requirement = NestedParameterRequirement(def.key, option.key, version); !requirement.empty())
        text += L"\r\n  " + requirement;
      for (const auto value : NestedParameterValues(def.key, option.key, version))
        text += L"\r\n    " + std::wstring(value) + L" — " + std::wstring(NestedValueExplanation(value)) + L".";
    }
  }
  std::wstring_view remaining(values);
  while (!remaining.empty()) {
    const auto end = remaining.find(L'|');
    const auto value = remaining.substr(0, end);
    const auto item = std::find_if(std::begin(choices), std::end(choices), [&](const auto& entry) {
      return Equal(def.key, entry.key) && Equal(value, entry.value);
    });
    if (item != std::end(choices)) {
      const auto detail = ContextParameterExplanation(def.key, value, version);
      text += L"\r\n\r\n• " + std::wstring(value) + L"\r\n  " + std::wstring(detail.empty() ? item->text : detail);
    }
    if (end == std::wstring_view::npos) break;
    remaining.remove_prefix(end + 1);
  }
  if (Equal(def.key, L"/TComp") && ParameterVersionAvailability(def.key, version) == ParameterAvailability::available)
    text += values.find(L"-zstd") != std::wstring::npos ?
        L"\r\n\r\nПо умолчанию: -zstd (Zstandard)." : L"\r\n\r\nПо умолчанию: -SDC.";
  if (!def.note.empty()) text += L"\r\n\r\n" + std::wstring(def.note);
  return text;
}
}  // namespace ibstart::launcher
