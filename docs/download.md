# Скачать IBStart

## Последняя стабильная версия

[![Последний релиз](https://img.shields.io/github/v/release/ant-m13/ibstart?display_name=tag&label=последняя%20версия)](https://github.com/ant-m13/ibstart/releases/latest)

[Открыть последний GitHub Release](https://github.com/ant-m13/ibstart/releases/latest){ .md-button .md-button--primary }

В каждом релизе доступны:

- `IBStart.exe` — отдельный исполняемый файл;
- `IBStart-<версия>-portable-x64.zip` — готовая portable-версия;
- `IBStart.version` — номер версии;
- `checksums.txt` — SHA-256 контрольные суммы.

Руководство и снимки окон описывают **0.8.0**. Версия последнего опубликованного
релиза указана на значке выше и на странице GitHub Release; она может отставать
от документации ветки разработки. Конструктор параметров запуска появился в 0.8.0.

## Обычный режим

Скачайте `IBStart.exe` и запустите его как обычный пользователь. Настройки и данные будут храниться в `%LOCALAPPDATA%\IBStart\`.

## Portable mode

Скачайте portable-архив, распакуйте его в доступную для записи папку и запустите `IBStart.exe`.

Архив содержит пустой маркер `IBStart.portable`; настройки IBStart находятся в `data\`
рядом с EXE. Файл списка `ibases.v8i`, данные баз и кэш платформы 1С автоматически
в эту папку не переносятся.

## Предыдущие версии

[Все релизы IBStart](https://github.com/ant-m13/ibstart/releases)
