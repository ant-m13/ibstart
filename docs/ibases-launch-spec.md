# Спецификация совместимости ibases.v8i и запуска 1С

Статус: рабочая спецификация для последующей реализации.

Дата проверки: 2026-08-26.

Этот документ фиксирует результаты аудита текущего IBStart и техническое
задание на расширение совместимости. Это не копия документации 1С и не замена
проверке поведения конкретной установленной версии платформы.

## 1. Источники, область действия и юридическая граница

Проверены два локальных снимка страниц 1С:ИТС, переданных вместе с задачей:

- приложение 3 из ветки документации 1С:Предприятие 8.5.1 — описание
  служебных файлов, включая *.v8i;
- приложение 7 из ветки документации 1С:Предприятие 8.5.1 — режимы запуска,
  параметры командной строки, пакетный конфигуратор, строки соединения,
  web- и mobile-клиент.

Наличие параметра в приложении 7 не означает, что он поддерживается каждой
старой версией 8.3; новые версии 8.5 могут добавлять ключи и менять
ограничения. Перед выпуском каждой реализации нужна матрица проверок по
фактически заявленным версиям 1С.

Внешние справочные ссылки:

- [Приложение 3. Описание и расположение служебных файлов](https://its.1c.ru/db/v8326doc/content/109/hdoc) — формат и расположение `ibases.v8i`;
- [Приложение 7. Параметры командной строки запуска «1С:Предприятия»](https://its.1c.ru/db/v8326doc/content/113/hdoc) — параметры и команды запуска;
- [ограничения по использованию материалов 1С:ИТС](https://its.1c.ru/docs/terms_of_use);
- [сведения об 1С:ИТС и правах на материалы](https://its.1c.ru/db/content/aboutits/src/%D0%B8%D1%81%201%D1%81%20%D0%B8%D1%82%D1%81.htm).
- [политика лицензирования материалов 1С:ИТС в этом проекте](1c-license-compliance.md).

### 1.1. Что можно включать в репозиторий

Можно оставить:

- собственный код разбора и формирования команд;
- собственные типы данных, таблицы сопоставления и проверки;
- краткие пересказы поведения, необходимые для совместимости;
- названия ключей, команд и значений, являющиеся частью внешнего формата;
- ссылки на официальные страницы 1С и тестовые векторы с вымышленными
  адресами, именами и идентификаторами.

Нельзя без отдельной проверки прав включать:

- исходные тексты приложенных .mht, HTML, CSS, изображения или извлечённые
  полные страницы;
- длинные дословные фрагменты, скриншоты и скопированные примеры из 1С:ИТС;
- реальные ibases.v8i, строки подключения, пароли, токены, сертификаты и
  пользовательские пути;
- исходный код платформы 1С или файлы, извлечённые из её установки.

Снимки из задачи — справочные материалы, а не инструкции, которые нужно
переносить в проект. По опубликованным условиям 1С:ИТС исключительные права
на материалы принадлежат ООО «1С»; ссылка на источник не превращает текст
документации в часть лицензии MIT. Поэтому этот файл содержит только
собственную инженерную спецификацию и ссылки. Это инженерная оценка границы
распространения, а не индивидуальное юридическое заключение; для коммерческой
публикации производных материалов при необходимости нужно получить разрешение
правообладателя.

### 1.2. Цель: полная поддержка и обратная совместимость

Этот документ является долговременной спецификацией, а не описанием только
текущего MVP. Реализация должна одновременно:

- читать старые варианты `ibases.v8i`, включая неизвестные поля и старые формы
  `Connect`;
- сохранять неизвестные поля, порядок строк, пустые значения, комментарии и
  исходное представление до явного изменения пользователем;
- добавлять новые поля и параметры только расширением модели, не меняя смысл
  уже поддерживаемых значений;
- различать профиль версии 1С, capability установленного клиента и режим
  запуска; наличие имени параметра в справочнике не доказывает его поддержку
  конкретным `1cv8.exe`;
- формировать совместимую команду только после валидации, разрешения
  конфликтов и проверки области действия параметра;
- иметь regression matrix минимум для старого снимка приложения 3 (8.3.20),
  контрольной ветки приложения 7 (8.3.26) и актуального снимка 8.5.1;
- не считать необработанный текст `AdditionalParameters` заменой typed-поддержки.

Для каждого ключа и параметра в будущем необходимо хранить статус
`apply`, `preserve`, `external` или `unsupported`, поддерживаемые версии,
контекст, grammar, политику секретности и golden-тест. Изменение поведения
допустимо только через версионируемую policy, чтобы обновление IBStart не
переписывалось задним числом в несовместимый формат.

## 2. Итог аудита текущего IBStart

### 2.1. Что уже работает

| Область | Текущее поведение | Основные места |
| --- | --- | --- |
| Кодировка | UTF-8 с BOM и без BOM; сохраняются BOM, вид перевода строк и завершающий перевод строки; некорректный UTF-8 отклоняется. | src/core/v8i/v8i_document.cpp |
| Секции и поля | Строка вида [Имя] создаёт секцию; строка с первым = становится полем; остальные строки сохраняются как непрозрачные. | src/core/v8i/v8i_document.cpp |
| Round-trip | Неизвестные поля, комментарии и пустые строки сохраняются в исходном порядке относительно полей. | src/core/v8i/v8i_document.hpp, src/core/v8i/v8i_document.cpp |
| Дерево | Секция с Connect считается базой, без Connect — группой; учитываются Folder, OrderInList, OrderInTree. | src/core/catalog/catalog.cpp |
| Подключение | Распознаются File, Srvr/Ref, WS и прямой http(s) URL; неизвестный вариант передаётся через /IBConnection. | src/core/connection/connection_string.cpp, src/core/launcher/command_builder.cpp |
| Базовый запуск | Формируются ENTERPRISE/DESIGNER, затем /F, /S, /WS или /IBConnection; добавляются raw-параметры. | src/core/launcher/command_builder.cpp |
| Платформа | Учитываются версия, x86/x64, тип клиента и наличие 1cv8c.exe; политика IBStart для авто-выбора предпочитает x64. | src/core/launcher/command_builder.cpp |
| Сохранение | Fingerprint, mutex, конфликт внешнего изменения, backup до пяти файлов, временный файл и атомарная замена. | src/core/v8i/v8i_file_store.cpp |
| Маскирование | Некоторые пароли/токены обнаруживаются; лог маскирует известные формы и скрывает fallback-операнд /IBConnection. | src/core/logging/logging.cpp, src/ui/main_window.cpp |

### 2.2. Что сохраняется, но не применяется полностью

| Возможность | Текущее состояние | Требуемое решение |
| --- | --- | --- |
| External | Читается, показывается и сохраняется; на запуск не влияет. | Оставить metadata источника, не приписывать новую семантику. |
| Locale | Читается, редактируется и сохраняется; в команду не преобразуется. В формальном списке полей приложения 3 8.5.1 его нет, но он встречается в строках соединения и текущей модели проекта. | Оставить как preserve/extension до отдельной проверки. |
| StartupErrorHelpURL, StartupErrorHelpText | Неизвестные поля сохраняются, но не отображаются и не применяются. | Определить, должен ли desktop IBStart редактировать подсказку штатного клиента. |
| DefaultVersion | Используется как запасная версия при пустом Version. | Разделить предпочтительную, фактически выбранную и автоматическую версии. |
| DefaultApp | Используется как запасной тип клиента. | Реализовать семантику AppAutoCheckMode или явно объявить упрощённую политику. |
| AdditionalParameters | Разбирается в Windows-аргументы и передаётся почти без фильтрации; отдельно распознаётся AppArch. | Ввести реестр параметров, область действия, конфликты, preview и redaction. |
| ClientConnectionSpeed | Читается и сохраняется, но /ONormal и /OLow не добавляются. | Применять только к thin/web-клиенту и не дублировать явный /O. |
| WA | Читается и сохраняется, но /WA+ и /WA- не добавляются. | Преобразовывать в typed launch option. |
| Web extras | Из WS извлекается URL, но остальные части Connect, например WA, теряются при построении /WS. | Передавать поддерживаемые extras с сохранением неизвестных фрагментов. |

### 2.3. Существенные пробелы и риски

1. Typed-модель знает только ограниченный набор полей. Нет WSA, proxy,
   сертификатов, mobile/web-common полей и многих connection-string keys.
2. Любой Connect, даже пустой, делает секцию базой. Нет отдельной диагностики
   пустого или неоднозначного подключения.
3. Не проверяются дубликаты имён секций и ID, дубликаты ключей, корректность
   Folder, диапазоны order и согласованность UseProxy с PSrv/PPort.
4. connection::Split не моделирует все варианты экранирования кавычек, а
   QuoteValue заменяет кавычку апострофом, что может изменить данные.
5. Строка server-подключения строится как Srvr плюс обратная косая черта плюс
   Ref; отдельно нужно различать формат /S, Srvr внутри строки, IPv6 и список
   кластеров.
6. Raw-параметры могут содержать /F, /S, /WS, /IBName, /IBConnectionString и
   взаимоисключающие режимы. Сейчас общий resolver отсутствует.
7. Только AppArch имеет специальный приоритет; остальные дубликаты не
   анализируются.
8. Текущая очистка кэша намеренно безопаснее полного поведения 1С и не должна
   расширяться до произвольных путей из Connect или /ClearCache.
9. Сертификаты, web/mobile, тест-менеджер, пакетный конфигуратор и OLE требуют
   отдельных API и ручной проверки; простая передача строки не равна полной
   реализации.

## 3. Формат ibases.v8i

### 3.1. Физический контракт редактора

- принимать UTF-8 с BOM и без BOM;
- секции имеют вид [Имя секции];
- поля имеют вид Ключ=Значение, значение может быть пустым;
- сохранять порядок секций и полей;
- сохранять комментарии, пустые строки и строки без =;
- неизвестные ключи не терять при изменении известного поля;
- при чтении не менять исходный файл;
- записывать только после проверки внешнего изменения, с backup и атомарной
  заменой.

Этот контракт относится к редактору IBStart, а не утверждает, что любой
произвольный файл принимает штатный стартер 1С. Диагностика должна сообщать
ошибку, но не исправлять неизвестные поля молча.

### 3.2. Секции

| Секция | Признак | Политика |
| --- | --- | --- |
| База | Есть ключ Connect | Показать в каталоге; пустой Connect — ошибка запуска, но запись не уничтожать. |
| Группа | Нет ключа Connect | Использовать имя, Folder и порядок; прочие поля сохранять. |
| Повтор имени | Одинаковое имя без учёта регистра | Не выбирать первую молча; заблокировать неоднозначные операции и показать ошибку. |
| Повтор ID | Несколько баз имеют один непустой ID | Предупредить; не связывать с таким ID новые теги, историю или очистку кэша без подтверждения. |

### 3.3. Полный реестр полей секции

Статусы относятся к проекту:

- PRESERVE — сохранять без самостоятельной семантики;
- PARTIAL — часть значения применяется, остальное только сохраняется;
- TARGET — добавить typed-модель, проверку и построение запуска;
- EXTERNAL — mobile/общий список; не активировать desktop-поведение.

| Ключ | Область | Назначение | Статус |
| --- | --- | --- | --- |
| Connect | база | Строка подключения; обязательна для записи базы в актуальном снимке. | PARTIAL |
| ID | база | Уникальный идентификатор; fallback к имени допустим только для старых списков. | PARTIAL |
| Folder | база/группа | Родительская группа; штатный вид — путь от /. | PARTIAL |
| OrderInList | база/группа | Порядок в плоском списке. | PARTIAL |
| OrderInTree | база/группа | Порядок в ветви. | PARTIAL |
| External | база | Признак внешней записи. | PARTIAL |
| UseProxy | web | 0 без proxy, 1 авто, 2 явная настройка. | TARGET |
| PSrv | web | Адрес proxy при UseProxy=2. | TARGET |
| PPort | web | Порт proxy при UseProxy=2. | TARGET |
| PUser | web | Пользователь proxy. | TARGET |
| PPasswd | web | Зашифрованный пароль proxy; не расшифровывать и не логировать. | TARGET |
| ClientConnectionSpeed | thin/web | Normal или Low; соответствует /O. | PARTIAL |
| App | база | Auto, ThinClient, ThickClient, WebClient. | PARTIAL |
| AppArch | база | x86, x86_64, x86_prt, x86_64_prt. | PARTIAL |
| DefaultApp | база | Thin/thick для автоматического определения. | PARTIAL |
| WA | база | Аутентификация ОС при входе в базу. | PARTIAL |
| WSA | web | Аутентификация ОС на web-сервере. | TARGET |
| Version | база | Требуемая версия платформы; может быть неполной. | PARTIAL |
| DefaultVersion | база | Версия, фактически выбранная стартером. | PARTIAL |
| AdditionalParameters | база | Raw-строка дополнительных аргументов. | PARTIAL |
| WebCommonInfoBaseURL | общий список | Источник записи из интернет-сервиса. | TARGET/PRESERVE |
| HttpsCA | web | None, File, Windows, Linux, macOS. | TARGET |
| HttpsCert | web | None, File, Windows, Linux, macOS. | TARGET |
| HttpsCAFile | web | Файл корневых сертификатов для HttpsCA=File. | TARGET |
| HttpsCertFile | web | Файл сертификата и закрытого ключа для HttpsCert=File. | TARGET |
| HttpsCertSelect | web | Recent, Choose или Auto. | TARGET |
| StartupErrorHelpURL | desktop/client | URL помощи при ошибке подключения. | TARGET/PRESERVE |
| StartupErrorHelpText | desktop/client | Текст или сериализованная форматированная подсказка. | TARGET/PRESERVE |
| ShowInList | mobile | 1 — основной список, 0 — специальное меню. | EXTERNAL |
| MobilePublicKey | mobile | Хеш открытого ключа для проверки подписи. | EXTERNAL/PRESERVE |
| WebCommonInfoBases | общий список/mobile | URL сервиса списка общих баз. | TARGET/PRESERVE |
| InternetService | mobile/общий список | URL сервиса списка баз/дистрибутива; может быть вложенным. | EXTERNAL |
| DisplayAuthDialog | mobile | Управление диалогом аутентификации. | EXTERNAL |
| DisableUseBiometrics | mobile | Скрывает настройку биометрии. | EXTERNAL |
| UseBiometrics | mobile | Начальное состояние биометрии: 1/0. | EXTERNAL |
| DisableRememberMe | mobile | Скрывает запоминание входа. | EXTERNAL |
| RememberMe | mobile | Начальное состояние запоминания: 1/0. | EXTERNAL |
| Locale | connection/extension | Есть в текущей модели и строках соединения; не считать подтверждённым полем desktop списка. | PRESERVE |

Приложение 3 8.5.1 формально перечисляет основные поля, а UseProxy, PSrv,
PPort, PUser, PPasswd и mobile-поля описывает в последующих подразделах того
же раздела. StartupErrorHelpURL и StartupErrorHelpText добавлены в актуальном
снимке и должны быть учтены как новые ключи.

### 3.4. Строка Connect

| Вариант | Минимальные фрагменты | План |
| --- | --- | --- |
| Файловый | File=каталог | /F каталог; наличие 1Cv8.1CD обязательно для ручного добавления, не для чтения существующей записи. |
| Клиент-серверный | Srvr=адрес и Ref=имя | /S адрес/имя либо Windows-вариант после проверки целевой платформы. |
| Web | WS=http(s)-url | /WS url для thin-клиента; extras не терять. |
| Legacy web | первый фрагмент — прямой http(s) URL | Нормализовать только в typed-view; raw сохранять до явной записи. |
| Неполный/неизвестный | нет минимального набора | Диагностика; /IBConnection только по явному безопасному правилу. |

Нужны отдельные ConnectionFragment и ConnectionKind. Нельзя строить connection
string заменой кавычек: требуются escaped/doubled quotes, quoted значения с
точкой с запятой, пустые значения, порядок фрагментов и round-trip unknown.

## 4. Архитектура полной реализации

Поток данных:

    ibases.v8i bytes
        -> lossless V8iDocument
        -> validated CatalogEntry + typed V8iFieldSet
        -> LaunchRequest
        -> ParameterRegistry / conflict resolver
        -> LaunchPlan (executable + argv + redacted preview + warnings)
        -> CreateProcessW / ShellExecuteW / browser / explicit operation

UI не должен разбирать ibases.v8i или склеивать командную строку.
process_launcher получает только проверенный LaunchPlan.

### 4.1. Доменные типы

Нужно добавить, не ломая lossless-слой:

- V8iFieldSet — typed view с raw value, normalized value, источником и
  предупреждениями;
- ConnectionSpec — file, server, web, legacy_web, unknown и упорядоченные
  known/unknown fragments;
- LaunchContext — enterprise, designer_batch, create_infobase, client_batch,
  ole_registration, web_url, mobile;
- LaunchParameter — имя, форма, чувствительность, контексты, cardinality и
  precedence;
- LaunchRequest — намерение пользователя и typed options;
- LaunchPlan — конечные executable/arguments, warnings и redacted preview.

Raw AdditionalParameters сохранять для неизвестных ключей. Известные ключи
сначала не переписывать автоматически в исходной строке: формировать план,
показывать diff/предупреждение и менять raw text только после отдельного
подтверждения.

### 4.2. Приоритеты и конфликты

1. Нельзя одновременно выбрать ENTERPRISE, DESIGNER, CREATEINFOBASE,
   OLE-регистрацию и batch-команду конфигуратора.
2. Действие UI задаёт режим процесса; противоречащий raw-режим — конфликт.
3. Валидный /AppArch из дополнительных аргументов имеет приоритет над полем
   AppArch; невалидное значение должно быть диагностировано.
4. Typed ClientConnectionSpeed, WA, WSA, version и client type добавлять
   только при отсутствии соответствующего reserved parameter в raw-аргументах.
   Два значения одного reserved parameter запрещать по умолчанию.
5. /IBConnectionString разрешать с учётом порядка: он должен находиться до
   параметров, которые могут менять его части; добавление его в конец неверно.
6. /@ — первая/единственная команда или полная замена command line; нельзя
   смешивать с автоматически сгенерированным подключением.
7. /Execute, /URL, /RunShortcut, /ClearCache, /RegServer, /UnregServer и
   очистка saved auth требуют отдельного контекста и подтверждения.
8. /P, /WSP, -pwd, PPasswd, AccessToken, UC, SPwd, DBPwd и похожие значения
   никогда не попадают в обычный preview, лог, shortcut, clipboard или настройки.

### 4.3. Версии и разрядность

Различать:

- exact version — полный номер с build;
- prefix version — например 8.3.27, совпадающий с 8.3.27.x;
- selected installed version;
- DefaultVersion как значение списка, не доказательство установки;
- auto-check как отдельную политику /AppAutoCheckVersion.

В desktop MVP разрешено искать только установленные платформы и честно
сообщать, что дистрибутивы автоматически не устанавливаются. Нельзя обещать
полный алгоритм 1С /AppAutoCheckVersion, если IBStart не реализует источник
дистрибутива и установку.

Политика AppArch:

    x86          -> только x86
    x86_64       -> только x64
    x86_prt      -> приоритет x86
    x86_64_prt   -> приоритет x64
    empty        -> явная политика IBStart

В snapshot 1С x86_prt описан как default, а текущий IBStart для пустого
значения предпочитает x64. Это различие нужно показывать в документации и
покрыть тестом.

## 5. Полный реестр командной строки

Инвентарь ниже пересказывает проверенный snapshot приложения 7 для планирования.

- PASS — передать в соответствующий клиент после разбора и проверки;
- TYPED — первоклассный объект доменной модели;
- PRESERVE — сохранить, но не обещать выполнение;
- OUT — отдельная операция за пределами обычного desktop launch.

### 5.1. Режим процесса

| Режим | Каноническая форма | План |
| --- | --- | --- |
| Предприятие | 1cv8 ENTERPRISE [параметры] | TYPED/PASS |
| Конфигуратор | 1cv8 DESIGNER [команды] | TYPED; отдельный batch API |
| Создание ИБ | 1cv8 CREATEINFOBASE строка соединения ... | OUT/TYPED |

### 5.2. Подключение

| Параметр | Значение | План |
| --- | --- | --- |
| /F | каталог с 1Cv8.1CD | TYPED; из File |
| /S | адрес server-базы | TYPED; slash/backslash по матрице |
| /IBName | имя базы в списке; дубликат — ошибка | TYPED; resolver |
| /IBConnectionString | полная connection string; порядок важен | TYPED; parser/precedence |
| /WS | web URL | TYPED; из WS |
| /O плюс Normal/Low | скорость соединения | TYPED; из ClientConnectionSpeed |
| /TComp плюс -None/-Deflate/-SDC/-lz4/-zstd | сжатие трафика | PASS после whitelist |
| UsePrivilegedMode | привилегированный сеанс | PASS с предупреждением |
| /SLev плюс 0/1/2 | защищённость server connection | TYPED/PASS |
| /Z | значения разделителей | TYPED/PASS |

### 5.3. Аутентификация

| Параметр | Значение/ограничение | План |
| --- | --- | --- |
| /N | имя пользователя | PASS |
| /P | пароль | PASS; SECRET |
| /ModifyPassword | новый пароль вместе с /N и /P | PASS; SECRET; interactive-only |
| /WA | плюс/минус, auth ОС базы | TYPED; из WA |
| /WSA | плюс/минус, auth ОС web-сервера | TYPED; из WSA |
| /WSN | имя для auth web-сервера | PASS |
| /WSP | пароль web-сервера | PASS; SECRET |
| /NoProxy | запрет proxy для WS | TYPED; конфликт с /Proxy |
| /Proxy | -PSrv, -PPort, optional -PUser, -PPwd | TYPED; -PPwd SECRET |
| /OIDA | плюс/минус, OpenID auth | PASS/TYPED |
| /AccessToken | JWT | PASS; SECRET |
| /Authoff | OpenID/OpenID Connect logout | PASS |
| /SAOnRestart | требовать auth при restart | PASS |
| /ResetSavedAuth | удалить сохранённые токены и имена | PASS; warning |
| /EmailAuth | email и код подтверждения | PASS; interactive-only |

### 5.4. Определение режима, версии и клиента

| Параметр | Значение/ограничение | План |
| --- | --- | --- |
| /AppAutoCheckVersion | optional плюс/минус | TYPED; ограничить auto-install |
| /AppAutoCheckMode | определить приложение по базе/пользователю | TYPED; two-stage launch |
| /RunModeOrdinaryApplication | thick, обычное приложение | PASS; thick-only |
| /RunModeManagedApplication | thick, управляемое приложение | PASS; thick-only |
| /AppArch | x86, x86_prt, x86_64, x86_64_prt | TYPED; частично есть |
| /MainWindowMode | Normal, Workplace, EmbeddedWorkplace, FullscreenWorkplace, Kiosk | TYPED/PASS |

### 5.5. Сертификаты и TLS

| Параметр | Допустимые элементы | План |
| --- | --- | --- |
| /HttpsCert | -windows, -linux, -macos, -recent, -auto, -choose, -file path, -pwd password, -none | TYPED; password SECRET |
| /HttpsCA | -windows, -linux, -macos, -file path, -pwd password, -none | TYPED; не менять trust store |
| /HttpsForceTLS1_0 | TLS 1.0 | PASS с security warning |
| /HttpsForceTLS1_1 | TLS 1.1 или старше | PASS с security warning |
| /HttpsForceTLS1_2 | TLS 1.2 или старше | PASS |

Несколько /HttpsForceTLS* одновременно запрещать. Пути файлов проверять перед
запуском, содержимое сертификатов не читать и не копировать.

### 5.6. Интерфейс и локализация

| Параметр | План |
| --- | --- |
| /i85 | PASS для клиента 8.5 |
| /Theme Dark или /Theme Light | PASS; проверить только interface 8.5 |
| /iTaxi | PASS |
| /itdi | PASS/PRESERVE; подтвердить фактическое поведение |
| /TechnicalSpecialistMode | PASS |
| /L код языка | TYPED; не смешивать с /VL |
| /VL код локализации сеанса | TYPED; не смешивать с /L |

### 5.7. Отладка

| Параметр | План |
| --- | --- |
| /debug [tcp/http] [-attach] | PASS; -attach только HTTP |
| /debuggerURL url | PASS; URL warning |
| DisplayPerformance | PASS |
| /EmulateServerCallDelay [-Call n] [-Send n] [-Recevie n] | PASS; сохранять официальное написание Recevie |

Каждая задержка не должна превышать 9.99 секунд; parsing не должен менять
десятичный формат, принимаемый клиентом.

### 5.8. Тестирование

| Параметр | План |
| --- | --- |
| /TestManager | PASS; thick/thin manager |
| /TestClient [-TPort n] [-TestClientID id] [-TURL=url] | PASS; context-aware |
| /UILogRecorder [-TPort n] [-File path] | PASS; подтверждение записи файла |

### 5.9. Проверки выполнения

| Параметр | План |
| --- | --- |
| /EnableCheckModal | PASS |
| /EnableCheckExtensionsAndAddInsSyncCalls | PASS; ограничения thick клиента |
| /EnableCheckServerCalls | PASS |
| /EnableCheckScriptCircularRefs | PASS |

### 5.10. Вспомогательные параметры

| Параметр | План |
| --- | --- |
| /C text | PASS; один аргумент со всеми пробелами |
| /ClearCache | PASS только после allowlist-политики |
| /AllowExecuteScheduledJobs -Off или -Force | PASS; -Force warning |
| /UC code | PASS; SECRET |
| /RunShortcut file | OUT/PASS; безопасный выбор v8i/v8l |
| /AppAutoInstallLastVersion плюс/минус | PASS/PRESERVE; IBStart не устанавливает сам |
| /Execute external processor | OUT; trusted-file policy |
| /URL e1c/http(s) link | TYPED; resolver запущенного клиента или новый launch |

/Execute имеет приоритет над /URL; абсолютный /URL может сделать подключение
из /F, /S, /WS или частей /IBConnection неактуальным.

### 5.11. Прочие параметры клиента

| Параметр | План |
| --- | --- |
| /@ command-file | TYPED; только первая/единственная команда |
| /Out file [-NoTruncate] | TYPED; batch; policy перезаписи |
| /DisableStartupMessages | PASS |
| /DisableStartupDialogs | PASS; non-interactive semantics |
| /DisableSplash | PASS |
| /DisableUnrecoverableErrorMessage | PASS; unattended launch |
| /DisableHomePageForms | PASS |
| /DisableBackgroundIndexBuild | PASS; designer-related |
| /UseHwLicenses плюс/минус | PASS |
| /DisplayUserNotificationList | PASS |

### 5.12. Пакетные команды конфигуратора

Эти команды нельзя добавлять в обычное поле AdditionalParameters без отдельного
режима «операция конфигуратора». Реестр:

| Группа | Команды и опции |
| --- | --- |
| Выгрузка/загрузка | /DumpIB file; /RestoreIB file [-JobsCount n] |
| Восстановление | /IBRestoreIntegrity |
| Конфигурация | /DumpCfg cf/cfe [-Extension name]; /LoadCfg cf/cfe [-Extension name]; /MergeCfg cf/cfe -Settings file [-EnableSupport/-DisableSupport] [-IncludeObjectsByUnresolvedRefs/-ClearUnresolvedRefs] [-Extension name] [-force] |
| Сравнение | /CompareCfg -FirstConfigurationType type [-FirstName name] [-FirstFile file] [-FirstVersion version] -SecondConfigurationType type [-SecondName name] [-SecondFile file] [-SecondVersion version] [-MappingRule file] [-Objects file] -ReportType type [-IncludeChangedObjects] [-IncludeDeletedObjects] [-IncludeAddedObjects] -ReportFormat format -ReportFile file |
| Обновление | /UpdateDBCfg [-Dynamic mode] [-BackgroundStart] [-BackgroundCancel] [-BackgroundFinish [-Visible]] [-BackgroundSuspend] [-BackgroundResume] [-WarningsAsErrors] [-Server] [-v1/-v2] [-Extension name] [-SessionTerminate mode] |
| База конфигурации | /DumpDBCfg cf/cfe [-Extension name]; /DumpDBCfgList [-Extension name] [-AllExtensions]; /RollbackCfg [-Extension name]; /DeleteCfg [-Extension name] [-AllExtensions] |
| Файлы конфигурации | /DumpConfigFiles dir [-Module] [-Template] [-Help] [-AllWritable] [-Picture] [-Right] [-Extension name]; /LoadConfigFiles dir [-Module] [-Template] [-Help] [-AllWritable] [-Picture] [-Right] [-Extension name] |
| Dump/load files | /DumpConfigToFiles dir [-Format format] [-Extension name] [-AllExtensions] [-update] [-force] [-getChanges file] [-configDumpInfoForChanges file] [-listFile file] [-configDumpInfoOnly] [-Server [-JobsCount n]] [-Archive file] [-ignoreUnresolvedReferences]; /LoadConfigFromFiles dir [-Extension name] [-AllExtensions] -files file -listFile file -Format format [-updateConfigDumpInfo] [-NoCheck] [-Archive zip] -partial [-JobsCount n] |
| Generation/source | /GetConfigGenerationID [-Extension name]; /ModuleGetSourceCodeAccess file; /ModuleSetSourceCodeAccess file |
| Проверка модулей | /CheckModules с -ThinClient, -WebClient, -MobileClient, -MobileClientStandalone, -MobileAppClient, -Server, -MobileAppServer, -ExternalConnection, -ThickClientOrdinaryApplication, -ExtendedModulesCheck, -Extension, -AllExtensions |
| Проверка конфигурации | /CheckConfig [-ConfigLogIntegrity] [-IncorrectReferences] [-ThinClient] [-WebClient] [-MobileClient] [-MobileAppClient] [-Server] [-MobileAppServer] [-MobileClientStandalone] [-ExternalConnection] [-ExternalConnectionServer] [-ThickClientManagedApplication] [-ThickClientServerManagedApplication] [-ThickClientOrdinaryApplication] [-ThickClientServerOrdinaryApplication] [-DistributiveModules] [-UnreferenceProcedures] [-HandlersExistence] [-EmptyHandlers] [-ExtendedModulesCheck] [-CheckUseModality] [-CheckUseSynchronousCalls] [-UnsupportedFunctional] [-MobileClientDigiSign] [-Extension name] [-AllExtensions] |
| Проверка расширений | /CheckCanApplyConfigurationExtensions [-Extension name] [-AllZones] [-Z values] |
| Восстановление базы | /IBCheckAndRepair [-ReIndex] [-LogIntegrity [MDtype[,MDtype]]] или [-LogAndRefsIntegrity [MDtype[,MDtype]]] [-RecalcTotals] [-IBCompression] [-Rebuild] [-RebuildStandaloneCfg] [-TestOnly] или [[-BadRefCreate] или [-BadRefClear] или [-BadRefNone]] [[-BadDataCreate] или [-BadDataDelete]] [-UseStartPoint] [-TimeLimit:hhh:mm] [-ConfigurationExtensionsLogIntegrity] [-RefreshTableLocation] [-BinaryDataStorageIntegrity [MDtype[,MDtype]]] [-JobsCount n] [-Z: separators] [-CheckAndEnableCORPFunctionality] |
| Поддержка | /UpdateCfg cf/cfu -Settings file [-IncludeObjectsByUnresolvedRefs/-ClearUnresolvedRefs] [-DumpListOfTwiceChangedProperties] [-force]; /ManageCfgSupport [-disableSupport [-force]] |
| Поставка | /CreateTemplateListFile file [-TemplatesSourcePath]; /CreateDistributivePackage dir -File description -PackageFileName archive [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]; /CreateDistributionFiles [-cffile cf] [-cfufile cfu [-f cf|-v version]+] [-digisign file] [-WarningAsError]; /CreateDistributive dir -File description [-Option variant] [-MakeSetup] [-MakeFiles] [-digisign file] [-WarningAsError]; /SignCfg -ConfigurationType type -SignedFile cfe [-SignType type] [-File cfe] [-Name extension] [-Version version] -digisign file |
| Внешние обработки | /DumpExternalDataProcessorOrReportToFiles root processor [-Format Plain/Hierarchical]; /LoadExternalDataProcessorOrReportFromFiles root processor |
| Mobile package | /MobileAppUpdatePublication; /MobileAppWriteFile zip; /MobileClientWriteFile file; /MobileClientDigiSign |
| Журнал/удаление | /ReduceEventLogSize Date [-saveAs file] [-KeepSplitting]; /EraseData [/Z separators]; /SetPredefinedDataUpdate [-Auto] [-UpdateAutomatically] [-DoNotUpdateAutomatically] |
| DIB | /ResetMasterNode |

Сокращения и регистр (-force, -Extension, -digisign, -Files, -v1) хранить в
registry как canonical tokens и сопоставлять без учёта регистра только после
проверки на целевой версии.

### 5.13. Хранилище конфигурации

| Группа | Реестр |
| --- | --- |
| Доступ | /ConfigurationRepositoryF dir; /ConfigurationRepositoryN name; /ConfigurationRepositoryP password |
| Создание | /ConfigurationRepositoryCreate с -AllowConfigurationChanges, -ChangesAllowedRule, -ChangesNotRecommendedRule, -NoBind, -MinPasswordLenght, -CheckPasswordComplexity, -Extension |
| Пользователи | /ConfigurationRepositoryAddUser -User name -Pwd password -Rights rights [-RestoreDeletedUser] [-Extension name]; /ConfigurationRepositoryCopyUsers -Path path -User name -Pwd password [-RestoreDeletedUser] [-Extension name] |
| Объекты | /ConfigurationRepositoryLock [-Objects file] [-revised] [-Extension name]; /ConfigurationRepositoryUnLock [-Objects file] [-force] [-Extension name]; /ConfigurationRepositoryCommit [-Objects file] [-comment text] [-keepLocked] [-force] [-Extension name] |
| Конфигурация | /ConfigurationRepositoryBindCfg [-forceBindAlreadyBindedUser] [-forceReplaceCfg] [-Extension name]; /ConfigurationRepositoryUnbindCfg [-force] [-Extension name]; /ConfigurationRepositoryDumpCfg cf [-v version] [-Extension name]; /ConfigurationRepositoryUpdateCfg [-v version] [-revised] [-force] [-Objects file] [-Extension name] |
| Отчёт/служебные | /ConfigurationRepositorySetLabel [-v version] [-name] label [-comment text] [-Extension name]; /ConfigurationRepositoryReport file [-NBegin version] [-NEnd version] [-DateBegin date] [-DateEnd date] [-GroupByObject] [-GroupByComment] [-DoNotIncludeVersionsWithLabels] [-IncludeOnlyVersionsWithLabels] [-IncludeCommentLinesWithDoubleSlash] [-ConfigurationVersion version] [-ReportFormat txt|mxl] [-Extension name]; /ConfigurationRepositoryOptimizeData [-Extension name] |
| Кэш | /ConfigurationRepositoryClearCache [-Extension name]; /ConfigurationRepositoryClearLocalCache [-Extension name]; /ConfigurationRepositoryClearGlobalCache [-Extension name] |

Пароли и операции удаления/очистки требуют отдельного подтверждения. Эти
команды не являются безопасным расширением обычного запуска базы.

### 5.14. Агент, OLE и прочие batch

| Группа | Команды |
| --- | --- |
| Агент | /AgentMode; /AgentPort port; /AgentListenAddress address; /AgentSSHHostKey private-key; /AgentSSHHostKeyAuto; /AgentBaseDir dir |
| Прочие batch | /Visible; /RunEnterprise; /ConvertFiles file-or-path; /DumpResult file |
| OLE | /RegServer [-AllUsers/-CurrentUser/-Auto]; /UnregServer |

/RegServer и /UnregServer меняют системную регистрацию и требуют отдельного
подтверждённого режима с проверкой прав.

### 5.15. CREATEINFOBASE connection string

Общие keys: disstt (Y/N), LicDstr (Y/N), prmod (1), Pwd (SECRET), Usr, Z.

Файловый вариант: DBFormat (8.2.14/8.3.8), DBPageSize
(4096/4k, 8192/8k, 16384/16k, 32768/32k, 65536/64k), File, Locale.

Клиент-серверный вариант: CrSQLDB (Y/N), DB, DBMS
(MSSQLServer, PostgreSQL, IBMDB2, OracleDatabase), DBPwd (SECRET), DBSrvr,
DBUID, Locale, Ref, SchJobDn (Y/N), SPwd (SECRET), SQLYOffs (0/2000), Srvr,
SUsr.

Srvr должен поддерживать protocol/port, IPv4, IPv6 и comma-separated cluster
list без пробелов, с отдельными тестами.

## 6. Web- и mobile-клиент

### 6.1. Параметры web-клиента в URL

Это отдельная модель query/navigation options, а не Windows command-line.
Нужно различать URL encoding и quoting аргументов.

| Категория | Имена |
| --- | --- |
| Auth | AccessToken, Authoff, N=value, P=value, OIDA плюс/минус, OidcSelectedProvider, UC=value, WA плюс/минус, UsePrivilegedMode |
| App/navigation | C=value, URL=value, Z=values |
| Debug | Debug=tcp/http с optional attach, DebuggerURL=value, DisplayPerformance |
| UI | DisableHomePageForms, DisableStartupMessages, DisableUnrecoverableErrorMessage, i85, iTaxi, itdi, L=value, MainWindowMode=mode, Theme=Dark/Light, VL=value |
| Notifications | ProgressiveWebApplicationName=value, SYSTEMWEBCLIENTSTAT, TechnicalSpecialistMode |
| Testing | TestClient, TestClientID=value |

MainWindowMode: Normal, Workplace, EmbeddedWorkplace, FullscreenWorkplace,
Kiosk. &, #, пробелы и = должны кодироваться корректно. JWT, password и UC
запрещены в логах и обычном preview.

Allowlist standard web functions:

ActiveUsers, AdditionalAuthenticationSettings, AnalyticsSystemManagement,
AuthenticationLocks, ConfigurationLicense, DataBaseCopiesManagement,
DataChangeHistory, DeleteMarkedObjects, DocumentsPosting,
CollaborationSystemManagement, ErrorProcessingSettings, EventLog,
EventLogSettings, ConfigurationExtensionsManagement,
ExternalDataSourcesManagement, FindByReference, FullTextSearchManagement,
InfobaseParameters, IntegrationServicesManagment, LicenseAcquisition,
MobileAppBuildService, MobileAppBuilderServiceLoader,
InfobaseRegionalSettings, TotalsManagement, UserList, ServersManagement.

До подтверждения новой версии сохранять точное написание
IntegrationServicesManagment.

### 6.2. Mobile-клиент

В snapshot перечислены совместимые группы:

- /N, /P, /WSN, /WSP, /WSA, /OIDA, /Authoff, /UsePrivilegedMode, /Z, /O;
- /HttpsForceTLS1_0;
- /L, /VL;
- /ClearCache, /C, /URL;
- /DisableStartupMessages;
- /TestClient, /UILogRecorder.

Desktop IBStart должен сохранять связанные ibases.v8i поля и показывать их как
external/mobile-only, но не обещать их выполнение в Windows desktop.

## 7. План реализации

### Этап 0 — контракт и fixtures

- Добавить безопасный fixture с каждым полем из раздела 3.3, unknown fields,
  duplicate keys, quoted semicolon и legacy URL.
- Зафиксировать заявленные версии 1С и ручную matrix.
- Добавить диагностический отчёт «прочитан / сохранён / применён / отклонён»
  без секретных значений.
- Не включать MHT и извлечённый HTML в fixtures.

### Этап 1 — lossless parser и диагностика

- Ввести ParseResult с warnings/errors, не меняя round-trip корректного файла.
- Явно обработать duplicate sections/keys, whitespace, empty names, CR-only и
  invalid UTF-8.
- Исправить connection parser для escaped/doubled quotes, semicolon, empty
  values, duplicate fragments, IPv6 и cluster lists.
- Убрать data-loss из QuoteValue, добавить golden tests.

### Этап 2 — typed ibases.v8i

- Добавить desktop-relevant UseProxy, PSrv, PPort, PUser, PPasswd, WSA,
  Https*, StartupErrorHelp*, ClientConnectionSpeed и связанные параметры.
- Mobile/общие поля оставить в extra_fields с видимым scope, пока нет
  отдельного workflow.
- Проверять уникальность ID, order и parent path; unknown/raw formatting не
  менять без явного действия.

### Этап 3 — registry и LaunchPlan

- Единый case-insensitive registry для параметров 5.2–5.11.
- Различать attached/separate/nested option и сохранять значимый порядок.
- Добавить conflict resolver, precedence и LaunchPlan::Warnings.
- Перенести redaction на metadata registry.
- В preview показывать только redacted command; raw secrets не копировать.

### Этап 4 — применение полей

- /O из ClientConnectionSpeed;
- /WA из WA, /WSA из WSA;
- App/DefaultApp с /AppAutoCheckMode;
- Version/DefaultVersion вместе с /AppAutoCheckVersion с ограничением «установленные
  версии»;
- единый AppArch resolver;
- web extras и /Https*;
- /IBName, /IBConnectionString, /URL, /@ с отдельными rules.

### Этап 5 — отдельные операции

- batch Designer: dump/load/check/repair/repository;
- create-infobase;
- client batch;
- web URL/navigation;
- OLE registration;
- agent mode;
- mobile metadata/export.

Каждая операция должна иметь свой builder, scope, confirmation policy,
exit-code handling, output-file policy и tests. Нельзя добавить всё в
AdditionalParameters и назвать это полной реализацией.

## 8. Тесты и критерии готовности

### 8.1. Unit/golden tests

- BOM/no BOM, CRLF/LF/CR, final newline;
- русские имена, пробелы, emoji, quoted semicolon, empty values;
- comments/opaque lines до, между и после fields;
- duplicate section/key diagnostics;
- every field from section 3.3;
- file/server/web/legacy URL, IPv4, IPv6, cluster list;
- all AppArch values and invalid value;
- App, DefaultApp, Version, DefaultVersion, WA, WSA,
  ClientConnectionSpeed;
- every parameter 5.2–5.11, including nested options and secrets;
- conflicts: /F + /S, /NoProxy + /Proxy, multiple TLS flags, /@ not first,
  /Execute + /URL, duplicate /AppArch;
- redacted preview/log;
- atomic save, backup retention, external modification, concurrent save.

### 8.2. Manual/version tests

Для каждой заявленной версии проверить:

1. file, server и web базу;
2. thin/thick и обе разрядности;
3. exact/prefix/missing version;
4. AppArch priority;
5. auth, proxy, certificate и TLS combinations;
6. UI, localization, debug and test-client flags;
7. URL navigation;
8. только те batch-команды, которые явно открыты в UI/API.

Хранить обезличенный manifest: версия, команда без секретов, exit code, факт
запуска и краткая ошибка. Реальные базы и учётные данные в репозиторий не
помещать.

### 8.3. Definition of done

Полную реализацию можно считать готовой только если:

- каждый ключ ibases.v8i имеет apply, preserve, external или unsupported с
  объяснением;
- каждый параметр заявленной версии приложения 7 имеет registry entry, scope,
  grammar, redaction policy и test;
- generated command не содержит противоречащих или случайно дублированных
  connection/mode arguments;
- unknown fields и unknown additional parameters не теряются;
- UI показывает, что будет передано клиенту и что не поддерживается;
- секреты не попадают в log, shortcut, clipboard, fixture или report;
- manual matrix подтверждена на целевых версиях;
- в release package нет MHT, HTML или скопированных фрагментов документации 1С.

## 9. Связь с документацией проекта

- [Конфигурация, локальные данные и файлы](configuration.md) остаётся
  пользовательским справочником текущего поведения.
- [Архитектура](architecture.md) описывает текущие границы модулей; после
  реализации добавить туда parameter_registry, connection_spec и launch_plan.
- Этот документ — основной backlog/spec для расширения совместимости, но не
  обещает включение всех перечисленных операций в ближайший релиз.
