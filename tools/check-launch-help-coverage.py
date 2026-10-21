"""Check source identifier coverage in embedded help; this does not verify prose semantics.

Authoring input is selected explicitly and never serialized. The regular Python
tests and native help tests remain usable without these authoring sources.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "src/core/launcher"
STRING = r'"(?:[^"\\]|\\.)*"'
spec = importlib.util.spec_from_file_location("grammar", Path(__file__).with_name("update-parameter-grammar.py"))
grammar = importlib.util.module_from_spec(spec)
spec.loader.exec_module(grammar)


def section(blocks, kind):
    title = {"connection": "Строка соединения с информационной базой",
             "web": "веб-клиента", "mobile": "мобильной версии"}[kind].lower()
    active, result = False, []
    for tag, text in blocks:
        if tag == "h2":
            if active:
                break
            active = title in text.lower()
        if active:
            result.append(text)
    return result


def section_identifiers(paragraphs, kind):
    if kind == "mobile":
        # The last item is often followed by ':', not a comma or whitespace.
        # Extract the entire comma-separated list rather than use that suffix
        # as an ad-hoc boundary for an individual expected name.
        return {key.lower() for p in paragraphs if p.startswith(("●", "•"))
                for key in re.findall(r"/[A-Za-z][A-Za-z0-9_]*\b", p)}
    result = set()
    for p in paragraphs:
        if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:[=<].*)?", p):
            result.add(re.split(r"[=<]", p)[0].lower())
        if re.fullmatch(r"Z\s*n", p, re.I):
            result.add("z")
    return result


def embedded_notes():
    pattern = r'\{L(' + STRING + r'), L(' + STRING + r'), L(' + STRING + r'), (0x[0-9a-f]+)u\},'
    return [(json.loads(c).lower(), json.loads(o), json.loads(t), int(mask, 16))
            for c, o, t, mask in re.findall(pattern, (CORE / "parameter_context_help.inc").read_text(encoding="utf8"))]


def missing_identifiers(identifiers, notes):
    mentioned = "\n".join(notes)
    return sorted(key for key in identifiers if not re.search(
        r"(?<!\w)" + re.escape(key) + r"(?=\s|[=—])", mentioned, re.I))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path, required=True)
    args = parser.parse_args()
    versions = re.findall(r'^    L"([\d.]+)",$', (CORE / "parameter_versions.inc").read_text(encoding="utf8"), re.M)
    profiles = {}
    for path in args.references.glob("*.mht*"):
        match = re.search(r"8\.(?:3|5)\.\d+", path.name)
        if not match:
            continue
        if match[0] in profiles:
            raise ValueError(f"Duplicate snapshot {match[0]}")
        blocks = grammar.versions_module.read_snapshot(path)
        if len(blocks) < 100 or match[0] not in blocks[0][1]:
            raise ValueError(f"Incomplete or mismatched snapshot: {path.name}")
        profiles[match[0]] = blocks
    if set(profiles) != set(versions):
        raise ValueError("Snapshot versions must match the embedded profiles")
    catalog = (CORE / "parameter_catalog.cpp").read_text(encoding="utf8").split("struct VersionPresence")[0]
    catalog += (CORE / "parameter_reference.inc").read_text(encoding="utf8")
    keys = {json.loads(k).lower() for k in re.findall(r'\{L(' + STRING + r'), L', catalog)}
    notes, errors, counts = embedded_notes(), [], {kind: 0 for kind in ("mobile", "web", "connection")}
    for i, version in enumerate(versions):
        blocks = profiles[version]
        for command in grammar.definitions(blocks):
            if command not in keys:
                errors.append(f"{version}: missing catalogue command {command}")
        for kind, command in (("mobile", "mobile: launch"), ("web", "web: query"),
                              ("connection", "createinfobase: connection")):
            identifiers = section_identifiers(section(blocks, kind), kind)
            counts[kind] += len(identifiers)
            active = [t for c, o, t, mask in notes if c == command and not o and mask & (1 << i)]
            for name in missing_identifiers(identifiers, active):
                errors.append(f"{version} {kind}: missing explanation for {name}")
    if errors:
        raise ValueError("\n".join(errors))
    print(f"{len(profiles)} profiles, {len(keys)} catalogue entries; section identifier coverage: " +
          ", ".join(f"{kind}={count}" for kind, count in counts.items()))
    print("Identifier coverage passed; semantic review is recorded separately.")


if __name__ == "__main__":
    main()
