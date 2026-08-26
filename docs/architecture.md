# Архитектура IBStart

IBStart придерживается правила направленных зависимостей: ядро не зависит от окна Win32. UI получает данные от ядра и отображает ошибки, но не разбирает `ibases.v8i`, не формирует командную строку и не пишет файлы напрямую.

```text
src/
├── app (точка входа и повторная активация экземпляра)
├── core
│   ├── domain
│   ├── connection
│   ├── catalog ── v8i ── domain
│   ├── launcher ── platform ── domain
│   │   ├── command_builder (параметры, выбор платформы и аргументы)
│   │   └── process_launcher (единственный CreateProcessW)
│   ├── update
│   │   ├── github_release_client (WinHTTP-транспорт)
│   │   └── update_service (SemVer и политика обновлений)
│   └── storage / logging / cache / scanner / shell
└── ui (тонкий Win32-слой)
    ├── main_window (координация окна и маршрутизация команд)
    ├── tree_view_controller + tree_presentation
    ├── details_view_controller
    ├── database_editor_dialog + advanced_database_options_dialog
    ├── tag_manager + tag_*_dialog
    ├── update_check_operation + cache_clear_operation
    └── dialog_support / folder_picker / input_box / owner_draw_menu
```

`domain` определяет типы базы, платформы и команды запуска. `v8i` хранит секции и поля в исходном порядке: поле, которое ядро не знает, не преобразуется и сериализуется на прежнем месте среди полей. `catalog` строит дерево по `Folder`, понимает нативные абсолютные пути 1С (`/`, `/Группа/Подгруппа`) и хранит ручной порядок в `OrderInList`.

`V8iFileStore` запоминает fingerprint прочитанного файла. До сохранения он сравнивает размер и время записи, делает backup и заменяет цель временным файлом через `MoveFileExW`. Поэтому ошибка записи не портит исходный список, а параллельное изменение требует перезагрузки.

`launcher/command_builder` содержит чистые функции выбора платформы, разбора дополнительных параметров и построения аргументов. Формирование полной строки для `LaunchCommand` также остаётся частью построителя. Единственный Win32-вызов `CreateProcessW` изолирован в `launcher/process_launcher.cpp`; он принимает уже подготовленную команду и не знает о выборе платформы. Тесты покрывают чистую часть без создания окон.

`storage` использует `%LOCALAPPDATA%\IBStart` или `data` возле EXE в portable mode. Активный `ibases.v8i` не дублируется. `logging` маскирует секреты до записи. `cache` намеренно принимает только список разрешённых cache-подкаталогов (`%APPDATA%\1C\1Cv8`, `%LOCALAPPDATA%\1C\1Cv8`, `%LOCALAPPDATA%\IBStart\cache`), никогда не путь из `Connect`, и отказывается очищать подкаталоги лицензий. Полные схемы и правила совместимости файлов описаны в [configuration.md](configuration.md).

`update/github_release_client` по явной команде пользователя получает небольшой release asset `IBStart.version` через стабильную HTTPS-ссылку GitHub `releases/latest/download`, без обращения к REST API. Он отвечает только за WinHTTP, тайм-ауты, HTTP-статусы, ограничение размера ответа и кооперативную отмену. `update/update_service` читает только SemVer-версию, строит ссылку на страницу релиза и сравнивает версии; он не скачивает исполняемые файлы, не меняет EXE и не сохраняет историю проверок. Сетевой запрос выполняется вне потока окна; UI получает лишь готовый результат.

`ui/main_window` координирует жизненный цикл окна, команды и обновление представлений. Заполнение дерева и операции с native tree view находятся в `TreeViewController`, подготовка фильтров и отрисовка — в `tree_presentation`, сведения о выбранной базе — в `DetailsViewController`. Редактор базы и окно дополнительных параметров являются отдельными диалоговыми модулями; `TagManager` координирует формы тегов и сохранение метаданных, а `update_check_operation` и `cache_clear_operation` владеют фоновыми потоками. Низкоуровневые общие операции Win32-диалогов находятся в `dialog_support`.

Версия продукта задаётся только в `cmake/IBStartVersion.cmake`. На этапе конфигурации CMake из неё генерируются C++ header, Windows VERSIONINFO, manifest, `ibstart-version.txt` и пустой маркер `IBStart.portable` для portable-архива. Благодаря этому окно «О программе», свойства PE-файла и release workflow используют одно значение.

## Тестируемость

`tests/unit/test_main.cpp` — самостоятельный CTest-исполняемый файл без фреймворка. Он использует fixtures, не открывает окна и проверяет round trip, сохранение/backup/conflict, команды запуска, выбор x64, маскирование `/P`, portable mode, SemVer-сравнение и разбор release asset `IBStart.version`.
