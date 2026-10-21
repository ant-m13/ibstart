# Параметры и команды запуска 1С

Статус: основной документ по командной строке, batch, web и mobile.

Дата сверки с кодом и официальной документацией ИТС: 2026-10-20.

## Назначение

Документ описывает реестр параметров, который требуется для полной реализации
запуска 1С с сохранением обратной совместимости. Это собственная инженерная
спецификация: имена параметров приведены как идентификаторы внешнего интерфейса,
а описания написаны для проекта своими словами.

Структура файла `ibases.v8i` описана в
[документе формата](ibases-format.md). Связь полей файла с параметрами,
приоритеты и общий план реализации находятся в
[интеграционной спецификации](ibases-launch-spec.md).

## Источники

- [Приложение 3. Описание и расположение служебных файлов](https://its.1c.ru/db/v8326doc/content/109/hdoc)
- [Приложение 7. Параметры командной строки запуска «1С:Предприятия»](https://its.1c.ru/db/v8326doc/content/113/hdoc)
- [политика лицензирования материалов 1С:ИТС](1c-license-compliance.md)

Проверенные версии приложения 7 охватывают 8.3.6–8.3.27, стабильную
8.5.1 и тестовую 8.5.4; профилей 8.5.2 и 8.5.3 нет. Наличие команд и значений
по версиям отражено в [генерируемой матрице](launch-parameter-versions.md).
Различия определений между соседними проверенными версиями, включая команды
вне каталога конструктора, приведены в [журнале сверки](1c-reference-versions.md).
Порядок добавления следующей страницы описан в
[обновлении справочников](maintaining-1c-reference.md).
Основной реестр ниже содержит собственные краткие описания; для показа полного
пояснения каждой команды в конструкторе используется
[встроенная справка](configuration.md).
Параметр из нового справочника может отсутствовать в старом клиенте.

## 1. Статусы реализации

- `PASS` — передать в соответствующий клиент после разбора и проверки;
- `TYPED` — представить отдельным объектом доменной модели;
- `PRESERVE` — сохранить, но не обещать выполнение;
- `OUT` — отдельная операция вне обычного desktop launch;
- `SECRET` — значение должно маскироваться в log и автоматически создаваемых
  diagnostics/report; явный ввод, просмотр и копирование пользователем
  разрешены, а fixture должен использовать синтетическое или замаскированное
  значение.

Raw `AdditionalParameters` может временно сохранять неизвестные параметры, но
сама передача текста не считается полной реализацией: для заявленных параметров
нужны grammar, контекст, версия, precedence, redaction policy и тест.

## 2. Что уже реализовано

Текущий IBStart формирует `ENTERPRISE` или `DESIGNER`, затем базовое подключение
через `/F`, `/S`, `/WS` или fallback `/IBConnection`, после чего добавляет raw
аргументы. Выбор платформы учитывает версию, тип клиента, разрядность и наличие
тонкого клиента. Typed-спецификация подключения и реестр зарезервированных
параметров отклоняют конфликтующие `File`/`Srvr`/`Ref`/`WS`, повторные `/F`,
`/S`, `/WS`, `/AppArch`, `/Proxy`, `/NoProxy`, а также raw-режимы и альтернативные
`/URL`/`/IBConnection`; валидный `/AppArch` в `AdditionalParameters` имеет
приоритет над полем `AppArch`. Любое другое непустое значение поля `AppArch`,
как и невалидный или повторный `/AppArch`, диагностируется и блокирует запуск;
исходное значение поля сохраняется. Ошибка поля не скрывается валидным
`/AppArch` из дополнительных параметров.

Основные места текущей реализации:

- выбор launch context и источника параметров —
  [`MainWindow::LaunchSelected`](https://github.com/ant-m13/ibstart/blob/main/src/ui/main_window.cpp);
- построение executable и аргументов —
  [`launcher::BuildCommand`](https://github.com/ant-m13/ibstart/blob/main/src/core/launcher/command_builder.cpp);
- распознавание raw Windows-аргументов —
  [`launcher::SplitCommandArguments`](https://github.com/ant-m13/ibstart/blob/main/src/core/launcher/command_builder.cpp).

Общий конструктор вызывается из разового запуска и дополнительных настроек
базы. Он показывает команду, собственное описание, синтаксис выбранной версии,
редакторы основных аргументов и вложенных опций и редактируемую итоговую строку. Результат возвращается
в вызвавшее поле только по «Применить»; конструктор не сохраняет базу и не запускает
процесс. Неизвестные фрагменты не переписываются. Неоднозначные повторения и
конструкции, не поддержанные редакторами, сохраняются для ручного редактирования.

Команды без аргументов добавляются и удаляются одним действием. Кнопка
«Добавить команду в строку» вставляет имя команды отдельно от опций; обязательные
значения можно заполнить после вставки, но незавершённая строка не применяется.
Историческая `/DisplayAllFunctions` доступна в профилях 8.3.12–8.3.16;
в более новых профилях используется `/TechnicalSpecialistMode`.

![Конструктор со встроенной справкой команды TComp](images/launch-parameters-builder.png)

Команда **Инструменты → Конструктор параметров запуска…** открывает ту же форму
самостоятельно, без зависимости от выбранной базы. В этом режиме доступны
проверка по мере ввода, выбор версии и явное копирование строки; вместо применения
результата отображается «Закрыть». Параметры не записываются в файлы приложения.

Каталог наличия команд сверён по 24 разделам ИТС: 8.3.6–8.3.27, стабильная 8.5.1
и тестовая 8.5.4. Профили 8.5.2 и 8.5.3 не предусмотрены. Для структурированных
перечислений проверяются варианты значений; полный номер сборки сопоставляется
с точным семейством. Неизвестные версии не наследуют соседние профили.
Наличие в документации не является подтверждением работы установленного
клиента. Матрица и порядок актуализации находятся в
[отдельном документе](launch-parameter-versions.md).

Обозначение `<...>` задаёт место для значения, `[]` делает текст необязательным,
а `|` разделяет альтернативы в одной позиции. Значение может принадлежать самой
команде или её вложенной опции; наличие начального минуса само по себе не
делает его независимой опцией. Например, у `/debug [<режим>] [-attach]` поле
«режим» выбирает одно значение — `-tcp` или `-http`, а `-attach` задаётся отдельно.
Оба элемента необязательны; явное сочетание `-tcp -attach` несовместимо.
Если режим пропущен, конструктор не определяет фактический транспорт клиента.

Конечные списки значений вложенных опций показываются выбором по профилю
версии. Имена, пути, содержимое файлов и списки через запятую сохраняют
свободный ввод. Условия применения поля не становятся его допустимыми
значениями: например, `VendorConfiguration` в описании `-FirstName` определяет
тип источника, а само имя вводится пользователем.
Результаты ревизии обозначений и редакторов приведены в
[протоколе проверки](verification-launch-operand-domains.md).

Последующая [смысловая ревизия всех пунктов каталога](verification-launch-semantic-audit.md)
учитывает требования и ограничения из пояснений после синтаксиса:
обязательную операцию `/ManageCfgSupport`, выбор удаляемых расширений,
несовместимые опции, правила кавычек и различия версий. У
`/ConfigurationRepositoryAddUser -Rights` конструктор предлагает выбор одного
из четырёх значений; это согласованный способ ввода в редакторе.

Редакторы поддерживают и пакетные команды с описанными аргументами и опциями.
Полная грамматика всех конструкций и отдельные API для web/mobile/OLE/agent
пока не реализованы. Специальные разделы остаются справочными; неизвестные аргументы
сохраняются как raw-текст. Перед построением команды обычные известные параметры
дополнительно проверяются по фактически выбранной версии и контексту клиента.

## 3. Общие режимы процесса

| Режим | Каноническая форма | Целевой статус |
| --- | --- | --- |
| Предприятие | `1cv8 ENTERPRISE [параметры]` | `TYPED/PASS` |
| Конфигуратор | `1cv8 DESIGNER [команды]` | `TYPED`, отдельный batch API |
| Создание ИБ | `1cv8 CREATEINFOBASE <connection string>` | `OUT/TYPED` |

Обычный запуск, batch-операция, создание информационной базы, web URL, mobile,
OLE и agent mode должны быть разными `LaunchContext`. Их нельзя без проверки
складывать в одну строку.

## 4. Общие параметры запуска

### 4.1. Подключение

| Параметр | Назначение | Целевой статус |
| --- | --- | --- |
| `/F` | Каталог файловой информационной базы. | `TYPED`, из `File` |
| `/S` | Адрес клиент-серверной базы. | `TYPED`, version profile |
| `/IBName` | Имя базы в списке; неоднозначность должна быть ошибкой. | `TYPED`, resolver |
| `/IBConnectionString` | Полная строка подключения с важным порядком параметров. | `TYPED`, parser/precedence |
| `/WS` | URL web-подключения. | `TYPED`, из `WS` |
| `/O` | Скорость соединения `Normal` или `Low`. | `TYPED`, из `ClientConnectionSpeed` |
| `/TComp` | Режим сжатия: `-None`, `-Deflate`, `-SDC`, `-lz4`, `-zstd`. | `PASS` после whitelist |
| `UsePrivilegedMode` | Запуск привилегированного сеанса. | `PASS` с предупреждением |
| `/SLev` | Уровень защищённости server connection: 0, 1 или 2. | `TYPED/PASS` |
| `/Z` | Значения разделителей. | `TYPED/PASS` |

### 4.2. Аутентификация и proxy

| Параметр | Назначение | Целевой статус |
| --- | --- | --- |
| `/N` | Имя пользователя. | `PASS` |
| `/P` | Пароль. | `PASS`, `SECRET` |
| `/ModifyPassword` | Новый пароль в интерактивном сценарии. | `PASS`, `SECRET` |
| `/WA` | Аутентификация ОС на уровне базы. | `TYPED`, из `WA` |
| `/WSA` | Аутентификация ОС на web-сервере. | `TYPED`, из `WSA` |
| `/WSN` | Имя для аутентификации web-сервера. | `PASS` |
| `/WSP` | Пароль для аутентификации web-сервера. | `PASS`, `SECRET` |
| `/NoProxy` | Запрет proxy для web-подключения. | `TYPED`, конфликт с `/Proxy` |
| `/Proxy` | Вложенные `-PSrv`, `-PPort`, опциональные `-PUser`, `-PPwd`. | `TYPED`, `-PPwd` — `SECRET` |
| `/OIDA` | Включение или отключение OpenID-аутентификации. | `PASS/TYPED` |
| `/AccessToken` | Токен доступа. | `PASS`, `SECRET` |
| `/Authoff` | Отключение/завершение сохранённой web-аутентификации. | `PASS` |
| `/SAOnRestart` | Повторно требовать аутентификацию после перезапуска. | `PASS` |
| `/ResetSavedAuth` | Удалить сохранённые имена и токены. | `PASS`, warning |
| `/EmailAuth` | Адрес и код подтверждения в интерактивном сценарии. | `PASS`, interactive-only |

### 4.3. Режим, версия и клиент

| Параметр | Назначение | Целевой статус |
| --- | --- | --- |
| `/AppAutoCheckVersion` | Разрешение или запрет автоматического выбора/установки версии. | `TYPED` |
| `/AppAutoCheckMode` | Определение приложения по базе и настройкам пользователя. | `TYPED`, two-stage launch |
| `/RunModeOrdinaryApplication` | Обычное приложение толстого клиента. | `PASS`, thick-only |
| `/RunModeManagedApplication` | Управляемое приложение толстого клиента. | `PASS`, thick-only |
| `/AppArch` | `x86`, `x86_prt`, `x86_64`, `x86_64_prt`. | `TYPED`, частично реализовано |
| `/MainWindowMode` | `-Normal`, `-Workplace`, `-EmbeddedWorkplace`, `-FullscreenWorkplace`, `-Kiosk`. | Редактор перечисления; варианты по версии |

### 4.4. Сертификаты и TLS

Перечень ниже объединяет разные версии. `/HttpsNSS` найден в 8.3.6–8.3.7,
`/HttpsForceSSLv3` — в 8.3.6–8.3.20; они остаются историческими командами.
`/HttpsForceTLS1_0` найден с 8.3.9, `/HttpsForceTLS1_1` и
`/HttpsForceTLS1_2` — с 8.3.21. Отсутствие команды в более новой документации
не является проверкой её исполнения. Опции источников сертификатов нужно
сверять внутри определения нужной версии; таблица наличия команды не
подтверждает одинаковые вложенные опции во всех версиях.

| Параметр | Значения | Целевой статус |
| --- | --- | --- |
| `/HttpsCert` | `-windows`, `-linux`, `-macos`, `-recent`, `-auto`, `-choose`, `-file`, `-pwd`, `-none`. | `TYPED`, пароль — `SECRET` |
| `/HttpsCA` | `-windows`, `-linux`, `-macos`, `-file`, `-pwd`, `-none`. | `TYPED` |
| `/HttpsForceTLS1_0` | Принудительно использовать TLS 1.0. | `PASS` с security warning |
| `/HttpsForceTLS1_1` | Принудительно использовать TLS 1.1. | `PASS` с security warning |
| `/HttpsForceTLS1_2` | Принудительно использовать TLS 1.2. | `PASS` |

Нужно запрещать несколько одновременных `/HttpsForceTLS*`, проверять пути до
запуска и не копировать содержимое сертификатов в настройки или репозиторий.

### 4.5. Интерфейс и локализация

| Параметр | Целевой статус |
| --- | --- |
| `/i85` | `PASS` только для совместимого клиента 8.5 |
| `/Theme -Dark` или `/Theme -Light` | Редактор перечисления для 8.5.1 и тестовой 8.5.4 |
| `/AppCaption <строка>` | В документации тестовой версии 8.5.4: задаёт заголовок окна; поздний вызов `УстановитьЗаголовок()` его не заменяет |
| `/AppShortCaption <строка>` | В документации тестовой версии 8.5.4: задаёт краткую часть заголовка; учитывается приоритет над `УстановитьКраткийЗаголовок()` |
| `/AppCaptionFromInfobaseList` | В документации тестовой версии 8.5.4: берёт представление первой базы с совпавшим подключением; для веб-клиента не применяется |
| `/iTaxi` | `PASS` |
| `/itdi` | `PASS/PRESERVE`, требует version check |
| `/TechnicalSpecialistMode` | `PASS` |
| `/L <код языка>` | `TYPED`, не смешивать с `/VL` |
| `/VL <код локализации>` | `TYPED`, не смешивать с `/L` |

### 4.6. Отладка и тестирование

| Параметр | Целевой статус |
| --- | --- |
| `/debug [<режим>] [-attach]` | `PASS`; режим — один из `-tcp`, `-http`; `-attach` применяется к HTTP |
| `/debuggerURL <url>` | `PASS` с предупреждением о URL |
| `DisplayPerformance` | `PASS` |
| `/EmulateServerCallDelay [-Call n] [-Send n] [-Recevie n]` | `PASS`; сохранять написание `Recevie` до подтверждения версии |
| `/TestManager` | `PASS`, поддерживаемый thick/thin context |
| `/TestClient [-TPort n] [-TestClientID id] [-TURL=url]` | `PASS`, context-aware |
| `/UILogRecorder [-TPort n] [-File path]` | `PASS`, отдельное подтверждение записи файла |

Значения задержек должны проходить диапазонную проверку, а параметры отладки и
тестирования не должны включаться в обычный запуск без явного действия.

### 4.7. Проверки выполнения

| Параметр | Целевой статус |
| --- | --- |
| `/EnableCheckModal` | `PASS` |
| `/EnableCheckExtensionsAndAddInsSyncCalls` | `PASS`, с ограничениями thick-клиента |
| `/EnableCheckServerCalls` | `PASS` |
| `/EnableCheckScriptCircularRefs` | `PASS` |

### 4.8. Вспомогательные параметры

| Параметр | Целевой статус |
| --- | --- |
| `/C <строка>` | `PASS`; один аргумент с сохранением пробелов |
| `/ClearCache` | `PASS` только с allowlist-политикой |
| `/AllowExecuteScheduledJobs -Off` или `-Force` | `PASS`; `-Force` требует warning |
| `/UC <код>` | `PASS`, `SECRET` |
| `/RunShortcut <файл>` | `OUT/PASS`; безопасный выбор `v8i`/`v8l` |
| `/AppAutoInstallLastVersion` | `PASS/PRESERVE`; IBStart не устанавливает дистрибутив сам |
| `/Execute <внешняя обработка>` | `OUT`; trusted-file policy |
| `/URL <e1c/http(s) link>` | `TYPED`; resolver существующего или нового клиента |

`/Execute` имеет приоритет над `/URL`. Абсолютный `/URL` может сделать обычное
подключение из `/F`, `/S`, `/WS` или `/IBConnection` неактуальным. Это правило
относится к отдельному контексту полной реализации; в текущем обычном запуске
`/URL` и альтернативные connection-параметры из `AdditionalParameters`
отклоняются как конфликт с typed-подключением, а `/Execute` допускается только
в единственном экземпляре и с обязательной командой.

### 4.9. Прочие параметры клиента

| Параметр | Целевой статус |
| --- | --- |
| `/@ <файл с командой>` | `TYPED`; первая или единственная команда |
| `/Out <файл> [-NoTruncate]` | `TYPED`; policy перезаписи |
| `/DisableStartupMessages` | `PASS` |
| `/DisableStartupDialogs` | `PASS`; unattended semantics |
| `/DisableSplash` | `PASS` |
| `/DisableUnrecoverableErrorMessage` | `PASS` |
| `/DisableHomePageForms` | `PASS` |
| `/DisableBackgroundIndexBuild` | `PASS`, designer-related |
| `/UseHwLicenses` плюс/минус | `PASS` |
| `/DisplayUserNotificationList` | `PASS` |
| `/DisableExtensions [имена через запятую] [-AllExtensions]` | В документации тестовой версии 8.5.4: отключение расширений для текущего сеанса; нужны права администрирования расширений, учитывается область данных |
| `/ManageLicenses` | В документации тестовой версии 8.5.4: отдельный сценарий управления лицензиями; условия подключения и лицензии зависят от типа клиента |
| `/ManageServers` | В документации тестовой версии 8.5.4: отдельный сценарий управления серверами с теми же различиями клиентов |

Для `/ManageLicenses` и `/ManageServers` толстый клиент может использовать
временную пустую базу без лицензии; тонкому клиенту нужны база и лицензия.
Программа запуска выбирает толстый клиент. Эти сценарии не следует считать
обычным запуском выбранной базы только потому, что определение команды найдено.

### 4.10. Ориентиры изменений по версиям

Полная история определений находится в [журнале сверки](1c-reference-versions.md),
а точные диапазоны каталога и значений — в [матрице](launch-parameter-versions.md).

| Версия | Отличие от предыдущей проверенной версии |
| --- | --- |
| 8.3.12 | Найдены `/@`, `/AppArch`, `/TComp`, `/Execute`, новые написания мобильных команд; `/isdi`, `/MAWriteFile`, `/MAUpdatePublication` больше не найдены. |
| 8.3.16–8.3.17 | Найден `/MainWindowMode`; затем `/TechnicalSpecialistMode` вместо отсутствующего в новой документации `/DisplayAllFunctions`. |
| 8.3.21 | Найдены `/AccessToken`, `/HttpsForceTLS1_1`, `/HttpsForceTLS1_2`; больше не найден `/HttpsForceSSLv3`. |
| 8.3.22 | Для `/TComp` найдены варианты `-lz4` и `-zstd`. |
| 8.3.26–8.3.27 | Найден `/ResetSavedAuth`, затем `/EmailAuth` и `/DisableBackgroundIndexBuild`; `/DisableLocalSpeechToText` больше не найден в 8.3.27. |
| 8.5.1 | Найдены `/i85`, `/Theme`, `/ModuleGetSourceCodeAccess`, `/ModuleSetSourceCodeAccess`. |
| Тестовая 8.5.4 | Найдены три команды заголовка окна, `/DisableExtensions`, `/ManageLicenses`, `/ManageServers`; статус тестовой версии сохраняется. |

## 5. Batch-команды конфигуратора

Эти команды должны выполняться отдельным API для `DESIGNER`. Их нельзя без
проверки добавлять в обычное поле `AdditionalParameters`.

### 5.1. База и конфигурация

| Группа | Команды и опции |
| --- | --- |
| Выгрузка/загрузка ИБ | `/DumpIB <file>`; `/RestoreIB <file> [-JobsCount n]` |
| Восстановление структуры | `/IBRestoreIntegrity` |
| Конфигурация | `/DumpCfg <cf/cfe> [-Extension name]`; `/LoadCfg <cf/cfe> [-Extension name]`; `/MergeCfg <cf/cfe> -Settings <file> [-EnableSupport/-DisableSupport] [-IncludeObjectsByUnresolvedRefs/-ClearUnresolvedRefs] [-Extension name] [-force]` |
| Сравнение | `/CompareCfg -FirstConfigurationType <type> [-FirstName name] [-FirstFile file] [-FirstVersion version] -SecondConfigurationType <type> [-SecondName name] [-SecondFile file] [-SecondVersion version] [-MappingRule file] [-Objects file] -ReportType <type> [-IncludeChangedObjects] [-IncludeDeletedObjects] [-IncludeAddedObjects] -ReportFormat <format> -ReportFile <file>` |
| Обновление | `/UpdateDBCfg [-Dynamic mode] [-BackgroundStart] [-BackgroundCancel] [-BackgroundFinish [-Visible]] [-BackgroundSuspend] [-BackgroundResume] [-WarningsAsErrors] [-Server] [-v1/-v2] [-Extension name] [-SessionTerminate mode]` |
| База конфигурации | `/DumpDBCfg <cf/cfe> [-Extension name]`; `/DumpDBCfgList [-Extension name] [-AllExtensions]`; `/RollbackCfg [-Extension name]`; `/DeleteCfg [-Extension name] [-AllExtensions]` |
| Файлы конфигурации | `/DumpConfigFiles <dir> [-Module] [-Template] [-Help] [-AllWritable] [-Picture] [-Right] [-Extension name]`; `/LoadConfigFiles <dir> [-Module] [-Template] [-Help] [-AllWritable] [-Picture] [-Right] [-Extension name]` |
| Dump/load files | `/DumpConfigToFiles <dir> [-Format format] [-Extension name] [-AllExtensions] [-update] [-force] [-getChanges file] [-configDumpInfoForChanges file] [-listFile file] [-configDumpInfoOnly] [-Server [-JobsCount n]] [-Archive file] [-ignoreUnresolvedReferences]`; `/LoadConfigFromFiles <dir> [-Extension name] [-AllExtensions] -files file -listFile file -Format format [-updateConfigDumpInfo] [-NoCheck] [-Archive zip] -partial [-JobsCount n]` |
| Generation/source | `/GetConfigGenerationID [-Extension name]`; `/ModuleGetSourceCodeAccess <file>`; `/ModuleSetSourceCodeAccess <file>` |

### 5.2. Проверка и поддержка

| Группа | Команды и опции |
| --- | --- |
| Проверка модулей | `/CheckModules [-ThinClient] [-WebClient] [-MobileClient] [-MobileClientStandalone] [-MobileAppClient] [-Server] [-MobileAppServer] [-ExternalConnection] [-ThickClientOrdinaryApplication] [-ExtendedModulesCheck] [-Extension name] [-AllExtensions]` |
| Проверка конфигурации | `/CheckConfig [-ConfigLogIntegrity] [-IncorrectReferences] [-ThinClient] [-WebClient] [-MobileClient] [-MobileAppClient] [-Server] [-MobileAppServer] [-MobileClientStandalone] [-ExternalConnection] [-ExternalConnectionServer] [-ThickClientManagedApplication] [-ThickClientServerManagedApplication] [-ThickClientOrdinaryApplication] [-ThickClientServerOrdinaryApplication] [-DistributiveModules] [-UnreferenceProcedures] [-HandlersExistence] [-EmptyHandlers] [-ExtendedModulesCheck] [-CheckUseModality] [-CheckUseSynchronousCalls] [-UnsupportedFunctional] [-MobileClientDigiSign] [-Extension name] [-AllExtensions]` |
| Проверка расширений | `/CheckCanApplyConfigurationExtensions [-Extension name] [-AllZones] [-Z values]` |
| Восстановление базы | `/IBCheckAndRepair [-ReIndex] [-LogIntegrity [MDtype[,MDtype]]]` или `[-LogAndRefsIntegrity [MDtype[,MDtype]]]`, `[-RecalcTotals] [-IBCompression] [-Rebuild] [-RebuildStandaloneCfg] [-TestOnly]` или `[-BadRefCreate]`/`[-BadRefClear]`/`[-BadRefNone]` с `[-BadDataCreate]`/`[-BadDataDelete]`, `[-UseStartPoint] [-TimeLimit:hhh:mm] [-ConfigurationExtensionsLogIntegrity] [-RefreshTableLocation] [-BinaryDataStorageIntegrity [MDtype[,MDtype]]] [-JobsCount n] [-Z: separators] [-CheckAndEnableCORPFunctionality]` |
| Поддержка | `/UpdateCfg <cf/cfu> -Settings <file> [-IncludeObjectsByUnresolvedRefs/-ClearUnresolvedRefs] [-DumpListOfTwiceChangedProperties] [-force]`; `/ManageCfgSupport [-disableSupport [-force]]` |

### 5.3. Поставка, внешние обработки и данные

| Группа | Команды и опции |
| --- | --- |
| Поставка | `/CreateTemplateListFile [<file>] [-TemplatesSourcePath <path>]`; `/CreateDistributivePackage <dir> -File <description> [-PackageFileName <archive>] [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]`; `/CreateDistributionFiles [-cffile cf] [-cfufile cfu [-f cf или -v version]+] [-digisign file] [-WarningAsError]`; `/CreateDistributive <dir> -File <description> [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]`; `/SignCfg -ConfigurationType type -SignedFile cfe [-SignType type] [-File cfe] [-Name extension] [-Version version] -digisign file` |
| Внешние обработки | `/DumpExternalDataProcessorOrReportToFiles <root> <processor> [-Format Plain/Hierarchical]`; `/LoadExternalDataProcessorOrReportFromFiles <root> <processor>` |
| Мобильный пакет | `/MobileAppUpdatePublication`; `/MobileAppWriteFile <zip>`; `/MobileClientWriteFile <file>`; `/MobileClientDigiSign` |
| Журнал и данные | `/ReduceEventLogSize <Date> [-saveAs file] [-KeepSplitting]`; `/EraseData [/Z separators]`; `/SetPredefinedDataUpdate [-Auto] [-UpdateAutomatically] [-DoNotUpdateAutomatically]` |
| Распределённая ИБ | `/ResetMasterNode` |

## 6. Хранилище конфигурации, agent и OLE

### 6.1. Хранилище конфигурации

| Группа | Реестр |
| --- | --- |
| Доступ | `/ConfigurationRepositoryF <dir>`; `/ConfigurationRepositoryN <name>`; `/ConfigurationRepositoryP <password>` |
| Создание | `/ConfigurationRepositoryCreate [-AllowConfigurationChanges -ChangesAllowedRule <rule> -ChangesNotRecommendedRule <rule>] [-NoBind] [-MinPasswordLenght n] [-CheckPasswordComplexity] [-Extension name]` |
| Пользователи | `/ConfigurationRepositoryAddUser -User <name> -Pwd <password> -Rights <rights> [-RestoreDeletedUser] [-Extension name]`; `/ConfigurationRepositoryCopyUsers -Path <path> -User <name> -Pwd <password> [-RestoreDeletedUser] [-Extension name]` |
| Объекты | `/ConfigurationRepositoryLock [-Objects file] [-revised] [-Extension name]`; `/ConfigurationRepositoryUnLock [-Objects file] [-force] [-Extension name]`; `/ConfigurationRepositoryCommit [-Objects file] [-comment text] [-keepLocked] [-force] [-Extension name]` |
| Конфигурация | `/ConfigurationRepositoryBindCfg [-forceBindAlreadyBindedUser] [-forceReplaceCfg] [-Extension name]`; `/ConfigurationRepositoryUnbindCfg [-force] [-Extension name]`; `/ConfigurationRepositoryDumpCfg <cf> [-v version] [-Extension name]`; `/ConfigurationRepositoryUpdateCfg [-v version] [-revised] [-force] [-Objects file] [-Extension name]` |
| Отчёт/служебные | `/ConfigurationRepositorySetLabel [-v version] [-name] <label> [-comment text] [-Extension name]`; `/ConfigurationRepositoryReport <file> [-NBegin version] [-NEnd version] [-DateBegin date] [-DateEnd date] [-GroupByObject] [-GroupByComment] [-DoNotIncludeVersionsWithLabels] [-IncludeOnlyVersionsWithLabels] [-IncludeCommentLinesWithDoubleSlash] [-ConfigurationVersion version] [-ReportFormat txt или mxl] [-Extension name]`; `/ConfigurationRepositoryOptimizeData [-Extension name]` |
| Кэш | `/ConfigurationRepositoryClearCache [-Extension name]`; `/ConfigurationRepositoryClearLocalCache [-Extension name]`; `/ConfigurationRepositoryClearGlobalCache [-Extension name]` |

Операции с паролями, удалением и очисткой должны требовать отдельного
подтверждения и не могут быть скрытым продолжением обычного запуска базы.

### 6.2. Agent, OLE и прочие batch

| Группа | Команды |
| --- | --- |
| Agent | `/AgentMode`; `/AgentPort <port>`; `/AgentListenAddress <address>`; `/AgentSSHHostKey <private-key>`; `/AgentSSHHostKeyAuto`; `/AgentBaseDir <dir>` |
| Прочие batch | `/Visible`; `/RunEnterprise`; `/ConvertFiles <file-or-path>`; `/DumpResult <file>` |
| OLE | `/RegServer [-AllUsers/-CurrentUser/-Auto]`; `/UnregServer` |

`/RegServer` и `/UnregServer` меняют системную регистрацию и требуют отдельного
подтверждённого режима с проверкой прав.

## 7. `CREATEINFOBASE` и строки соединения

Параметры создания в каталоге конструктора справочные (`OUT/PRESERVE`):

| Параметр | Назначение и профиль |
| --- | --- |
| `/AddToList [<имя базы>]` | Добавляет созданную базу в список; без имени формирует представление по умолчанию. При отсутствии команды база не добавляется. Определение есть с 8.3.11 в проверенной документации. |
| `/AddInList [<имя базы>]` | Историческое имя той же команды в проверенных версиях 8.3.6–8.3.10. |
| `/UseTemplate [<имя файла шаблона>]` | Создание по cf или dt; без файла опция игнорируется. |

Конструктор показывает их назначение и версии; отдельная операция создания
базы в IBStart не реализована. `/L`, `/VL` и `/DumpResult` также описаны в
контексте создания в справке команды `CREATEINFOBASE`.

Общие ключи: `disstt` (Y/N), `LicDstr` (Y/N), `prmod` (1), `Pwd` (SECRET),
`Usr`, `Z`.

Файловый вариант: `DBFormat` (8.2.14/8.3.8), `DBPageSize` (4096/4k,
8192/8k, 16384/16k, 32768/32k, 65536/64k), `File`, `Locale`.

Клиент-серверный вариант: `CrSQLDB` (Y/N), `DB`, `DBMS` (MSSQLServer,
PostgreSQL, IBMDB2, OracleDatabase), `DBPwd` (SECRET), `DBSrvr`, `DBUID`,
`Locale`, `Ref`, `SchJobDn` (Y/N), `SPwd` (SECRET), `SQLYOffs` (0/2000),
`Srvr`, `SUsr`.

`Srvr` должен тестироваться с protocol/port, IPv4, IPv6 и списком кластеров
через запятую без пробелов. Строка должна иметь round-trip без потери порядка,
кавычек и неизвестных ключей.

## 8. Web-клиент

Параметры web-клиента являются отдельной моделью query/navigation options. Их
нельзя механически смешивать с Windows command line. Нужно различать URL
encoding и quoting аргументов.

| Категория | Имена |
| --- | --- |
| Auth | `AccessToken`, `Authoff`, `N=value`, `P=value`, `OIDA` плюс/минус, `OidcSelectedProvider`, `UC=value`, `WA` плюс/минус, `UsePrivilegedMode` |
| App/navigation | `C=value`, `URL=value`, `Z=values` |
| Debug | `Debug=tcp/http` с optional attach, `DebuggerURL=value`, `DisplayPerformance` |
| UI | `DisableHomePageForms`, `DisableStartupMessages`, `DisableUnrecoverableErrorMessage`, `i85`, `iTaxi`, `itdi`, `L=value`, `MainWindowMode=mode`, `Theme=Dark/Light`, `VL=value` |
| Notifications | `ProgressiveWebApplicationName=value`, `SYSTEMWEBCLIENTSTAT`, `TechnicalSpecialistMode` |
| Testing | `TestClient`, `TestClientID=value` |

`MainWindowMode`: `Normal`, `Workplace`, `EmbeddedWorkplace`,
`FullscreenWorkplace`, `Kiosk`. Символы `&`, `#`, пробелы и `=` должны корректно
кодироваться. JWT, password и `UC` маскируются в логах и автоматически
создаваемых диагностических сообщениях; в пользовательском просмотре и при
явном копировании они остаются доступны.

Разрешённый список стандартных web-функций:

`ActiveUsers`, `AdditionalAuthenticationSettings`, `AnalyticsSystemManagement`,
`AuthenticationLocks`, `ConfigurationLicense`, `DataBaseCopiesManagement`,
`DataChangeHistory`, `DeleteMarkedObjects`, `DocumentsPosting`,
`CollaborationSystemManagement`, `ErrorProcessingSettings`, `EventLog`,
`EventLogSettings`, `ConfigurationExtensionsManagement`,
`ExternalDataSourcesManagement`, `FindByReference`, `FullTextSearchManagement`,
`InfobaseParameters`, `IntegrationServicesManagment`, `LicenseAcquisition`,
`MobileAppBuildService`, `MobileAppBuilderServiceLoader`,
`InfobaseRegionalSettings`, `TotalsManagement`, `UserList`, `ServersManagement`.

До подтверждения новой версии нужно сохранять точное написание
`IntegrationServicesManagment`.

## 9. Mobile-клиент

В проверенном snapshot перечислены совместимые группы:

- `/N`, `/P`, `/WSN`, `/WSP`, `/WSA`, `/OIDA`, `/Authoff`,
  `/UsePrivilegedMode`, `/Z`, `/O`;
- `/HttpsForceTLS1_0`;
- `/L`, `/VL`;
- `/ClearCache`, `/C`, `/URL`;
- `/DisableStartupMessages`;
- `/TestClient`, `/UILogRecorder`.

Desktop IBStart должен сохранять связанные поля `ibases.v8i` и показывать их как
external/mobile-only, но не обещать выполнение мобильных функций в Windows
desktop-контексте.

## 10. Связь с полями `ibases.v8i`

| Поле файла | Параметр запуска | Текущий статус |
| --- | --- | --- |
| `File` внутри `Connect` | `/F` | Базово применяется |
| `Srvr` + `Ref` внутри `Connect` | `/S` | Базово применяется |
| `WS` внутри `Connect` | `/WS` | Базово применяется |
| `ClientConnectionSpeed` | `/O` | Сохраняется, но пока не применяется |
| `WA` | `/WA` | Сохраняется, но пока не применяется |
| `WSA` | `/WSA` | Typed-поддержки нет |
| `UseProxy`, `PSrv`, `PPort`, `PUser`, `PPasswd` | `/Proxy` и nested options | Typed-поддержки нет |
| `App` и `DefaultApp` | client selection и `/AppAutoCheckMode` | Частично |
| `Version` и `DefaultVersion` | platform selection и `/AppAutoCheckVersion` | Частично |
| `AppArch` | `/AppArch` | Применяется |
| `Https*` | `/HttpsCert`, `/HttpsCA`, TLS flags | Typed-поддержки нет |
| `AdditionalParameters` | raw argv | Частично |

## 11. Правила полной реализации

Для каждого параметра registry должна хранить:

- canonical name и case-insensitive lookup;
- attached, separate или nested форму;
- тип значения и допустимые значения;
- контекст запуска;
- минимальную и проверенную версию;
- связь с полем `ibases.v8i`;
- приоритет и конфликтные параметры;
- redaction policy;
- способ проверки и exit-code policy.

Необходимо блокировать или явно подтверждать конфликты `/F` + `/S`, `/NoProxy` +
`/Proxy`, несколько TLS-флагов, `/@` не в начале, `/Execute` + `/URL`, дубликаты
`/AppArch` и несовместимые режимы процесса.

Секреты `/P`, `/WSP`, `-pwd`, `PPasswd`, `AccessToken`, `UC`, `SPwd`, `DBPwd` и
аналогичные значения разрешено хранить в исходном `ibases.v8i`, а именованные
логины и пароли — в `settings.json` в массиве `credentials`. Последние можно
выбрать в окне «Запуск с параметрами…» только для подходящей базы: область
записи бывает общей для всех списков либо ограниченной выбранными базами и
группами. Эти значения можно показывать в интерфейсе и копировать явным
действием пользователя. Их нельзя записывать в открытом виде в log и
автоматически создаваемые diagnostics/report; fixture должен использовать
синтетическое или замаскированное значение. Перед запуском остаётся
предупреждение с решением пользователя о продолжении.

Запись `credentials` не изменяет `ibases.v8i` и не добавляет в него
пользователя или пароль. Подстановка в разовом запуске также не изменяет
справочник: ручная правка полей действует только для текущей команды. Формат
`settings.json`, поля области и правила разрешения групп описаны в
[configuration.md](configuration.md), а пользовательский сценарий — в
[user-guide.md](user-guide.md).

## 12. Тесты

Результаты реализованного конструктора и границы проверок зафиксированы в
[отчёте проверки](verification-launch-parameter-builder.md). Следующий перечень
описывает требования к полной реализации реестра, а не уже закрытые сценарии.

- каждый параметр разделов 3–4 с отдельной проверкой grammar;
- nested options и параметры со значимым порядком;
- секреты и redacted preview;
- конфликты режимов и подключений;
- точные, префиксные и отсутствующие версии;
- все значения `AppArch` и invalid value;
- file/server/web/legacy URL, IPv4, IPv6 и cluster list;
- batch-команды только через явно открытый UI/API;
- web URL encoding и mobile subset;
- manual matrix для 8.3.20, 8.3.26 и 8.5.1.
