# Ревизия смысла команд и параметров запуска

Дата: 2026-10-20. Ветка: `feat/launch-parameters-builder`.

## Источники и границы

Сопоставлены встроенные описания и приложение 7 документации для всех
24 профилей: 8.3.6–8.3.27, стабильная 8.5.1 и тестовая 8.5.4.
Профили 8.5.2/8.5.3 отсутствуют. Ссылки на официальную документацию находятся
в [реестре источников](reference-data/1c-reference.json); официальная точка
входа стабильной версии — [ИТС 8.5.1](https://its.1c.ru/db/v851doc/bookmark/adm/TI000000493).

Рассмотрены все 166 пунктов каталога, 297 различных пар «команда + вложенная
опция», перечисления и дополнительные разделы строки соединения, веб-клиента
и мобильной версии. Проверялись назначение, обязательность, альтернативы,
формат значений, единицы измерения, значения по умолчанию, приоритеты,
совместимость, условия игнорирования и различия проверенных версий.

Для смысловой сверки использованы основное определение и отличающиеся
абзацы остальных версий. Синтаксис сверялся с правилами обозначений в начале
приложения 7 и пояснениями после заголовка: одних квадратных скобок недостаточно,
если последующий текст явно уточняет требование или исключение.

Тексты остаются собственными пояснениями проекта. Страницы, HTML и исходные
примеры ИТС не переносятся в репозиторий или EXE. Документация платформы,
ограничения полей конструктора и проверка исполнения в установленной 1С
рассматриваются отдельно.

## Исправленные расхождения

| Область | Результат |
| --- | --- |
| `/HttpsCert -recent/-auto/-choose` | В старых профилях явно указана Windows; с появлением источника macOS указаны Windows/macOS. Эти способы выбора больше не выглядят применимыми к каталогу сертификатов Linux. Сохранены приоритет `-auto`, запоминание выбора и условия показа диалога. |
| `/CompareCfg -FirstName/-SecondName` | Указана связь имени конфигурации с типом соответствующего источника. Для поставщика это его конфигурация, для расширения — расширение, включая вариант, сохранённый в базе. Уточнение согласовано с пользователем. |
| Старые `-FirstConfigurationKey/-SecondConfigurationKey` | Уточнено назначение имени для расширения; остальные варианты идентификатора — файл, версия хранилища либо отсутствие значения — сохранены по типу источника. |
| `/ConfigurationRepositoryAddUser -Pwd` | Проверка по политикам паролей упоминается только в профилях, где об этом говорит источник. Раннее описание не получает детали новых версий. |
| `/ConfigurationRepositoryAddUser -Rights` | По окончательному решению пользователя конструктор предлагает выбор одного из четырёх документированных значений. Это способ структурированного ввода, а не утверждение о невозможности сочетания прав в платформе. |
| Мобильная версия | Добавлены пропущенные `/O`, `/VL`, `/HttpsForceTLS1_0`, `/URL`, `/DisableStartupMessages` и `/UILogRecorder`. Причина пропуска — двоеточие после последнего элемента строки перечня. Маски рассчитаны по наличию каждого имени в документации. |
| Справка веб-клиента | Убрано повторение краткого пункта `Debug`; подробное описание транспорта, значения по умолчанию и `attach` сохранено. Общий `DebuggerURL` оставлен для раннего профиля, где нет позднего уточнения протоколов. |
| `/DisableExtensions` | Добавлено поле списка имён через запятую. Обозначение `ExtName1` в заголовке не заключено в угловые скобки, из-за чего раньше поле терялось. `-AllExtensions` остаётся отдельным флагом. |
| `/LoadConfigFiles` в ранних справочниках | Поле называется «каталог загрузки» по назначению команды. Ошибочное имя «каталог выгрузки» остаётся видимым в исходном версионном синтаксисе и поясняется примечанием. |
| `/ManageCfgSupport` | Отсутствие `-disableSupport` диагностируется во всех профилях, где пояснение явно требует эту операцию. Учтены старое слово «ключ» и современное «параметр». |
| `/DeleteCfg` | Требуется выбор расширения либо всех расширений. Один только режим реструктуризации не выбирает удаляемые объекты. |
| Формирование поставок | `-MakeSetup` и `-MakeFiles` проверяются как несовместимые, хотя в заголовках они записаны независимыми необязательными блоками. Неявный режим `-MakeSetup` сохранён. |
| Объединение и расширения | Добавлены явно описанные конфликты `-Extension` с `-EnableSupport/-DisableSupport`, `-AllZones` с `-Extension/-Z`, `-AllExtensions` с `-files/-listFile`. |
| Тестирование и исправление базы | Начиная с профилей, где запрет явно описан, `/IBCheckAndRepair` не сочетается с глобальной `/Z`. Локальная `-Z:` остаётся допустимой. |
| Кавычки | При сборке через поля соблюдаются указания о кавычках для списка `-files`, комментариев, меток и значений разделителей. Это не меняет сохранённую вручную строку без действия пользователя. |
| Слитные перечислимые значения | Редактор распознаёт `/SLev0` и `/ONormal` наряду с разделённой формой. Совпадение проверяется по полному допустимому значению, чтобы `/OIDA` и неизвестные команды не принимались за `/O`. |

## Решения для неоднозначных источников

- У `/CompareCfg` текст про имя поставщика для `ExtensionConfiguration`
  трактуется через выбранный тип источника: пользователь подтвердил, что
  имя может относиться к расширению.
- Наличие нескольких прав в настройках пользователя хранилища не определяет
  разделитель набора в CLI. Пользователь отдельно выбрал редактор одного
  значения `-Rights`; непроверенный синтаксис сочетаний не генерируется.
- У `/LoadConfigFromFiles` в 8.5.4 вводный текст упоминает `-Format`, но
  заголовок и перечень его не содержат. Сохранено явное примечание о расхождении;
  опция не наследуется из 8.5.1.
- У `/DumpConfigToFiles -configDumpInfoOnly` общий запрет сочетаний дополняется
  отдельно описанным исключением `-Archive`. Оба условия остаются в справке;
  из противоречивой формулировки не выводится новый общий запрет архивирования.
- У `/CheckCanApplyConfigurationExtensions` в одном старом абзаце роли `/Z`
  и `-Z` переставлены. Сохранено пояснение о расхождении с общим разделом.
- Для `/ConfigurationRepositoryUpdateCfg -v` раннее условие 8.3.6 отличается
  от следующих справочников. Пояснение выбирается по версии, а расхождение
  не выдаётся за проверенное поведение установленной платформы.
- Неуказанный транспорт desktop `/debug` не получает значение по умолчанию
  из раздела веб-клиента. В веб-разделе TCP указан явно, в desktop-определении
  соответствующего утверждения нет.

Опции, которые платформа игнорирует вне своего контекста, не превращаются
автоматически в ошибки. Внешние условия — состояние базы, поддержка,
захват объектов, права, лицензия, содержимое файлов и настройки окружения —
показываются в справке; их нельзя проверить по одной строке конструктора.

## Проверки

- Release MSVC с `/W4 /WX`; native unit и контракты настоящих Win32-форм.
- Во всех профилях проверены исправленные зависимости и выбор прав;
  отдельно проверены версия сертификатов, мобильные пункты, кавычки,
  слитные значения и поле имён отключаемых расширений.
- Python: 44 синтетические проверки, включая разбор старого «ключ» вместо
  «параметр», ограничения из пояснений и последний элемент перед двоеточием.
- `tools/check-launch-help-coverage.py` сверяет наличие пунктов каталога
  и пояснений дополнительных разделов непосредственно с документацией.
  Проверены 335 пар «мобильная команда × версия», 870 пар для веб-раздела
  и 507 пар для строки соединения. Это количества определений по версиям,
  а не число различных команд и не автоматическая оценка точности смысла.
- Генерация грамматики воспроизводится: 252 формы синтаксиса, 431 вариант
  вложенных опций, 82 записи значений, 35 правил присутствия, 38 конфликтов
  вложенных опций, 6 правил кавычек и один версионный конфликт команд.
- Встроенная справка содержит 166 основных пояснений и 460 контекстных
  записей: 301 пояснение опции и 159 дополнений к командам/разделам.

Снимки тестовой формы сохраняются локально в
`out/ui-parameter-builder/semantic-audit-2026-10-20/`. Проверочные изображения не включаются в поставку.
Снимок `repository-user-rights.bmp` просмотрен: видны заголовок `-Rights`,
четыре назначения прав в справке, одиночное значение `ReadOnly` и одна опция
`-Rights ReadOnly` в строке. Форма с заполненными синтетическими именем и
паролем допускает применение. Контракт формы проверяет замену ранее
выбранного `Administration`, а не добавление второго экземпляра опции.
Клиент 1С и операции с реальными базами или хранилищами в этой ревизии
не запускались.

## Реестр проверенных пунктов

Группировка ниже охватывает каждый из 166 пунктов каталога ровно один раз.
Существенные изменения и спорные случаи перечислены выше; для остальных
пунктов сохранены проверенные собственные описания и версионные уточнения.

| Группа каталога | Проверено пунктов | Команды и справочные разделы |
| --- | ---: | --- |
| Подключение | 10 | `/F`, `/S`, `/WS`, `/IBName`, `/IBConnectionString`, `/O`, `/TComp`, `UsePrivilegedMode`, `/SLev`, `/Z` |
| Аутентификация | 15 | `/N`, `/P`, `/ModifyPassword`, `/WA`, `/WSA`, `/WSN`, `/WSP`, `/NoProxy`, `/Proxy`, `/OIDA`, `/AccessToken`, `/Authoff`, `/SAOnRestart`, `/ResetSavedAuth`, `/EmailAuth` |
| Клиент и режим | 6 | `/AppAutoCheckVersion`, `/AppAutoCheckMode`, `/RunModeOrdinaryApplication`, `/RunModeManagedApplication`, `/AppArch`, `/MainWindowMode` |
| Сертификаты и TLS | 7 | `/HttpsCert`, `/HttpsCA`, `/HttpsForceTLS1_0`, `/HttpsForceTLS1_1`, `/HttpsForceTLS1_2`, `/HttpsNSS`, `/HttpsForceSSLv3` |
| Интерфейс | 12 | `/i85`, `/Theme`, `/iTaxi`, `/itdi`, `/TechnicalSpecialistMode`, `/L`, `/VL`, `/DisplayAllFunctions`, `/isdi`, `/AppCaptionFromInfobaseList`, `/AppShortCaption`, `/AppCaption` |
| Отладка и тестирование | 8 | `/debug`, `/debuggerURL`, `DisplayPerformance`, `/EmulateServerCallDelay`, `/TestManager`, `/TestClient`, `/UILogRecorder`, `/LogUI` |
| Проверки | 4 | `/EnableCheckModal`, `/EnableCheckExtensionsAndAddInsSyncCalls`, `/EnableCheckServerCalls`, `/EnableCheckScriptCircularRefs` |
| Прочие | 22 | `/C`, `/ClearCache`, `/AllowExecuteScheduledJobs`, `/UC`, `/RunShortcut`, `/AppAutoInstallLastVersion`, `/Execute`, `/URL`, `/@`, `/Out`, `/DisableStartupMessages`, `/DisableStartupDialogs`, `/DisableSplash`, `/DisableUnrecoverableErrorMessage`, `/DisableHomePageForms`, `/DisableBackgroundIndexBuild`, `/UseHwLicenses`, `/DisplayUserNotificationList`, `/DisableLocalSpeechToText`, `/ManageLicenses`, `/ManageServers`, `/DisableExtensions` |
| Режимы процесса | 4 | `ENTERPRISE`, `DESIGNER`, `CREATEINFOBASE`, `CREATEINFOBASE: connection` |
| Пакетные команды | 29 | `/DumpIB`, `/RestoreIB`, `/IBRestoreIntegrity`, `/DumpCfg`, `/LoadCfg`, `/MergeCfg`, `/CompareCfg`, `/UpdateDBCfg`, `/DumpDBCfg`, `/DumpDBCfgList`, `/RollbackCfg`, `/DeleteCfg`, `/DumpConfigFiles`, `/LoadConfigFiles`, `/DumpConfigToFiles`, `/LoadConfigFromFiles`, `/GetConfigGenerationID`, `/ModuleGetSourceCodeAccess`, `/ModuleSetSourceCodeAccess`, `/CheckModules`, `/CheckConfig`, `/CheckCanApplyConfigurationExtensions`, `/IBCheckAndRepair`, `/UpdateCfg`, `/ManageCfgSupport`, `/Visible`, `/RunEnterprise`, `/ConvertFiles`, `/DumpResult` |
| Поставка и данные | 11 | `/CreateTemplateListFile`, `/CreateDistributivePackage`, `/CreateDistributionFiles`, `/CreateDistributive`, `/SignCfg`, `/DumpExternalDataProcessorOrReportToFiles`, `/LoadExternalDataProcessorOrReportFromFiles`, `/ReduceEventLogSize`, `/EraseData`, `/SetPredefinedDataUpdate`, `/ResetMasterNode` |
| Мобильные команды | 6 | `/MobileAppUpdatePublication`, `/MobileAppWriteFile`, `/MobileClientWriteFile`, `/MobileClientDigiSign`, `/MAUpdatePublication`, `/MAWriteFile` |
| Хранилище | 19 | `/ConfigurationRepositoryF`, `/ConfigurationRepositoryN`, `/ConfigurationRepositoryP`, `/ConfigurationRepositoryCreate`, `/ConfigurationRepositoryAddUser`, `/ConfigurationRepositoryCopyUsers`, `/ConfigurationRepositoryLock`, `/ConfigurationRepositoryUnLock`, `/ConfigurationRepositoryCommit`, `/ConfigurationRepositoryBindCfg`, `/ConfigurationRepositoryUnbindCfg`, `/ConfigurationRepositoryDumpCfg`, `/ConfigurationRepositoryUpdateCfg`, `/ConfigurationRepositorySetLabel`, `/ConfigurationRepositoryReport`, `/ConfigurationRepositoryOptimizeData`, `/ConfigurationRepositoryClearCache`, `/ConfigurationRepositoryClearLocalCache`, `/ConfigurationRepositoryClearGlobalCache` |
| Агент и OLE | 8 | `/AgentMode`, `/AgentPort`, `/AgentListenAddress`, `/AgentSSHHostKey`, `/AgentSSHHostKeyAuto`, `/AgentBaseDir`, `/RegServer`, `/UnregServer` |
| Web и mobile | 2 | `Web: query`, `Mobile: launch` |
| Создание базы | 3 | `/AddToList`, `/AddInList`, `/UseTemplate` |
