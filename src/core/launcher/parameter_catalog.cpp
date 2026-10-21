#include "core/launcher/parameter_catalog.hpp"

#include "core/launcher/command_builder.hpp"
#include "core/launcher/parameter_help.hpp"
#include "core/launcher/parameter_options.hpp"
#include "core/platform/platform_version.hpp"

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <map>
#include <stdexcept>

namespace ibstart::launcher {
namespace {
using Kind = ParameterValueKind;

// Own concise descriptions; identifiers describe the external CLI interface.
const ParameterDefinition catalog[] = {
    {L"/F", L"/F <каталог>", L"Каталог файловой базы.", L"Подключение", Kind::text},
    {L"/S", L"/S <сервер\\база>", L"Адрес серверной базы.", L"Подключение", Kind::text},
    {L"/WS", L"/WS <URL>", L"Адрес опубликованной базы.", L"Подключение", Kind::text},
    {L"/IBName", L"/IBName <имя>", L"Выбор базы по имени из списка.", L"Подключение", Kind::text},
    {L"/IBConnectionString", L"/IBConnectionString <строка>", L"Подключение через полную строку соединения.", L"Подключение", Kind::text},
    {L"/O", L"/O <Normal|Low>", L"Скорость соединения с базой.", L"Подключение", Kind::choice, L"Normal|Low"},
    {L"/TComp", L"/TComp <-None|-Deflate|-SDC|-lz4|-zstd>", L"Алгоритм сжатия сетевого трафика.", L"Подключение", Kind::choice, L"-None|-Deflate|-SDC|-lz4|-zstd"},
    {L"UsePrivilegedMode", L"UsePrivilegedMode", L"Запрос привилегированного сеанса.", L"Подключение"},
    {L"/SLev", L"/SLev <0|1|2>", L"Уровень защиты соединения с сервером.", L"Подключение", Kind::choice, L"0|1|2"},
    {L"/Z", L"/Z <значения>", L"Значения разделителей данных.", L"Подключение", Kind::text},
    {L"/N", L"/N <имя>", L"Имя пользователя информационной базы.", L"Аутентификация", Kind::text, {}, L"Не дублируйте пользователя, указанного отдельным полем окна запуска."},
    {L"/P", L"/P <пароль>", L"Пароль пользователя информационной базы.", L"Аутентификация", Kind::text, {}, L"Значение видно в исходной строке; в журнале и предпросмотре команды оно маскируется."},
    {L"/ModifyPassword", L"/ModifyPassword <пароль>", L"Новый пароль для интерактивного изменения.", L"Аутентификация", Kind::text},
    {L"/WA", L"/WA+ или /WA-", L"Использование аутентификации операционной системы.", L"Аутентификация", Kind::signed_switch, L"+|-"},
    {L"/WSA", L"/WSA+ или /WSA-", L"Аутентификация ОС на веб-сервере.", L"Аутентификация", Kind::signed_switch, L"+|-"},
    {L"/WSN", L"/WSN <имя>", L"Пользователь для аутентификации на веб-сервере.", L"Аутентификация", Kind::text},
    {L"/WSP", L"/WSP <пароль>", L"Пароль для аутентификации на веб-сервере.", L"Аутентификация", Kind::text},
    {L"/NoProxy", L"/NoProxy", L"Подключение к веб-серверу без прокси.", L"Аутентификация", Kind::flag, {}, {}, 3, false, L"proxy"},
    {L"/Proxy", L"/Proxy -PSrv <сервер> -PPort <порт> [-PUser <имя>] [-PPwd <пароль>]", L"Настройки прокси-сервера и его аутентификации.", L"Аутентификация"},
    {L"/OIDA", L"/OIDA+ или /OIDA-", L"Использование OpenID-аутентификации.", L"Аутентификация", Kind::signed_switch, L"+|-"},
    {L"/AccessToken", L"/AccessToken <токен>", L"Токен доступа для аутентификации.", L"Аутентификация", Kind::text},
    {L"/Authoff", L"/Authoff", L"Завершение сохранённой веб-аутентификации.", L"Аутентификация", Kind::flag},
    {L"/SAOnRestart", L"/SAOnRestart", L"Повторный запрос аутентификации после перезапуска.", L"Аутентификация", Kind::flag},
    {L"/ResetSavedAuth", L"/ResetSavedAuth", L"Удаление сохранённых данных аутентификации.", L"Аутентификация", Kind::flag},
    {L"/EmailAuth", L"/EmailAuth <адрес>", L"Интерактивная аутентификация по электронной почте.", L"Аутентификация"},
    {L"/AppAutoCheckVersion", L"/AppAutoCheckVersion[+|-]", L"Автоматическое определение версии стартером 1С.", L"Клиент и режим", Kind::signed_switch, L"+|-"},
    {L"/AppAutoCheckMode", L"/AppAutoCheckMode", L"Автоматическое определение приложения стартером 1С.", L"Клиент и режим"},
    {L"/RunModeOrdinaryApplication", L"/RunModeOrdinaryApplication", L"Открытие толстого клиента в обычном режиме.", L"Клиент и режим", Kind::flag, {}, {}, 1, true, L"application"},
    {L"/RunModeManagedApplication", L"/RunModeManagedApplication", L"Открытие толстого клиента в управляемом режиме.", L"Клиент и режим", Kind::flag, {}, {}, 1, true, L"application"},
    {L"/AppArch", L"/AppArch <разрядность>", L"Выбор разрядности платформы или её приоритета.", L"Клиент и режим", Kind::choice, L"x86|x86_prt|x86_64|x86_64_prt", L"Имеет приоритет над разрядностью в настройках базы; явная установка платформы должна ему соответствовать."},
    {L"/MainWindowMode", L"/MainWindowMode <режим>", L"Режим основного окна клиентского приложения.", L"Клиент и режим", Kind::choice, L"-Normal|-Workplace|-EmbeddedWorkplace|-FullscreenWorkplace|-Kiosk", {}, 1},
    {L"/HttpsCert", L"/HttpsCert <опции сертификата>", L"Выбор клиентского сертификата HTTPS.", L"Сертификаты и TLS"},
    {L"/HttpsCA", L"/HttpsCA <опции хранилища>", L"Доверенные центры сертификации HTTPS.", L"Сертификаты и TLS"},
    {L"/HttpsForceTLS1_0", L"/HttpsForceTLS1_0", L"Принудительное использование TLS 1.0.", L"Сертификаты и TLS", Kind::flag, {}, L"Используется устаревшая версия TLS.", 3, false, L"tls"},
    {L"/HttpsForceTLS1_1", L"/HttpsForceTLS1_1", L"Принудительное использование TLS 1.1.", L"Сертификаты и TLS", Kind::flag, {}, L"Используется устаревшая версия TLS.", 3, false, L"tls"},
    {L"/HttpsForceTLS1_2", L"/HttpsForceTLS1_2", L"Принудительное использование TLS 1.2.", L"Сертификаты и TLS", Kind::flag, {}, {}, 3, false, L"tls"},
    {L"/i85", L"/i85", L"Выбор интерфейса поколения 8.5.", L"Интерфейс", Kind::flag, {}, {}, 1, false, L"interface"},
    {L"/Theme", L"/Theme <-Dark|-Light>", L"Цветовая тема поддерживающего её клиента.", L"Интерфейс", Kind::choice, L"-Dark|-Light", {}, 1},
    {L"/iTaxi", L"/iTaxi", L"Выбор интерфейса Такси.", L"Интерфейс", Kind::flag, {}, {}, 1, false, L"interface"},
    {L"/itdi", L"/itdi", L"Использование вкладок в основном окне.", L"Интерфейс", Kind::flag, {}, {}, 1, false, L"interface"},
    {L"/TechnicalSpecialistMode", L"/TechnicalSpecialistMode", L"Доступ к функциям технического специалиста.", L"Интерфейс", Kind::flag, {}, {}, 1},
    {L"/L", L"/L <код языка>", L"Язык интерфейса платформы, например ru или en.", L"Интерфейс", Kind::text},
    {L"/VL", L"/VL <код локализации>", L"Локализация сеанса для форматирования значений.", L"Интерфейс", Kind::text},
    {L"/debug", L"/debug [<режим>] [-attach]", L"Режим отладки клиентского приложения.", L"Отладка и тестирование"},
    {L"/debuggerURL", L"/debuggerURL <URL>", L"Адрес отладчика для подключения клиента.", L"Отладка и тестирование", Kind::text},
    {L"DisplayPerformance", L"DisplayPerformance", L"Отображение показателей производительности.", L"Отладка и тестирование"},
    {L"/EmulateServerCallDelay", L"/EmulateServerCallDelay [-Call n] [-Send n] [-Recevie n]", L"Имитация задержек серверных вызовов.", L"Отладка и тестирование"},
    {L"/TestManager", L"/TestManager", L"Запуск клиента как менеджера автоматического тестирования.", L"Отладка и тестирование", Kind::flag, {}, {}, 1},
    {L"/TestClient", L"/TestClient [-TPort n] [-TestClientID id] [-TURL=url]", L"Запуск клиента под управлением менеджера тестирования.", L"Отладка и тестирование"},
    {L"/UILogRecorder", L"/UILogRecorder [-TPort n] [-File <путь>]", L"Запись действий пользовательского интерфейса.", L"Отладка и тестирование"},
    {L"/EnableCheckModal", L"/EnableCheckModal", L"Контроль использования модальных вызовов.", L"Проверки", Kind::flag, {}, {}, 1},
    {L"/EnableCheckExtensionsAndAddInsSyncCalls", L"/EnableCheckExtensionsAndAddInsSyncCalls", L"Контроль синхронных вызовов расширений и внешних компонент.", L"Проверки", Kind::flag, {}, {}, 1},
    {L"/EnableCheckServerCalls", L"/EnableCheckServerCalls", L"Контроль серверных вызовов.", L"Проверки", Kind::flag, {}, {}, 1},
    {L"/EnableCheckScriptCircularRefs", L"/EnableCheckScriptCircularRefs", L"Контроль циклических ссылок во встроенном языке.", L"Проверки", Kind::flag, {}, {}, 1},
    {L"/C", L"/C <строка>", L"Параметры, передаваемые прикладному решению одной строкой.", L"Прочие", Kind::text},
    {L"/ClearCache", L"/ClearCache", L"Очистка кэша средствами платформы.", L"Прочие"},
    {L"/AllowExecuteScheduledJobs", L"/AllowExecuteScheduledJobs <-Off|-Force>", L"Управление выполнением регламентных заданий файловой базы.", L"Прочие", Kind::choice, L"-Off|-Force", L"-Force принудительно разрешает выполнение заданий.", 1},
    {L"/UC", L"/UC <код>", L"Код разрешения входа при блокировке соединений.", L"Прочие", Kind::text},
    {L"/RunShortcut", L"/RunShortcut <файл>", L"Открытие списка или ярлыка баз.", L"Прочие"},
    {L"/AppAutoInstallLastVersion", L"/AppAutoInstallLastVersion<режим>", L"Установка версии средствами стартера 1С.", L"Прочие", Kind::signed_switch, L"+|-"},
    {L"/Execute", L"/Execute <обработка>", L"Запуск внешней обработки после открытия базы.", L"Прочие"},
    {L"/URL", L"/URL <ссылка>", L"Навигация по ссылке; требует отдельного разрешения подключения.", L"Прочие"},
    {L"/@", L"/@ <файл>", L"Чтение команды из файла с особым порядком аргументов.", L"Прочие"},
    {L"/Out", L"/Out <файл> [-NoTruncate]", L"Файл служебных сообщений платформы.", L"Прочие", Kind::file, {}, L"Без -NoTruncate существующий файл может быть перезаписан.", 3, false, {}, L"-NoTruncate"},
    {L"/DisableStartupMessages", L"/DisableStartupMessages", L"Подавление информационных сообщений при запуске.", L"Прочие", Kind::flag},
    {L"/DisableStartupDialogs", L"/DisableStartupDialogs", L"Подавление диалогов при запуске.", L"Прочие", Kind::flag},
    {L"/DisableSplash", L"/DisableSplash", L"Запуск без заставки платформы.", L"Прочие", Kind::flag},
    {L"/DisableUnrecoverableErrorMessage", L"/DisableUnrecoverableErrorMessage", L"Подавление сообщения о невосстановимой ошибке.", L"Прочие", Kind::flag},
    {L"/DisableHomePageForms", L"/DisableHomePageForms", L"Отключение открытия форм начальной страницы.", L"Прочие", Kind::flag, {}, {}, 1},
    {L"/DisableBackgroundIndexBuild", L"/DisableBackgroundIndexBuild", L"Отключение фонового построения индекса конфигуратора.", L"Прочие", Kind::flag, {}, {}, 2},
    {L"/UseHwLicenses", L"/UseHwLicenses+ или /UseHwLicenses-", L"Использование аппаратных лицензий.", L"Прочие", Kind::signed_switch, L"+|-"},
    {L"/DisplayUserNotificationList", L"/DisplayUserNotificationList", L"Отображение списка уведомлений пользователя.", L"Прочие", Kind::flag, {}, {}, 1},
#include "core/launcher/parameter_reference.inc"
};

struct VersionPresence { std::wstring_view key; uint32_t versions; };
struct VersionChoice { std::wstring_view key; std::wstring_view value; uint32_t versions; };
#include "core/launcher/parameter_versions.inc"

bool Equal(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

using Token = CommandArgument;

bool AcceptsEmptyValue(const ParameterDefinition& def) {
  return def.key == L"/C" || def.key == L"/P" || def.key == L"/WSP" || def.key == L"/ConfigurationRepositoryP" ||
      def.key == L"/SetPredefinedDataUpdate" || def.key == L"/TComp" || def.key == L"/SLev" ||
      def.key == L"/DisableLocalSpeechToText" || def.key == L"/AppAutoCheckVersion";
}

bool MayOmitValue(const ParameterDefinition& def) {
  return def.key == L"/SetPredefinedDataUpdate" || def.key == L"/TComp" || def.key == L"/SLev" ||
      def.key == L"/DisableLocalSpeechToText" || def.key == L"/AppAutoCheckVersion";
}

bool IsCommand(const Token& token) {
  return !token.quoted && !token.value.empty() && token.value.front() == L'/';
}

bool IsBareSwitchName(std::wstring_view value) {
  return Equal(value, L"UsePrivilegedMode") || Equal(value, L"DisplayPerformance");
}

bool IsBareSwitch(const std::vector<Token>& tokens, size_t index) {
  if (tokens[index].quoted || !IsBareSwitchName(tokens[index].value)) return false;
  // A bare switch spelling may also be a value. Recognize it only after a
  // complete simple command; unknown/compound operands remain untouched.
  size_t parent = index;
  while (parent && !IsCommand(tokens[parent - 1])) --parent;
  if (!parent) return true;
  --parent;
  const auto owner = std::find_if(std::begin(catalog), std::end(catalog),
      [&](const auto& item) { return Equal(item.key, tokens[parent].value); });
  if (owner == std::end(catalog)) return false;
  const bool flag = owner->kind == Kind::flag ||
      (owner->kind == Kind::reference && Equal(owner->syntax, owner->key));
  const bool value = owner->kind == Kind::text || owner->kind == Kind::file || owner->kind == Kind::choice;
  if (!flag && !value) return false;
  const size_t after_value = parent + (flag ? 1 : 2);
  if (index < after_value) return false;
  return std::all_of(tokens.begin() + static_cast<ptrdiff_t>(after_value),
      tokens.begin() + static_cast<ptrdiff_t>(index), [](const auto& token) {
        return !token.quoted && IsBareSwitchName(token.value);
      });
}

bool HasChoice(std::wstring_view choices, std::wstring_view value);

bool Matches(const Token& token, std::wstring_view text, const ParameterDefinition& def,
    std::wstring* attached) {
  if (token.quoted || token.value.size() < def.key.size()) return false;
  if (Equal(token.value, def.key)) { attached->clear(); return true; }
  if (!Equal(std::wstring_view(token.value).substr(0, def.key.size()), def.key)) return false;
  const auto suffix = std::wstring_view(token.value).substr(def.key.size());
  if (suffix.front() == L'=') { *attached = suffix.substr(1); return true; }
  if (def.kind == Kind::signed_switch && (suffix == L"+" || suffix == L"-")) {
    *attached = suffix; return true;
  }
  // These finite domains are documented without a separating space. Exact
  // values avoid treating future commands beginning with /O as operands.
  if ((Equal(def.key, L"/O") || Equal(def.key, L"/SLev")) && HasChoice(def.choices, suffix)) {
    *attached = suffix; return true;
  }
  const auto raw = text.substr(token.begin, token.end - token.begin);
  if (raw.size() > def.key.size() && raw[def.key.size()] == L'"') {
    *attached = suffix; return true;
  }
  return false;
}

bool HasChoice(std::wstring_view choices, std::wstring_view value) {
  while (!choices.empty()) {
    const size_t end = choices.find(L'|');
    if (Equal(choices.substr(0, end), value)) return true;
    if (end == std::wstring_view::npos) break;
    choices.remove_prefix(end + 1);
  }
  return false;
}
}  // namespace

std::span<const ParameterDefinition> LaunchParameterCatalog() { return catalog; }

std::vector<std::wstring> LaunchParameterVersions() {
  std::vector<std::wstring> versions;
  for (const auto version : verified_versions) versions.emplace_back(version);
  return versions;
}

std::wstring ParameterVersionFamily(std::wstring_view version) {
  size_t pos = 0;
  for (int part = 0; part < 3; ++part) {
    const size_t begin = pos;
    while (pos < version.size() && version[pos] >= L'0' && version[pos] <= L'9') ++pos;
    if (pos == begin) return {};
    if (part < 2) {
      if (pos == version.size() || version[pos] != L'.') return {};
      ++pos;
    }
  }
  if (pos != version.size() && version[pos] != L'.') return {};
  return std::wstring(version.substr(0, pos));
}

std::wstring ParameterDocumentationUrl(std::wstring_view version) {
  const auto family = ParameterVersionFamily(version);
  if (family.empty()) return {};
  auto compact = family;
  std::erase(compact, L'.');
  return L"https://its.1c.ru/db/v" + compact + L"doc/bookmark/adm/TI000000493";
}

std::wstring DefaultParameterReferenceVersion(std::wstring_view requested,
    std::span<const domain::PlatformInstallation> installations) {
  if (!ParameterVersionFamily(requested).empty()) return std::wstring(requested);
  const bool automatic = requested.empty() || Equal(requested, L"Авто") || Equal(requested, L"Auto");
  std::wstring newest;
  for (const auto& installation : installations) {
    if (ParameterVersionFamily(installation.version).empty()) continue;
    if (!automatic && !(installation.version.starts_with(requested) &&
        installation.version.size() > requested.size() && installation.version[requested.size()] == L'.')) continue;
    if (newest.empty() || platform::IsNewerVersion(installation.version, newest)) newest = installation.version;
  }
  if (!newest.empty()) return newest;
  return automatic ? L"8.5.1" : std::wstring(requested);
}

std::wstring ParameterFirstDocumentedVersion(std::wstring_view key) {
  const auto rule = std::find_if(std::begin(version_presence), std::end(version_presence),
      [&](const auto& item) { return Equal(item.key, key); });
  if (rule == std::end(version_presence)) return {};
  for (size_t i = 0; i < std::size(verified_versions); ++i)
    if (rule->versions & (uint32_t{1} << static_cast<unsigned>(i))) return std::wstring(verified_versions[i]);
  return {};
}

std::wstring ParameterDocumentedVersions(std::wstring_view key) {
  std::wstring result, first, last;
  const auto flush = [&] {
    if (first.empty()) return;
    if (!result.empty()) result += L", ";
    result += first;
    if (last != first) result += L"–" + last;
    first.clear(); last.clear();
  };
  for (const auto version : verified_versions) {
    if (ParameterVersionAvailability(key, version) != ParameterAvailability::available) { flush(); continue; }
    if (!last.empty()) {
      const auto dot = last.rfind(L'.');
      const std::wstring next = last.substr(0, dot + 1) + std::to_wstring(std::stoi(last.substr(dot + 1)) + 1);
      if (next != version) flush();
    }
    if (first.empty()) first = version;
    last = version;
  }
  flush();
  return result;
}

ParameterAvailability ParameterVersionAvailability(std::wstring_view key, std::wstring_view version) {
  const auto family = ParameterVersionFamily(version);
  const auto profile = std::find(std::begin(verified_versions), std::end(verified_versions), family);
  if (profile == std::end(verified_versions)) return ParameterAvailability::unverified;
  const auto rule = std::find_if(std::begin(version_presence), std::end(version_presence),
      [&](const auto& item) { return Equal(item.key, key); });
  if (rule == std::end(version_presence)) return ParameterAvailability::unverified;
  const uint32_t bit = uint32_t{1} << static_cast<unsigned>(profile - std::begin(verified_versions));
  return rule->versions & bit ? ParameterAvailability::available : ParameterAvailability::unavailable;
}

std::wstring ParameterChoicesForVersion(const ParameterDefinition& definition, std::wstring_view version) {
  const auto family = ParameterVersionFamily(version);
  const auto profile = std::find(std::begin(verified_versions), std::end(verified_versions), family);
  if (profile == std::end(verified_versions)) return std::wstring(definition.choices);
  const uint32_t bit = uint32_t{1} << static_cast<unsigned>(profile - std::begin(verified_versions));
  std::wstring choices;
  bool has_rules = false;
  for (const auto& rule : version_choices) {
    if (!Equal(rule.key, definition.key)) continue;
    has_rules = true;
    if (!(rule.versions & bit)) continue;
    if (!choices.empty()) choices += L'|';
    choices += rule.value;
  }
  return has_rules ? choices : std::wstring(definition.choices);
}

std::wstring ParameterContextNotice(const ParameterDefinition& def, const ParameterEditorContext& context) {
  if (!context.allow_connection_parameters &&
      (Equal(def.key, L"/F") || Equal(def.key, L"/S") || Equal(def.key, L"/WS") ||
       Equal(def.key, L"/IBName") || Equal(def.key, L"/IBConnectionString")))
    return L"Подключение задаётся в настройках базы. Эти параметры можно составлять в самостоятельном конструкторе из главного меню.";
  if (context.mode && !(def.modes & (*context.mode == domain::LaunchMode::designer ? 2 : 1)))
    return L"Команда не соответствует выбранному режиму запуска.";
  if (def.thick_only && context.client == domain::ClientType::thin)
    return L"Команда предназначена для толстого клиента.";
  return {};
}

ParameterDefinition ParameterEditorDefinition(const ParameterDefinition& definition, std::wstring_view version) {
  auto editor = definition;
  const auto syntax = DocumentedParameterSyntax(definition.key, version);
  const bool process_mode = Equal(definition.key, L"ENTERPRISE") || Equal(definition.key, L"DESIGNER") ||
      Equal(definition.key, L"CREATEINFOBASE");
  if (!process_mode && (definition.kind == Kind::reference || definition.kind == Kind::signed_switch) &&
      !syntax.empty() && Equal(syntax, definition.key) && ParameterNestedOptions(definition.key, version).empty()) {
    editor.kind = Kind::flag;
    editor.choices = {};
  }
  return editor;
}

std::wstring ParameterHelpText(const ParameterDefinition& def, const ParameterEditorContext& context) {
  std::wstring text = BuiltinParameterHelp(def, context.version);
  if (const auto notice = ParameterContextNotice(def, context); !notice.empty())
    text += L"\r\n\r\n" + notice;
  if (ParameterEditorDefinition(def, context.version).kind == Kind::reference && ParameterNestedOptions(def.key, context.version).empty() &&
      ParameterPrimaryOperandLabel(def.key, context.version).empty())
    text += L"\r\n\r\nВ конструкторе пока нет отдельных полей для параметров этой команды.";
  const auto availability = ParameterVersionAvailability(def.key, context.version);
  if (availability == ParameterAvailability::unavailable)
    text += L"\r\n\r\nКоманда отсутствует в справочнике выбранной версии.";
  else if (availability == ParameterAvailability::unverified &&
      std::find(std::begin(verified_versions), std::end(verified_versions), ParameterVersionFamily(context.version)) ==
          std::end(verified_versions))
    text += L"\r\n\r\nДля этой версии нет справочника. Выберите версию из списка.";
  const auto first = ParameterFirstDocumentedVersion(def.key);
  if (!first.empty() && (first != L"8.3.6" || availability == ParameterAvailability::unavailable))
    text += L"\r\n\r\nВпервые описана в доступных справочниках: " + first + L".";
  if (availability == ParameterAvailability::unavailable && !first.empty())
    text += L"\r\nОписана в версиях: " + ParameterDocumentedVersions(def.key) + L".";
  return text;
}

std::vector<ParameterFragment> FindParameterFragments(std::wstring_view text, const ParameterDefinition& def) {
  const auto tokens = TokenizeCommandArguments(text);
  std::vector<ParameterFragment> result;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (IsBareSwitchName(def.key) && !IsBareSwitch(tokens, i)) continue;
    std::wstring attached;
    if (!Matches(tokens[i], text, def, &attached)) continue;
    size_t end = i + 1;
    while (end < tokens.size() && !IsCommand(tokens[end]) && !IsBareSwitch(tokens, end)) ++end;
    ParameterFragment item;
    item.begin = tokens[i].begin;
    item.end = tokens[end - 1].end;
    item.value = attached;
    item.has_value = !Equal(tokens[i].value, def.key);
    size_t next = i + 1;
    if (def.kind != Kind::flag && def.kind != Kind::signed_switch && !item.has_value && next < end) {
      item.value = tokens[next++].value;
      item.has_value = true;
    }
    if (!def.option.empty() && next < end && Equal(tokens[next].value, def.option)) {
      item.option = true; ++next;
    }
    item.structured = def.kind != Kind::reference && next == end &&
        (def.kind != Kind::flag || attached.empty());
    result.push_back(std::move(item));
    i = end - 1;
  }
  return result;
}

std::wstring SetLaunchParameter(std::wstring_view text, const ParameterDefinition& def,
    std::wstring_view value, bool option) {
  if (def.kind == Kind::reference) throw std::invalid_argument("Reference parameter cannot be edited.");
  if (def.kind != Kind::flag && value.empty() && !AcceptsEmptyValue(def))
    throw std::invalid_argument("A parameter value is required.");
  if (!def.choices.empty() && !(value.empty() && MayOmitValue(def)) && !HasChoice(def.choices, value))
    throw std::invalid_argument("Invalid parameter choice.");
  const auto fragments = FindParameterFragments(text, def);
  if (fragments.size() > 1 || (!fragments.empty() && !fragments.front().structured))
    throw std::invalid_argument("Ambiguous or complex parameter fragment.");
  std::wstring replacement(def.key);
  if (def.kind == Kind::signed_switch) replacement += value;
  else if (def.kind != Kind::flag && !(value.empty() && MayOmitValue(def)))
    replacement += L" " + QuoteWindowsArgument(value, value.starts_with(L"/") || IsBareSwitchName(value));
  if (option && !def.option.empty()) replacement += L" " + std::wstring(def.option);
  std::wstring result(text);
  if (fragments.empty()) {
    if (!result.empty() && !std::iswspace(result.back())) result += L' ';
    result += replacement;
  } else {
    const auto& fragment = fragments.front();
    result.replace(fragment.begin, fragment.end - fragment.begin, replacement);
  }
  if (IsBareSwitchName(def.key)) {
    const auto inserted = FindParameterFragments(result, def);
    if (inserted.size() != 1 || !inserted.front().structured)
      throw std::invalid_argument("Bare switch position is ambiguous; use manual editing.");
  }
  return result;
}

std::wstring AddLaunchParameterCommand(std::wstring_view text, const ParameterDefinition& def,
    const ParameterEditorContext& context) {
  if ((!def.key.starts_with(L"/") && !IsBareSwitchName(def.key)) ||
      DocumentedParameterSyntax(def.key, context.version).empty() ||
      ParameterVersionAvailability(def.key, context.version) != ParameterAvailability::available ||
      !ParameterContextNotice(def, context).empty())
    throw std::invalid_argument("Command is unavailable in the selected context.");
  if (!FindParameterFragments(text, def).empty()) return std::wstring(text);
  const auto editor = ParameterEditorDefinition(def, context.version);
  if (editor.kind == Kind::flag) return SetLaunchParameter(text, editor, L"");
  std::wstring result(text);
  if (!result.empty() && !std::iswspace(result.back())) result += L' ';
  result += def.key;
  return result;
}

std::wstring RemoveLaunchParameter(std::wstring_view text, const ParameterDefinition& def) {
  if (def.kind == Kind::reference) throw std::invalid_argument("Reference parameter cannot be edited.");
  const auto fragments = FindParameterFragments(text, def);
  if (fragments.empty()) return std::wstring(text);
  if (fragments.size() != 1 || !fragments.front().structured)
    throw std::invalid_argument("Ambiguous or complex parameter fragment.");
  std::wstring result(text);
  result.erase(fragments.front().begin, fragments.front().end - fragments.front().begin);
  return result;
}

std::vector<std::wstring> ValidateParameterDraft(std::wstring_view text, const ParameterEditorContext& context) {
  std::vector<std::wstring> errors;
  try {
    static_cast<void>(SplitCommandArguments(text));
    std::map<std::wstring_view, std::wstring_view> groups;
    for (const auto& definition : catalog) {
      // Signed switches keep their original parser so an attached sign from
      // a newer syntax is still diagnosed in an older flag-only profile.
      const auto def = definition.kind == Kind::reference ?
          ParameterEditorDefinition(definition, context.version) : definition;
      const auto fragments = FindParameterFragments(text, def);
      if (fragments.empty()) continue;
      if (const auto notice = ParameterContextNotice(def, context); !notice.empty())
        errors.push_back(std::wstring(def.key) + L": " + notice);
      if (ParameterVersionAvailability(def.key, context.version) == ParameterAvailability::unavailable)
        errors.push_back(std::wstring(def.key) + L" не описана для выбранной версии платформы.");
      if (def.kind == Kind::reference) continue;  // Nested grammar is checked separately.
      if (fragments.size() > 1) errors.push_back(L"Команда " + std::wstring(def.key) + L" указана несколько раз.");
      for (const auto& fragment : fragments) {
        if (!def.choices.empty()) {
          const auto tokens = TokenizeCommandArguments(text.substr(fragment.begin, fragment.end - fragment.begin));
          const auto count = std::count_if(tokens.begin() + 1, tokens.end(), [&](const auto& token) { return HasChoice(def.choices, token.value); });
          if (count > 1) errors.push_back(L"Для " + std::wstring(def.key) + L" выберите только одно значение.");
        }
        if (!fragment.structured) continue;  // Preserve future or complex grammar.
        if (def.kind != Kind::flag && ((!fragment.has_value && !MayOmitValue(def)) ||
            (fragment.value.empty() && !AcceptsEmptyValue(def))))
          errors.push_back(L"Для " + std::wstring(def.key) + L" требуется значение.");
        else if (!def.choices.empty() && !(fragment.value.empty() && MayOmitValue(def)) &&
            !HasChoice(ParameterChoicesForVersion(def, context.version), fragment.value))
          errors.push_back(L"Недопустимый вариант значения " + std::wstring(def.key) + L".");
      }
      if (!def.conflict_group.empty()) {
        const auto [it, inserted] = groups.emplace(def.conflict_group, def.key);
        if (!inserted) errors.push_back(L"Несовместимые команды: " + std::wstring(it->second) + L" и " + std::wstring(def.key) + L".");
      }
    }
    const auto proxy_def = std::find_if(std::begin(catalog), std::end(catalog), [](const auto& def) { return def.key == L"/Proxy"; });
    const auto no_proxy_def = std::find_if(std::begin(catalog), std::end(catalog), [](const auto& def) { return def.key == L"/NoProxy"; });
    const bool proxy = !FindParameterFragments(text, *proxy_def).empty();
    const bool no_proxy = !FindParameterFragments(text, *no_proxy_def).empty();
    if (proxy && no_proxy) errors.push_back(L"Несовместимые команды: /Proxy и /NoProxy.");
    const auto nested_errors = ValidateNestedParameters(text, context);
    errors.insert(errors.end(), nested_errors.begin(), nested_errors.end());
  } catch (const std::exception&) {
    errors.push_back(L"Незакрытая кавычка в строке параметров.");
  }
  return errors;
}

}  // namespace ibstart::launcher
