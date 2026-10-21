"""Maintain versioned identifiers and official documentation links.

Default: refresh JSON and Markdown. --docs-only/--check use project data.
--references selects authoring input explicitly.
"""
from __future__ import annotations

import argparse
from datetime import date
from email import policy
from email.parser import BytesParser
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "docs/reference-data/1c-reference.json"
SPEC = importlib.util.spec_from_file_location("launch_versions", Path(__file__).with_name("update-launch-parameter-versions.py"))
LAUNCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCH)
VERSION = re.compile(r"(?<![\d.])8\.(?:3|5)\.\d+(?!\d)")
FIELD = re.compile(r"^([A-Za-z][A-Za-z0-9]*)(?: (?:(?:необязательный|обязательный)(?: для iOS)?|только мобильный клиент))?$")


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def version_key(value):
    return tuple(map(int, value.split(".")))


def v8i_section(blocks):
    starts = [i for i, (tag, text) in enumerate(blocks) if tag == "h2" and re.search(r"\*\.v8i\b", text)]
    if len(starts) != 1:
        raise ValueError("Expected exactly one *.v8i section")
    start = starts[0]
    end = next((i for i in range(start + 1, len(blocks)) if blocks[i][0] == "h2"), len(blocks))
    return blocks[start + 1:end]


def fields(blocks):
    result, active = {}, None
    for tag, text in v8i_section(blocks):
        if text.startswith(("Смотри также:", "Следующий набор", "Следующий набора")):
            active = None
            continue
        match = FIELD.fullmatch(text.replace("\u00ad", " ")) if tag == "p" else None
        if match:
            active = match[1]
            if active in result:
                raise ValueError(f"Duplicate field definition: {active}")
            result[active] = [text]
        elif active:
            result[active].append(text)
    if not {"ID", "Connect", "Version", "AdditionalParameters"} <= result.keys():
        raise ValueError("Incomplete *.v8i field definitions")
    return result


def enum_values(parts):
    # Only leading literal tokens from enumerations; prose is not serialized.
    values = set()
    for text in parts[1:]:
        match = re.match(r"^[●•]\s+([A-Za-z][A-Za-z0-9_]*|\d+)\s+[–—‑-]\s", text)
        if match:
            values.add(match[1])
    return sorted(values)


def collect(references, checked_on):
    sources = []
    for kind in ("launch-parameters", "service-files"):
        seen = set()
        paths = sorted(p for p in (references / kind).glob("*") if p.suffix.lower() in (".mht", ".mhtml"))
        if not paths:
            raise ValueError(f"No snapshots in {references / kind}")
        for path in paths:
            matches = VERSION.findall(path.name)
            if len(matches) != 1:
                raise ValueError(f"Expected one version in filename: {path.name}")
            version = matches[0]
            if version in seen:
                raise ValueError(f"Duplicate {kind} snapshot: {version}")
            seen.add(version)
            blocks = LAUNCH.read_snapshot(path)
            titles = [text for tag, text in blocks if tag == "title"]
            if len(titles) != 1 or version not in VERSION.findall(titles[0]):
                raise ValueError(f"Filename/title version mismatch: {path.name}")
            expected = "Приложение 7." if kind == "launch-parameters" else "Приложение 3."
            if expected not in titles[0] or len(blocks) < 100:
                raise ValueError(f"Incomplete or wrong appendix: {path.name}")
            if kind == "service-files":
                definitions = fields(blocks)
            else:
                definitions = {item[0]: item[1] for item in LAUNCH.commands(blocks).values()}
                if not {"/F", "/S", "/N", "/P"} <= definitions.keys():
                    raise ValueError(f"Incomplete CLI definitions: {path.name}")
            entries = {}
            for name, parts in sorted(definitions.items()):
                entries[name] = {"sha256": digest("\n".join(parts))}
                if kind == "service-files":
                    entries[name]["values"] = enum_values(parts)
            message = BytesParser(policy=policy.default).parsebytes(path.read_bytes())
            source_url = str(message.get("Snapshot-Content-Location", ""))
            url = urlsplit(source_url)
            if url.scheme != "https" or url.hostname != "its.1c.ru" or f"v{version.replace('.', '')}doc" not in url.path:
                raise ValueError(f"Missing or mismatched ITS source URL: {path.name}")
            sources.append({
                "kind": kind, "version": version,
                "source_url": source_url,
                "preview": "Тестовая версия" in titles[0],
                "entries": entries,
            })
    return {"schema": 1, "checked_on": checked_on, "sources": sorted(sources, key=lambda s: (s["kind"], version_key(s["version"])))}


def ranges(versions):
    groups = []
    for version in sorted(versions, key=version_key):
        if groups and version_key(version)[:2] == version_key(groups[-1][-1])[:2] and version_key(version)[2] == version_key(groups[-1][-1])[2] + 1:
            groups[-1].append(version)
        else:
            groups.append([version])
    return ", ".join(g[0] if len(g) == 1 else f"{g[0]}–{g[-1]}" for g in groups)


def identifiers(values):
    return ", ".join(f"`{value}`" for value in sorted(values, key=str.lower)) or "—"


