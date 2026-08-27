# Спецификация интеграции `ibases.v8i` и запуска 1С

Статус: основной интеграционный документ и индекс.

Дата проверки: 2026-08-27.

## Назначение

Этот документ описывает границу между списком баз и запуском клиента 1С.
Подробные реестры вынесены в специализированные документы, чтобы не смешивать
два разных контракта:

- [формат и поля `ibases.v8i`](ibases-format.md);
- [параметры и команды запуска](launch-parameters.md);
- [политика лицензирования материалов 1С:ИТС](1c-license-compliance.md).

Текущий файл является собственной инженерной спецификацией для дальнейшей
реализации. Он не заменяет официальную документацию и не содержит копий MHT,
HTML, CSS, изображений или исходного кода 1С.

## Источники

- [Приложение 3. Описание и расположение служебных файлов](https://its.1c.ru/db/v8326doc/content/109/hdoc)
- [Приложение 7. Параметры командной строки запуска «1С:Предприятия»](https://its.1c.ru/db/v8326doc/content/113/hdoc)
- [Ограничения по использованию 1С:ИТС](https://its.1c.ru/docs/terms_of_use)
- [Об 1С:ИТС](https://its.1c.ru/db/content/aboutits/src/%D0%B8%D1%81%201%D1%81%20%D0%B8%D1%82%D1%81.htm)

Проверенные локальные снимки относятся к ветке документации 8.5.1. До выпуска
изменений должна проверяться фактическая версия установленного клиента: наличие
параметра в новом справочнике не доказывает его наличие в старой платформе.

## 1. Итог аудита текущего IBStart

| Область | Текущее состояние | Решение для полной поддержки |
| --- | --- | --- |
| Файл | UTF-8 с BOM/без BOM, порядок строк и неизвестные строки сохраняются. | Сохранить lossless-контракт и добавить диагностику. |
| Секции | Наличие `Connect` определяет базу; без него секция считается группой. | Добавить диагностику пустого подключения, дубликатов имён и `ID`. |
| Поля | Typed-модель содержит базовый набор; остальные поля сохраняются как unknown. | Полный реестр находится в [ibases-format.md](ibases-format.md). |
| `Connect` | Распознаются `File`, `Srvr/Ref`, `WS` и прямой HTTP(S)-URL. | Добавить escaped quotes, quoted semicolon, IPv6, cluster list и round-trip fragments. |
| Запуск | Формируются `ENTERPRISE`/`DESIGNER`, `/F`, `/S`, `/WS`, `/IBConnection` и raw-аргументы. | Ввести typed `LaunchRequest`, registry и `LaunchPlan`. |
| Клиент | Учитываются тип, версия, разрядность и наличие тонкого клиента. | Разделить version profile, capability и режим процесса. |
| Поля запуска | `AppArch` применяется; `WA` и `ClientConnectionSpeed` пока только сохраняются. | Реализовать связь с `/WA`, `/WSA`, `/O`, proxy и сертификатами. |
| Raw-параметры | Windows-аргументы передаются почти без фильтрации; `/AppArch` распознаётся отдельно. | Ввести grammar, конфликт resolver, version check и redaction. |
| Безопасность | Известные секреты маскируются в логах и перед запуском показывается предупреждение; значения в `ibases.v8i`, UI и явном копировании доступны пользователю. | Расширить registry и тесты так, чтобы секреты не попадали в открытом виде в логи и автоматически создаваемые diagnostics/report; fixtures должны быть синтетическими или замаскированными. |

Фактическое mapping полей в коде находится в
[catalog.cpp](../src/core/catalog/catalog.cpp:209), выбор launch options — в
[main_window.cpp](../src/ui/main_window.cpp:1154), построение команды — в
[command_builder.cpp](../src/core/launcher/command_builder.cpp:175).

## 2. Сквозная модель

Поток данных полной реализации:

    ibases.v8i bytes
        -> lossless V8iDocument
        -> validated CatalogEntry + typed V8iFieldSet
        -> LaunchRequest
        -> ParameterRegistry / conflict resolver
        -> LaunchPlan (executable + argv + redacted diagnostics + warnings)
        -> CreateProcessW / ShellExecuteW / browser / explicit operation

UI не должен самостоятельно разбирать `ibases.v8i` или склеивать командную
строку. `process_launcher` получает только проверенный `LaunchPlan`.

### 2.1. Доменные типы

Не ломая lossless-слой, требуется выделить:

- `V8iFieldSet` — raw value, normalized value, источник и warnings;
- `ConnectionSpec` — file, server, web, legacy_web, unknown и упорядоченные
  known/unknown fragments;
- `LaunchContext` — enterprise, designer_batch, create_infobase, client_batch,
  ole_registration, web_url, mobile;
- `LaunchParameter` — имя, форма, чувствительность, контексты, cardinality,
  версии и precedence;
- `LaunchRequest` — намерение пользователя и typed options;
- `LaunchPlan` — executable, arguments, warnings и redacted preview.

## 3. Связь полей и запуска

| Источник | Производный параметр | Статус |
| --- | --- | --- |
| `File` внутри `Connect` | `/F` | Базово реализовано |
| `Srvr` и `Ref` внутри `Connect` | `/S` | Базово реализовано |
| `WS` внутри `Connect` | `/WS` | Базово реализовано |
| `ClientConnectionSpeed` | `/O` | Сохраняется, применение запланировано |
| `WA` | `/WA` | Сохраняется, применение запланировано |
| `WSA` | `/WSA` | Typed-поддержки нет |
| `UseProxy`, `PSrv`, `PPort`, `PUser`, `PPasswd` | `/Proxy` и nested options | Typed-поддержки нет |
| `App`, `DefaultApp` | выбор клиента и `/AppAutoCheckMode` | Частично |
| `Version`, `DefaultVersion` | выбор платформы и `/AppAutoCheckVersion` | Частично |
| `AppArch` | `/AppArch` | Применяется |
| `Https*` | `/HttpsCert`, `/HttpsCA`, TLS flags | Typed-поддержки нет |
| `AdditionalParameters` | raw argv | Частично |

Нельзя считать `AdditionalParameters` заменой реализации: raw-текст не даёт
проверки версии, контекста, конфликтов и чувствительности.

## 4. Приоритеты и конфликты

1. Один запуск не может одновременно быть `ENTERPRISE`, `DESIGNER`,
   `CREATEINFOBASE`, OLE-регистрацией и batch-операцией конфигуратора.
2. Режим, выбранный UI, задаёт контекст; противоречащий raw-режим является
   конфликтом.
3. Валидный `/AppArch` из дополнительных аргументов имеет приоритет над полем
   `AppArch`; невалидное значение должно быть диагностировано.
4. Typed `ClientConnectionSpeed`, `WA`, `WSA`, version и client type добавляются
   только при отсутствии соответствующего reserved parameter в raw-аргументах.
5. `/IBConnectionString` должен обрабатываться с учётом значимого порядка и не
   добавляться после параметров, меняющих его части.
6. `/@` является первой или единственной командой и не смешивается с
   автоматически созданным подключением.
7. `/Execute`, `/URL`, `/RunShortcut`, `/ClearCache`, `/RegServer`,
   `/UnregServer` и очистка saved auth требуют отдельного контекста и
   подтверждения.
8. `/P`, `/WSP`, `-pwd`, `PPasswd`, `AccessToken`, `UC`, `SPwd`, `DBPwd` и
   аналогичные значения можно хранить в `ibases.v8i`, показывать в UI и
   копировать явным действием пользователя. В открытом виде они не попадают в
   log и автоматически создаваемые diagnostics/report; перед запуском остаётся
   предупреждение.

## 5. Version profiles и обратная совместимость

Профиль версии должен разделять:

- возможности формата файла;
- возможности конкретного клиента;
- возможности режима запуска;
- наличие параметра в официальном справочнике;
- фактически подтверждённое поведение на тестовой установке.

Минимальная матрица проекта:

| Профиль | Источник | Назначение |
| --- | --- | --- |
| 8.3.20 | старый snapshot приложения 3 | Регрессия старых `ibases.v8i`. |
| 8.3.26 | контрольная ветка документации | Проверка базового CLI-контракта. |
| 8.5.1 | актуальные snapshots приложений 3 и 7 | Новые поля, web/mobile и batch-команды. |

Правила совместимости:

- чтение старого файла должно быть permissive, но с диагностикой;
- неизвестные поля и raw-аргументы не удаляются;
- добавление нового поля не меняет смысл старого поля;
- запись без изменения значения сохраняет исходное представление;
- нормализация выполняется только в typed-view или после явного действия;
- feature может быть `apply`, `preserve`, `external` или `unsupported`;
- `unsupported` означает явный отказ с объяснением, а не молчаливую потерю;
- обновление IBStart не должно задним числом менять policy старого профиля.

Разрядность:

    x86          -> только x86
    x86_64       -> только x64
    x86_prt      -> приоритет x86
    x86_64_prt   -> приоритет x64
    empty        -> явная политика IBStart

Для `Version` различать exact version, prefix version, установленную версию,
значение `DefaultVersion` и политику `/AppAutoCheckVersion`. IBStart не должен
обещать автоматическую установку дистрибутива, если такой workflow не реализован.

## 6. План реализации

### Этап 0 — contract и fixtures

- fixture с каждым полем из [ibases-format.md](ibases-format.md), unknown fields,
  duplicate keys, quoted semicolon и legacy URL;
- manifest заявленных версий и ручная matrix;
- диагностический отчёт `прочитан / сохранён / применён / отклонён` без секретов;
- запрет MHT и извлечённого HTML в fixtures.

### Этап 1 — lossless parser и diagnostics

- `ParseResult` с warnings/errors;
- duplicate sections/keys, whitespace, empty names, CR-only и invalid UTF-8;
- escaped/doubled quotes, semicolon, empty values, duplicate fragments, IPv6 и
  cluster lists;
- отсутствие потери данных в `QuoteValue` и golden tests.

### Этап 2 — typed `ibases.v8i`

- `UseProxy`, `PSrv`, `PPort`, `PUser`, `PPasswd`, `WSA`, `Https*`,
  `StartupErrorHelp*`, `ClientConnectionSpeed`;
- mobile/общие поля в `extra_fields` с видимым scope до отдельного workflow;
- проверка уникальности `ID`, порядка и parent path без изменения unknown/raw.

### Этап 3 — ParameterRegistry и LaunchPlan

- case-insensitive registry;
- attached, separate и nested options;
- conflict resolver, precedence и `LaunchPlan::Warnings`;
- redaction по metadata registry;
- redacted command preview и diagnostics; пользовательский просмотр и явное
  копирование исходного значения остаются отдельными разрешёнными действиями.

### Этап 4 — применение полей

- `/O` из `ClientConnectionSpeed`;
- `/WA` из `WA` и `/WSA` из `WSA`;
- `App`/`DefaultApp` с `/AppAutoCheckMode`;
- `Version`/`DefaultVersion` с `/AppAutoCheckVersion`;
- единый `AppArch` resolver;
- web extras, `/Https*`, `/IBName`, `/IBConnectionString`, `/URL`, `/@`.

### Этап 5 — отдельные операции

- batch Designer: dump/load/check/repair/repository;
- `CREATEINFOBASE`;
- client batch;
- web URL/navigation;
- OLE registration;
- agent mode;
- mobile metadata/export.

Каждая операция должна иметь отдельный builder, scope, confirmation policy,
exit-code handling, output-file policy и tests. Нельзя добавить все команды в
`AdditionalParameters` и считать это полной реализацией.

## 7. Definition of Done

Полная реализация готова только когда:

- каждый ключ `ibases.v8i` имеет `apply`, `preserve`, `external` или
  `unsupported` с объяснением;
- каждый параметр заявленного version profile имеет registry entry, scope,
  grammar, redaction policy и test;
- generated command не содержит противоречивых или случайно дублированных
  connection/mode arguments;
- unknown fields и unknown additional parameters не теряются;
- UI показывает, что будет передано клиенту и что не поддерживается;
- секреты не попадают в открытом виде в log и автоматически создаваемый report;
  fixtures используют синтетические или замаскированные значения, а ввод,
  отображение и явное копирование пользователем разрешены;
- version matrix подтверждена на целевых версиях;
- в release package нет MHT, HTML или скопированных фрагментов документации 1С.

## 8. Документы проекта

- [Формат `ibases.v8i`](ibases-format.md) — структура, поля, `Connect` и
  round-trip.
- [Параметры и команды запуска](launch-parameters.md) — CLI, batch, web,
  mobile, OLE, agent и connection strings.
- [Конфигурация](configuration.md) — пользовательский справочник текущего
  поведения.
- [Руководство пользователя](user-guide.md) — пользовательские сценарии.
- [Архитектура](architecture.md) — текущие границы модулей.
- [Лицензионная политика 1С:ИТС](1c-license-compliance.md) — допустимый состав
  репозитория и release-архива.
