# Параметры и команды запуска 1С

Статус: основной документ по командной строке, batch, web и mobile.

Дата сверки с кодом: 2026-08-31.

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

Проверенные локальные снимки относятся к ветке документации 8.5.1. Отдельная
контрольная ссылка на ветку 8.3.26 не заменяет проверку фактической версии
платформы: параметр из нового справочника может отсутствовать в старом клиенте.

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
  [`MainWindow::LaunchSelected`](../src/ui/main_window.cpp);
- построение executable и аргументов —
  [`launcher::BuildCommand`](../src/core/launcher/command_builder.cpp);
- распознавание raw Windows-аргументов —
  [`launcher::SplitCommandArguments`](../src/core/launcher/command_builder.cpp).

Полный version profile и отдельные API для batch/web/mobile/OLE/agent пока не
реализованы; неизвестные параметры по-прежнему передаются как raw-текст после
проверки конфликтов.

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
| `/MainWindowMode` | `Normal`, `Workplace`, `EmbeddedWorkplace`, `FullscreenWorkplace`, `Kiosk`. | `TYPED/PASS` |

### 4.4. Сертификаты и TLS

| Параметр | Значения | Целевой статус |
| --- | --- | --- |
| `/HttpsCert` | `-windows`, `-linux`, `-macos`, `-recent`, `-auto`, `-choose`, `-file`, `-pwd`, `-none`. | `TYPED`, пароль — `SECRET` |
| `/HttpsCA` | `-windows`, `-linux`, `-macos`, `-file`, `-pwd`, `-none`. | `TYPED` |
| `/HttpsForceTLS1_0` | Разрешить TLS 1.0. | `PASS` с security warning |
| `/HttpsForceTLS1_1` | Разрешить TLS 1.1 и новее. | `PASS` с security warning |
| `/HttpsForceTLS1_2` | Требовать TLS 1.2 и новее. | `PASS` |

Нужно запрещать несколько одновременных `/HttpsForceTLS*`, проверять пути до
запуска и не копировать содержимое сертификатов в настройки или репозиторий.

### 4.5. Интерфейс и локализация

| Параметр | Целевой статус |
| --- | --- |
| `/i85` | `PASS` только для совместимого клиента 8.5 |
| `/Theme Dark` или `/Theme Light` | `PASS` после проверки интерфейса версии |
| `/iTaxi` | `PASS` |
| `/itdi` | `PASS/PRESERVE`, требует version check |
| `/TechnicalSpecialistMode` | `PASS` |
| `/L <код языка>` | `TYPED`, не смешивать с `/VL` |
| `/VL <код локализации>` | `TYPED`, не смешивать с `/L` |

### 4.6. Отладка и тестирование

| Параметр | Целевой статус |
| --- | --- |
| `/debug [tcp/http] [-attach]` | `PASS`, `-attach` только для допустимого HTTP-сценария |
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
| Поставка | `/CreateTemplateListFile <file> [-TemplatesSourcePath]`; `/CreateDistributivePackage <dir> -File <description> -PackageFileName <archive> [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]`; `/CreateDistributionFiles [-cffile cf] [-cfufile cfu [-f cf или -v version]+] [-digisign file] [-WarningAsError]`; `/CreateDistributive <dir> -File <description> [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]`; `/SignCfg -ConfigurationType type -SignedFile cfe [-SignType type] [-File cfe] [-Name extension] [-Version version] -digisign file` |
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
аналогичные значения разрешено хранить в исходном `ibases.v8i`, показывать в
интерфейсе и копировать явным действием пользователя. Их нельзя записывать в
открытом виде в log и автоматически создаваемые diagnostics/report; fixture
должен использовать синтетическое или замаскированное значение. Перед запуском
остаётся предупреждение с решением пользователя о продолжении.

## 12. Тесты

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
