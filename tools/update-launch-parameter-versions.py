"""Maintain CLI identifiers and versioned presence masks.

No HTML, prose, images or source examples are copied into tracked output.
Python is needed only to refresh the checked-in compatibility data.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from email import policy
from email.parser import BytesParser
from html.parser import HTMLParser
from pathlib import Path
import re
import sys


class Blocks(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.items: list[tuple[str, str]] = []
        self.tag = ""
        self.text: list[str] = []

    def handle_starttag(self, tag, attrs):
        if tag in ("title", "p", "h1", "h2", "h3", "h4", "h5", "h6"):
            self.tag, self.text = tag, []

    def handle_data(self, data):
        if self.tag:
            self.text.append(data)

    def handle_endtag(self, tag):
        if tag == self.tag:
            value = " ".join("".join(self.text).split())
            if value:
                self.items.append((tag, value))
            self.tag, self.text = "", []


def read_snapshot(path):
    message = BytesParser(policy=policy.default).parsebytes(path.read_bytes())
    candidates = []
    for part in message.walk():
        if part.get_content_type() != "text/html":
            continue
        raw = part.get_payload(decode=True)
        match = re.search(rb"charset\s*=\s*[\"']?([\w-]+)", raw[:4000], re.I)
        encoding = match[1].decode("ascii") if match else part.get_content_charset() or "utf-8"
        parser = Blocks()
        parser.feed(raw.decode(encoding))
        candidates.append(parser.items)
    if not candidates:
        raise ValueError(f"No HTML document in {path.name}")
    return max(candidates, key=len)


def command_paragraph(text):
    # Historical pages occasionally omit the slash or add a full stop to a
    # standalone command heading. Normalize only these definition spellings.
    text = text.lstrip("●• ")
    if text == "DisableHomePageForms":
        return "/DisableHomePageForms"
    if re.fullmatch(r"/[A-Za-z][A-Za-z0-9_]*\.", text):
        return text[:-1]
    return text


def commands(blocks):
    # Command paragraphs are distinct from descriptive mentions and web query
    # parameters. This deliberately does not scan arbitrary prose for /keys.
    found = {}
    section = ""
    for i, (tag, text) in enumerate(blocks):
        if tag.startswith("h"):
            section = text
        command_text = command_paragraph(text)
        match = re.match(r"^(/(?:[A-Za-z][A-Za-z0-9_]*|@))(?=[\s<+\-\[\"=]|$)", command_text)
        if match or command_text in ("UsePrivilegedMode", "DisplayPerformance"):
            key = match[1] if match else command_text
            if key.lower() in ("/regserver", "/unregserver") or not re.search(
                    r"строк[аи] соединения|веб-клиента|мобильной версии", section, re.I):
                found.setdefault(key.lower(), (key, [command_text]))
                # Include adjacent list items for enum identifiers, stopping
                # at the next command or heading. Prose is never serialized.
                for next_tag, next_text in blocks[i + 1:]:
                    if next_tag.startswith("h") or next_text.lstrip("●• ").startswith("/"):
                        break
                    found[key.lower()][1].append(next_text)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path)
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--docs-only", action="store_true", help="Render the matrix from checked-in identifiers, without snapshots")
    parser.add_argument("--check-docs", action="store_true", help="Check the rendered matrix without reading snapshots")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.docs_only or args.check_docs:
        render_docs(root, check=args.check_docs)
        return
    if args.references is None:
        parser.error("--references is required for import")
    profiles = {}
    for path in args.references.glob("*.mht*"):
        match = re.search(r"8\.(?:3|5)\.\d+", path.name)
        if not match:
            continue
        version = match[0]
        if version in profiles:
            raise ValueError(f"Duplicate snapshot for {version}")
        blocks = read_snapshot(path)
        if len(blocks) < 100:
            raise ValueError(f"Incomplete snapshot for {version}: only {len(blocks)} text blocks")
        title = blocks[0][1]
        if version not in title:
            raise ValueError(f"Filename and document title disagree: {path.name}")
        profiles[version] = commands(blocks)
    versions = sorted(profiles, key=lambda v: tuple(map(int, v.split("."))))
    if not versions or len(versions) > 32:
        raise ValueError("Expected between 1 and 32 verified version profiles")
    if args.inspect:
        for version in versions:
            print(version, len(profiles[version]), " ".join(item[0] for item in profiles[version].values()))
        return

    catalog = (root / "src/core/launcher/parameter_catalog.cpp").read_text(encoding="utf-8")
    catalog += (root / "src/core/launcher/parameter_reference.inc").read_text(encoding="utf-8")
    keys = re.findall(r'\{L"(/[^" ]+|UsePrivilegedMode|DisplayPerformance)", L"', catalog)
    # Choices are intentionally an allowlist of compatibility identifiers from
    # our own catalog. The extractor never emits source prose or examples.
    choice_sets = {
        "/setpredefineddataupdate": ["-Auto", "-UpdateAutomatically", "-DoNotUpdateAutomatically"],
        "/o": ["Normal", "Low"],
        "/tcomp": ["-None", "-Deflate", "-SDC", "-lz4", "-zstd"],
        "/slev": ["0", "1", "2"],
        "/apparch": ["x86", "x86_prt", "x86_64", "x86_64_prt"],
        "/mainwindowmode": ["-Normal", "-Workplace", "-EmbeddedWorkplace", "-FullscreenWorkplace", "-Kiosk"],
        "/theme": ["-Dark", "-Light"],
        "/wa": ["+", "-"], "/wsa": ["+", "-"], "/oida": ["+", "-"],
        "/usehwlicenses": ["+", "-"],
        "/appautocheckversion": ["+", "-"],
        "/appautoinstalllastversion": ["+", "-"],
        "/disablelocalspeechtotext": ["+", "-"],
        "/allowexecutescheduledjobs": ["-Off", "-Force"],
    }
    lines = ["// Derived compatibility identifiers only. Refresh with tools/update-launch-parameter-versions.py.",
             "// Documentation presence is not runtime verification.",
             "const std::wstring_view verified_versions[] = {"]
    lines.extend(f'    L"{v}",' for v in versions)
    lines.extend(["};", "const VersionPresence version_presence[] = {"])
    for key in sorted(set(keys), key=str.lower):
        mask = sum(1 << i for i, v in enumerate(versions) if key.lower() in profiles[v])
        lines.append(f'    {{L"{key}", 0x{mask:08x}u}},')
    lines.extend(["};", "const VersionChoice version_choices[] = {"])
    for key, choices in choice_sets.items():
        for choice in choices:
            mask = 0
            for i, version in enumerate(versions):
                entry = profiles[version].get(key)
                if not entry:
                    continue
                text = " ".join(entry[1])
                # Signed switches have their +/- in the command syntax rather
                # than in the following enumeration.
                if choice in ("+", "-"):
                    present = key not in ("/disablelocalspeechtotext", "/appautocheckversion") or "<" in entry[1][0]
                else:
                    present = re.search(r"(?<![\w])" + re.escape(choice) + r"(?![\w])", text, re.I) is not None
                if present:
                    mask |= 1 << i
            lines.append(f'    {{L"{key}", L"{choice}", 0x{mask:08x}u}},')
    lines.extend(["};", ""])
    output = root / "src/core/launcher/parameter_versions.inc"
    output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    render_docs(root)
    print(f"Updated {len(versions)} version profiles, {len(set(keys))} catalog commands.")


def render_docs(root, check=False):
    data = (root / "src/core/launcher/parameter_versions.inc").read_text(encoding="utf-8")
    versions = re.findall(r'^    L"([\d.]+)",$', data, re.M)
    rules = re.findall(r'\{L"([^\"]+)", (0x[\da-f]+)u\}', data)
    choices = re.findall(r'\{L"([^\"]+)", L"([^\"]+)", (0x[\da-f]+)u\}', data)
    manifest_path = root / "docs/reference-data/1c-reference.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"sources": []}
    source_status = {s["version"]: s["preview"] for s in manifest["sources"] if s["kind"] == "launch-parameters"}

    def ranges(mask):
        groups = []
        start = end = None
        for i, version in enumerate(versions):
            present = int(mask, 16) & (1 << i)
            if present:
                if start is None:
                    start = end = version
                elif version.split('.')[:2] == end.split('.')[:2] and int(version.split('.')[2]) == int(end.split('.')[2]) + 1:
                    end = version
                else:
                    groups.append(start if start == end else f"{start}–{end}")
                    start = end = version
            elif start is not None:
                groups.append(start if start == end else f"{start}–{end}")
                start = end = None
        if start is not None:
            groups.append(start if start == end else f"{start}–{end}")
        return ", ".join(groups) or "Не найдено в разделах документации"

    lines = ["# Матрица версий параметров запуска", "",
        "Таблицы сформированы из каталога совместимости IBStart. Дата сверки официальной документации и различия между версиями приведены в [общем журнале](1c-reference-versions.md).", "",
        f"SHA-256 данных совместимости: `{hashlib.sha256(data.encode('utf-8')).hexdigest()}`.", "",
        "Основные правила и статус исполнения описаны в [параметрах запуска](launch-parameters.md).", "",
        "## Значение статусов", "",
        "Наличие команды в таблице означает, что её определение найдено в разделе документации ИТС соответствующей версии. Это не проверка исполнения на установленной платформе. Отсутствие определения означает отсутствие в проверенном справочнике, а не доказательство невозможности исполнения. Промежуточные и неизвестные версии не наследуют профиль соседней версии.", "",
        "Конструктор редактирует обычные параметры и вложенные опции команд с извлечённой грамматикой выбранного профиля. Web/mobile и отдельные режимы остаются справочными. Наличие определения в матрице не подтверждает выполнение операции платформой.", "",
        "## Проверенные профили", "", "| Версия | Официальный раздел |", "| --- | --- |"]
    for v in versions:
        label = v + ("*" if source_status.get(v, False) else "")
        lines.append(f"| {label} | [ИТС](https://its.1c.ru/db/v{v.replace('.', '')}doc/bookmark/adm/TI000000493) |")
    if any(source_status.get(v, False) for v in versions):
        lines += ["", "\\* — тестовая версия по заголовку документации."]
    lines += ["", "Профили 8.5.2 и 8.5.3 не предусмотрены. Полный номер сборки сопоставляется с точным семейством (например, 8.3.26.1540 → 8.3.26). Пустая версия, префикс 8.3 и неизвестное семейство имеют статус «не сверено».", "",
        "## Наличие команд в документации", "", "| Команда | Проверенные версии с определением |", "| --- | --- |"]
    for key, mask in rules:
        lines.append(f"| `{key}` | {ranges(mask)} |")
    lines += ["", "## Варианты значений структурированных команд", "",
        "Ниже приведены варианты основных команд; вложенные опции, их значения и требования хранятся отдельно в `parameter_grammar.inc`. Написание сохраняется, включая минус перед `-Dark`, `-Light` и вариантами `/MainWindowMode`.", "",
        "| Команда | Значение | Проверенные версии с вариантом |", "| --- | --- | --- |"]
    for key, value, mask in choices:
        lines.append(f"| `{key}` | `{value}` | {ranges(mask)} |")
    lines += ["", "## Обновление", "",
        "Общий процесс для обоих приложений, контроль новых страниц и границы автоматической сверки описаны в [обновлении справочников](maintaining-1c-reference.md). Обновление матриц документации само по себе не расширяет поддержку приложения.", "",
        "1. Проверить официальный раздел ИТС для нужной версии по ссылке в реестре.",
        "2. Обновить идентификаторы и варианты значений поддержанных команд; проверить соответствие выбранной версии.",
        "3. Просмотреть изменения `parameter_versions.inc`, особенно удаления, пробелы в старых справочниках и изменения вложенных опций. Автоматическое извлечение требует инженерной проверки.",
        "4. Добавить собственные описания и редакторы новых команд; при изменении синтаксиса обновить основной справочник и граничные тесты.",
        "5. Выполнить сборку, CTest и `python tools/update-launch-parameter-versions.py --check-docs`. Повторная генерация должна давать тот же результат.",
        "", "Таблицу можно восстановить без исходных страниц командой `python tools/update-launch-parameter-versions.py --docs-only`. Сборка приложения использует собственные данные каталога.", ""]
    output = root / "docs/launch-parameter-versions.md"
    rendered = "\n".join(lines)
    if check:
        if output.read_text(encoding="utf-8") != rendered:
            raise ValueError("Version documentation is out of date; run --docs-only")
    else:
        output.write_text(rendered, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
