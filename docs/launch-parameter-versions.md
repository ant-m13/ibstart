# Матрица версий параметров запуска

Таблицы сформированы из каталога совместимости IBStart. Дата сверки официальной документации и различия между версиями приведены в [общем журнале](1c-reference-versions.md).

SHA-256 данных совместимости: `45733b23e0c9be238b52bfe481e43758cae1e3956c5b8359683240ff84741216`.

Основные правила и статус исполнения описаны в [параметрах запуска](launch-parameters.md).

## Значение статусов

Наличие команды в таблице означает, что её определение найдено в разделе документации ИТС соответствующей версии. Это не проверка исполнения на установленной платформе. Отсутствие определения означает отсутствие в проверенном справочнике, а не доказательство невозможности исполнения. Промежуточные и неизвестные версии не наследуют профиль соседней версии.

Конструктор редактирует обычные параметры и вложенные опции команд с извлечённой грамматикой выбранного профиля. Web/mobile и отдельные режимы остаются справочными. Наличие определения в матрице не подтверждает выполнение операции платформой.

## Проверенные профили

| Версия | Официальный раздел |
| --- | --- |
| 8.3.6 | [ИТС](https://its.1c.ru/db/v836doc/bookmark/adm/TI000000493) |
| 8.3.7 | [ИТС](https://its.1c.ru/db/v837doc/bookmark/adm/TI000000493) |
| 8.3.8 | [ИТС](https://its.1c.ru/db/v838doc/bookmark/adm/TI000000493) |
| 8.3.9 | [ИТС](https://its.1c.ru/db/v839doc/bookmark/adm/TI000000493) |
| 8.3.10 | [ИТС](https://its.1c.ru/db/v8310doc/bookmark/adm/TI000000493) |
| 8.3.11 | [ИТС](https://its.1c.ru/db/v8311doc/bookmark/adm/TI000000493) |
| 8.3.12 | [ИТС](https://its.1c.ru/db/v8312doc/bookmark/adm/TI000000493) |
| 8.3.13 | [ИТС](https://its.1c.ru/db/v8313doc/bookmark/adm/TI000000493) |
| 8.3.14 | [ИТС](https://its.1c.ru/db/v8314doc/bookmark/adm/TI000000493) |
| 8.3.15 | [ИТС](https://its.1c.ru/db/v8315doc/bookmark/adm/TI000000493) |
| 8.3.16 | [ИТС](https://its.1c.ru/db/v8316doc/bookmark/adm/TI000000493) |
| 8.3.17 | [ИТС](https://its.1c.ru/db/v8317doc/bookmark/adm/TI000000493) |
| 8.3.18 | [ИТС](https://its.1c.ru/db/v8318doc/bookmark/adm/TI000000493) |
| 8.3.19 | [ИТС](https://its.1c.ru/db/v8319doc/bookmark/adm/TI000000493) |
| 8.3.20 | [ИТС](https://its.1c.ru/db/v8320doc/bookmark/adm/TI000000493) |
| 8.3.21 | [ИТС](https://its.1c.ru/db/v8321doc/bookmark/adm/TI000000493) |
| 8.3.22 | [ИТС](https://its.1c.ru/db/v8322doc/bookmark/adm/TI000000493) |
| 8.3.23 | [ИТС](https://its.1c.ru/db/v8323doc/bookmark/adm/TI000000493) |
| 8.3.24 | [ИТС](https://its.1c.ru/db/v8324doc/bookmark/adm/TI000000493) |
| 8.3.25 | [ИТС](https://its.1c.ru/db/v8325doc/bookmark/adm/TI000000493) |
| 8.3.26 | [ИТС](https://its.1c.ru/db/v8326doc/bookmark/adm/TI000000493) |
| 8.3.27 | [ИТС](https://its.1c.ru/db/v8327doc/bookmark/adm/TI000000493) |
| 8.5.1 | [ИТС](https://its.1c.ru/db/v851doc/bookmark/adm/TI000000493) |
| 8.5.4* | [ИТС](https://its.1c.ru/db/v854doc/bookmark/adm/TI000000493) |

\* — тестовая версия по заголовку документации.

Профили 8.5.2 и 8.5.3 не предусмотрены. Полный номер сборки сопоставляется с точным семейством (например, 8.3.26.1540 → 8.3.26). Пустая версия, префикс 8.3 и неизвестное семейство имеют статус «не сверено».

## Наличие команд в документации

| Команда | Проверенные версии с определением |
| --- | --- |
| `/@` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/AccessToken` | 8.3.21–8.3.27, 8.5.1, 8.5.4 |
| `/AddInList` | 8.3.6–8.3.10 |
| `/AddToList` | 8.3.11–8.3.27, 8.5.1, 8.5.4 |
| `/AgentBaseDir` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AgentListenAddress` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AgentMode` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AgentPort` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AgentSSHHostKey` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AgentSSHHostKeyAuto` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/AllowExecuteScheduledJobs` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/AppArch` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/AppAutoCheckMode` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/AppAutoCheckVersion` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/AppAutoInstallLastVersion` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/AppCaption` | 8.5.4 |
| `/AppCaptionFromInfobaseList` | 8.5.4 |
| `/AppShortCaption` | 8.5.4 |
| `/Authoff` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/C` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/CheckCanApplyConfigurationExtensions` | 8.3.9–8.3.27, 8.5.1, 8.5.4 |
| `/CheckConfig` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/CheckModules` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ClearCache` | 8.3.10–8.3.27, 8.5.1, 8.5.4 |
| `/CompareCfg` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryAddUser` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryBindCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryClearCache` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryClearGlobalCache` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryClearLocalCache` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryCommit` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryCopyUsers` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryCreate` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryDumpCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryF` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryLock` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryN` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryOptimizeData` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryP` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryReport` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositorySetLabel` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryUnbindCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryUnLock` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/ConfigurationRepositoryUpdateCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ConvertFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/CreateDistributionFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/CreateDistributive` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/CreateDistributivePackage` | 8.3.13–8.3.27, 8.5.1, 8.5.4 |
| `/CreateTemplateListFile` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/debug` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/debuggerURL` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/DeleteCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DisableBackgroundIndexBuild` | 8.3.27, 8.5.1, 8.5.4 |
| `/DisableExtensions` | 8.5.4 |
| `/DisableHomePageForms` | 8.3.20–8.3.27, 8.5.1, 8.5.4 |
| `/DisableLocalSpeechToText` | 8.3.23–8.3.26 |
| `/DisableSplash` | 8.3.13–8.3.27, 8.5.1, 8.5.4 |
| `/DisableStartupDialogs` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DisableStartupMessages` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DisableUnrecoverableErrorMessage` | 8.3.17–8.3.27, 8.5.1, 8.5.4 |
| `/DisplayAllFunctions` | 8.3.12–8.3.16 |
| `/DisplayUserNotificationList` | 8.3.13–8.3.27, 8.5.1, 8.5.4 |
| `/DumpCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpConfigFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpConfigToFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpDBCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpDBCfgList` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpExternalDataProcessorOrReportToFiles` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/DumpIB` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/DumpResult` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/EmailAuth` | 8.3.27, 8.5.1, 8.5.4 |
| `/EmulateServerCallDelay` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/EnableCheckExtensionsAndAddInsSyncCalls` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/EnableCheckModal` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/EnableCheckScriptCircularRefs` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/EnableCheckServerCalls` | 8.3.9–8.3.27, 8.5.1, 8.5.4 |
| `/EraseData` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Execute` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/F` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/GetConfigGenerationID` | 8.3.20–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsCA` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsCert` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsForceSSLv3` | 8.3.6–8.3.20 |
| `/HttpsForceTLS1_0` | 8.3.9–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsForceTLS1_1` | 8.3.21–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsForceTLS1_2` | 8.3.21–8.3.27, 8.5.1, 8.5.4 |
| `/HttpsNSS` | 8.3.6–8.3.7 |
| `/i85` | 8.5.1, 8.5.4 |
| `/IBCheckAndRepair` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/IBConnectionString` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/IBName` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/IBRestoreIntegrity` | 8.3.9–8.3.27, 8.5.1, 8.5.4 |
| `/isdi` | 8.3.6–8.3.11 |
| `/iTaxi` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/itdi` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/L` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/LoadCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/LoadConfigFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/LoadConfigFromFiles` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/LoadExternalDataProcessorOrReportFromFiles` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/LogUI` | 8.3.6–8.3.22 |
| `/MainWindowMode` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/ManageCfgSupport` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/ManageLicenses` | 8.5.4 |
| `/ManageServers` | 8.5.4 |
| `/MAUpdatePublication` | 8.3.6–8.3.11 |
| `/MAWriteFile` | 8.3.6–8.3.11 |
| `/MergeCfg` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/MobileAppUpdatePublication` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/MobileAppWriteFile` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/MobileClientDigiSign` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/MobileClientWriteFile` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/ModifyPassword` | 8.3.17–8.3.27, 8.5.1, 8.5.4 |
| `/ModuleGetSourceCodeAccess` | 8.5.1, 8.5.4 |
| `/ModuleSetSourceCodeAccess` | 8.5.1, 8.5.4 |
| `/N` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/NoProxy` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/O` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/OIDA` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Out` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/P` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Proxy` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ReduceEventLogSize` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RegServer` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ResetMasterNode` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/ResetSavedAuth` | 8.3.26–8.3.27, 8.5.1, 8.5.4 |
| `/RestoreIB` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RollbackCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RunEnterprise` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RunModeManagedApplication` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RunModeOrdinaryApplication` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/RunShortcut` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/S` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/SAOnRestart` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/SetPredefinedDataUpdate` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/SignCfg` | 8.3.15–8.3.27, 8.5.1, 8.5.4 |
| `/SLev` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/TComp` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/TechnicalSpecialistMode` | 8.3.17–8.3.27, 8.5.1, 8.5.4 |
| `/TestClient` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/TestManager` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Theme` | 8.5.1, 8.5.4 |
| `/UC` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/UILogRecorder` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/UnregServer` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/UpdateCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/UpdateDBCfg` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/URL` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/UseHwLicenses` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/UseTemplate` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Visible` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/VL` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/WA` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/WS` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/WSA` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/WSN` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/WSP` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/Z` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `DisplayPerformance` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `UsePrivilegedMode` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |

## Варианты значений структурированных команд

Ниже приведены варианты основных команд; вложенные опции, их значения и требования хранятся отдельно в `parameter_grammar.inc`. Написание сохраняется, включая минус перед `-Dark`, `-Light` и вариантами `/MainWindowMode`.

| Команда | Значение | Проверенные версии с вариантом |
| --- | --- | --- |
| `/setpredefineddataupdate` | `-Auto` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/setpredefineddataupdate` | `-UpdateAutomatically` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/setpredefineddataupdate` | `-DoNotUpdateAutomatically` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/o` | `Normal` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/o` | `Low` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/tcomp` | `-None` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/tcomp` | `-Deflate` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/tcomp` | `-SDC` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/tcomp` | `-lz4` | 8.3.22–8.3.27, 8.5.1, 8.5.4 |
| `/tcomp` | `-zstd` | 8.3.22–8.3.27, 8.5.1, 8.5.4 |
| `/slev` | `0` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/slev` | `1` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/slev` | `2` | 8.3.7–8.3.27, 8.5.1, 8.5.4 |
| `/apparch` | `x86` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/apparch` | `x86_prt` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/apparch` | `x86_64` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/apparch` | `x86_64_prt` | 8.3.12–8.3.27, 8.5.1, 8.5.4 |
| `/mainwindowmode` | `-Normal` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/mainwindowmode` | `-Workplace` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/mainwindowmode` | `-EmbeddedWorkplace` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/mainwindowmode` | `-FullscreenWorkplace` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/mainwindowmode` | `-Kiosk` | 8.3.16–8.3.27, 8.5.1, 8.5.4 |
| `/theme` | `-Dark` | 8.5.1, 8.5.4 |
| `/theme` | `-Light` | 8.5.1, 8.5.4 |
| `/wa` | `+` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/wa` | `-` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/wsa` | `+` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/wsa` | `-` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/oida` | `+` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/oida` | `-` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/usehwlicenses` | `+` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/usehwlicenses` | `-` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/appautocheckversion` | `+` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/appautocheckversion` | `-` | 8.3.8–8.3.27, 8.5.1, 8.5.4 |
| `/appautoinstalllastversion` | `+` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/appautoinstalllastversion` | `-` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/disablelocalspeechtotext` | `+` | 8.3.24–8.3.26 |
| `/disablelocalspeechtotext` | `-` | 8.3.24–8.3.26 |
| `/allowexecutescheduledjobs` | `-Off` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |
| `/allowexecutescheduledjobs` | `-Force` | 8.3.6–8.3.27, 8.5.1, 8.5.4 |

## Обновление

Общий процесс для обоих приложений, контроль новых страниц и границы автоматической сверки описаны в [обновлении справочников](maintaining-1c-reference.md). Обновление матриц документации само по себе не расширяет поддержку приложения.

1. Проверить официальный раздел ИТС для нужной версии по ссылке в реестре.
2. Обновить идентификаторы и варианты значений поддержанных команд; проверить соответствие выбранной версии.
3. Просмотреть изменения `parameter_versions.inc`, особенно удаления, пробелы в старых справочниках и изменения вложенных опций. Автоматическое извлечение требует инженерной проверки.
4. Добавить собственные описания и редакторы новых команд; при изменении синтаксиса обновить основной справочник и граничные тесты.
5. Выполнить сборку, CTest и `python tools/update-launch-parameter-versions.py --check-docs`. Повторная генерация должна давать тот же результат.

Таблицу можно восстановить без исходных страниц командой `python tools/update-launch-parameter-versions.py --docs-only`. Сборка приложения использует собственные данные каталога.