def render(data):
    sources = data["sources"]
    service = [s for s in sources if s["kind"] == "service-files"]
    header = [f"Дата сверки документации: {data['checked_on']}.", "",
              "Сформировано `tools/update-1c-reference.py` из [реестра источников](reference-data/1c-reference.json). "
              "Порядок повторной сверки: [обновление справочников](maintaining-1c-reference.md).", ""]
    matrix = ["# Версии полей ibases.v8i", "", *header,
              "Таблица показывает определения полей именно в разделе `*.v8i` приложения 3. "
              "Упоминания в соседнем `1cestart.cfg` не считаются полями v8i. "
              "Отсутствие определения не доказывает отсутствие поддержки платформой. "
              "Статусы реализации IBStart приведены в [описании формата](ibases-format.md).", "",
              "## Наличие полей", "", "| Поле | Проверенные версии с определением |", "| --- | --- |"]
    for name in sorted(set().union(*(s["entries"] for s in service)), key=str.lower):
        matrix.append(f"| `{name}` | {ranges([s['version'] for s in service if name in s['entries']])} |")
    matrix += ["", "## Перечисленные значения", "",
               "Только явно перечисленные литералы. Пустой набор не означает свободный ввод; "
               "значения, описанные ссылкой на CLI (например, `AppArch`), здесь не выводятся. "
               "Условия, значения по умолчанию и зависимости проверяются вручную по исходной странице.", "",
               "| Поле | Значение | Проверенные версии |", "| --- | --- | --- |"]
    for name in sorted(set().union(*(s["entries"] for s in service)), key=str.lower):
        values = set().union(*(s["entries"].get(name, {}).get("values", []) for s in service))
        for value in sorted(values):
            present = [s["version"] for s in service if value in s["entries"].get(name, {}).get("values", [])]
            matrix.append(f"| `{name}` | `{value}` | {ranges(present)} |")
    report = ["# Сверка версий документации 1С", "", *header,
              "Наличие в документации, реализация IBStart и исполнение установленной платформой — "
              "отдельные факты. Диапазон начинается с первого проверенного определения, а не с доказанной "
              "версии появления функции. Сравниваются соседние **проверенные** версии; пропуски не заполняются.", "",
              "## Источники", "",
              "Статус указан по документации на дату сверки. «Без отметки тестовой» не означает "
              "самую новую стабильную версию на текущую дату. Официальные источники указаны ссылками.", "",
              "| Версия | Приложение 3 | Приложение 7 | Статус версии |", "| --- | --- | --- | --- |"]
    for version in sorted({s["version"] for s in sources}, key=version_key):
        row = []
        selected = [s for s in sources if s["version"] == version]
        for kind in ("service-files", "launch-parameters"):
            source = next((s for s in selected if s["kind"] == kind), None)
            if source:
                row.append(f"[ИТС]({source['source_url']}) ({len(source['entries'])})")
            else:
                row.append("Нет данных")
        status = "Тестовая версия" if any(s["preview"] for s in selected) else "Без отметки тестовой"
        report.append(f"| {version} | {' | '.join(row)} | {status} |")
    for kind, title in (("service-files", "Поля ibases.v8i"), ("launch-parameters", "Команды CLI")):
        report += ["", f"## {title}: различия между версиями", "",
                   "«Изменён текст» — изменение нормализованных абзацев определения, включая "
                   "пунктуацию и редакторские правки. Это список для ручной проверки, а не подтверждение "
                   "изменённой семантики или полной грамматики. HTML, таблицы и примеры кода не сравниваются.", "",
                   "| Версия ← предыдущая версия | Добавлены определения | Больше не найдены | Изменён текст общих определений |",
                   "| --- | --- | --- | --- |"]
        previous = None
        for source in [s for s in sources if s["kind"] == kind]:
            if previous:
                before, after = previous["entries"], source["entries"]
                changed = {k for k in before.keys() & after.keys() if before[k]["sha256"] != after[k]["sha256"]}
                report.append(f"| {source['version']} ← {previous['version']} | {identifiers(after.keys() - before.keys())} | {identifiers(before.keys() - after.keys())} | {identifiers(changed)} |")
            previous = source
    report += ["", "## Границы сверки", "",
               "[Матрица полей и значений](ibases-format-versions.md) охватывает определения v8i. "
               "[Матрица конструктора](launch-parameter-versions.md) ограничена командами, внесёнными в каталог приложения. "
               "Этот отчёт извлекает также определения CLI вне каталога: новая строка здесь требует решения "
               "разработчика о реализации. Автоматическая сверка не обновляет код приложения.", ""]
    return {"docs/ibases-format-versions.md": "\n".join(matrix) + "\n", "docs/1c-reference-versions.md": "\n".join(report)}


def write_or_check(path, content, check):
    if check:
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            raise ValueError(f"Stale generated file: {path.relative_to(ROOT)}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--docs-only", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--check-snapshots", action="store_true")
    parser.add_argument("--date", type=date.fromisoformat, help="Explicit audit date, YYYY-MM-DD")
    args = parser.parse_args()
    existing = json.loads(DATA.read_text(encoding="utf-8")) if DATA.exists() else None
    if args.check or args.docs_only:
        if existing is None:
            raise ValueError("No reference manifest")
        data = existing
    else:
        checked_on = str(args.date) if args.date else existing["checked_on"] if existing else None
        if checked_on is None:
            raise ValueError("First import requires --date YYYY-MM-DD")
        if args.references is None:
            parser.error("--references is required for import")
        data = collect(args.references, checked_on)
        write_or_check(DATA, json.dumps(data, ensure_ascii=False, indent=2) + "\n", args.check_snapshots)
    for path, content in render(data).items():
        write_or_check(ROOT / path, content, args.check or args.check_snapshots)
    print(f"Verified {len(data['sources'])} sources; documentation {'checked' if args.check or args.check_snapshots else 'updated'}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
