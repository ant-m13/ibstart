"""Extract versioned CLI syntax/option identifiers only; never serialize ITS prose."""
import argparse
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location("versions", Path(__file__).with_name("update-launch-parameter-versions.py"))
versions_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(versions_module)
COMMAND = re.compile(r'^(/(?:[A-Za-z][A-Za-z0-9_]*|@))(?=[\s<+\-\["=]|$)')
OPTION = re.compile(r'(?<![\w])-[A-Za-z][A-Za-z0-9_]*')


def definitions(blocks):
    result, section, active = {}, "", None
    for tag, text in blocks:
        if tag.startswith("h"):
            section, active = text, None
            continue
        value = versions_module.command_paragraph(text)
        match = COMMAND.match(value)
        if match:
            if re.search(r"строк[аи] соединения|веб-клиента|мобильной версии", section, re.I) and match[1].lower() not in ("/regserver", "/unregserver"):
                active = None
                continue
            active = match[1].lower()
            # Non-bulleted definitions supersede introductory mentions.
            if active not in result or not text.startswith(("●", "•")):
                result[active] = [value]
        elif value in ("UsePrivilegedMode", "DisplayPerformance"):
            if re.search(r"строк[аи] соединения|веб-клиента|мобильной версии", section, re.I):
                active = None
                continue
            active = value.lower()
            result[active] = [value]
        elif active:
            result[active].append(text)
    return result


def syntax_only(text):
    # Inline definitions use a dash followed by prose; that prose is not syntax.
    syntax = re.split(r"\s+[‑–—]\s+", text, maxsplit=1)[0].strip()
    # Creation options are described inline; optional operands/defaults are
    # specified in their adjacent prose rather than the printed heading.
    if syntax.lower() in ("/addtolist", "/addinlist"):
        return syntax + " [<имя базы>]"
    if syntax.lower() == "/usetemplate":
        return syntax + " [<имя файла шаблона>]"
    return syntax


def normalized_syntax(text):
    # Older pages use typographic dashes for some CLI switches.
    return re.sub(r"(?<!\w)[‑–—]\s*(?=[A-Za-z])", "-", syntax_only(text))


def option_paragraph(text, header):
    text = re.sub(r"^([●•]\s*)[‑–—]\s*(?=[A-Za-z])", r"\1-", text)
    # One historical bullet misspells a switch which is correctly named in
    # that same command header. It must not become a second CLI switch.
    if "-FirstVersion" in header:
        text = text.replace("-FitstVersion", "-FirstVersion")
    return text


def options(parts):
    header = normalized_syntax(parts[0])
    parts = [parts[0], *(option_paragraph(text, header) for text in parts[1:])]
    names = list(dict.fromkeys(m[0] for m in OPTION.finditer(header)))
    for paragraph in parts[1:]:
        match = re.match(r"^[●•]\s*(-[A-Za-z][A-Za-z0-9_]*)\b", paragraph)
        if match and match[1] not in names and match[1] not in primary_values(parts):
            names.append(match[1])
    result = []
    for name in names:
        match = re.search(re.escape(name) + r"(?![\w])", header)
        tail = header[match.end():] if match else ""
        if not match:
            paragraph = next((p.lstrip("●• ") for p in parts[1:] if p.lstrip("●• ").startswith(name)), "")
            tail = syntax_only(paragraph)[len(name):]
        value = re.match(r'(?P<gap>\s*[:=]?\s*)[«"“]?(?P<optional>\[)?(?P<value><[^>]+>|MDtype\[[^\]]*\]|hhh:mm|Plain\|Hierarchical|[A-Za-z]+\|[A-Za-z]+)', tail)
        if not value:
            value = re.match(r'(?P<gap>\s*[:=]?\s*)(?P<optional>\[)?(?P<value>«[^»]+»|“[^”]+”|"[^"]+")', tail)
        # -name is an optional spelling before the required label operand.
        if name.lower() == "-name" and not value:
            value = re.match(r'\]\s*(?P<gap>)(?P<optional>)(?P<value><[^>]+>)', tail)
        if value:
            gap = value["gap"]
            style = "equals" if "=" in gap else "colon" if ":" in gap else "space" if gap or name.lower() == "-name" else "attached"
            label = value["value"]
            optional = bool(value["optional"])
        else:
            style, label, optional = "flag", "", False
        if name.lower() == "-templatessourcepath":
            style, label = "space", "<путь>"
        if name.lower() == "-dynamic":
            optional = True
        result.append((name, name + (" <значение>" if label else ""), style, optional))
    return result


def grammar_rules(parts):
    """Compile bracket/alternative structure into presence requirements and conflicts.

    Operands and their internal brackets/pipes are not option groups. A bracketed
    sequence becomes active when any of its switches is present. Repeated groups
    may use several alternatives, so they do not produce mutual exclusions.
    """
    header = normalized_syntax(parts[0])
    header = re.sub(r"<[^>]*>|MDtype(?:\[,MDtype\])?|hhh:mm|Plain\|Hierarchical", "", header)
    tokens = re.findall(r"-[A-Za-z][A-Za-z0-9_]*|\[|\]|\||\+", header)
    position = 0

    def sequence():
        nonlocal position
        branches, children = [], []
        while position < len(tokens) and tokens[position] != "]":
            token = tokens[position]
            position += 1
            if token == "[":
                child = sequence()
                if position >= len(tokens) or tokens[position] != "]":
                    raise ValueError(f"Unbalanced syntax: {parts[0]}")
                position += 1
                repeated = position < len(tokens) and tokens[position] == "+"
                if repeated:
                    position += 1
                children.append(("repeat" if repeated else "optional", child))
            elif token == "|":
                branches.append(("sequence", children))
                children = []
            elif token.startswith("-"):
                children.append(("option", token))
        branches.append(("sequence", children))
        return branches[0] if len(branches) == 1 else ("choice", branches)

    tree = sequence()
    if position != len(tokens):
        raise ValueError(f"Unbalanced syntax: {parts[0]}")

    def names(node):
        kind, children = node
        if kind == "option":
            return {children}
        if kind in ("optional", "repeat"):
            return names(children)
        return set().union(*(names(child) for child in children))

    def mandatory(node):
        kind, children = node
        if kind == "option":
            return [{children}]
        if kind == "optional":
            return []
        if kind == "repeat":
            return mandatory(children)
        if kind == "sequence":
            return [clause for child in children for clause in mandatory(child)]
        # OR of branch CNFs: distribute clauses. A nullable branch imposes no
        # presence requirement (e.g. TestOnly versus optional repair switches).
        clauses = [set()]
        for child in children:
            required = mandatory(child)
            if not required:
                return []
            clauses = [left | right for left in clauses for right in required]
        return clauses

    requirements, conflicts = set(), set()

    def add_requirements(node, triggers):
        for clause in mandatory(node):
            if triggers and triggers <= clause:
                continue  # Presence of a switch already satisfies this clause.
            requirements.add((tuple(sorted(triggers)), tuple(sorted(clause))))

    def visit(node, repeated=False):
        kind, children = node
        if kind == "option":
            return
        if kind in ("optional", "repeat"):
            add_requirements(children, names(children))
            visit(children, repeated or kind == "repeat")
            return
        if kind == "choice" and not repeated:
            for i, left in enumerate(children):
                for right in children[i + 1:]:
                    for first in names(left):
                        for second in names(right):
                            if first != second:
                                conflicts.add(tuple(sorted((first, second))))
        for child in children:
            visit(child, repeated)

    add_requirements(tree, set())
    visit(tree)
    # This header omits brackets, while the option descriptions explicitly
    # allow full loading, default format and independently selected partial input.
    if parts[0].lower().startswith("/loadconfigfromfiles"):
        optional = {"-files", "-listfile", "-format", "-partial"}
        requirements = {rule for rule in requirements if rule[0] or not any(name.lower() in optional for name in rule[1])}
        if any(option[0].lower() == "-format" for option in options(parts)) and any(
                "частичной загрузки" in text and "автоматического определения" in text and "Format" in text for text in parts[1:]):
            requirements.add((("-files", "-listFile"), ("-Format",)))
    if parts[0].lower().startswith("/createdistributivepackage") and any("updsetup.zip" in text for text in parts[1:]):
        requirements = {rule for rule in requirements if rule != ((), ("-PackageFileName",))}
    # Some requirements and exclusions are stated only in the option prose.
    # These are reviewed semantic rules, gated by evidence in each snapshot;
    # ignored or ineffective options must not become invented hard errors.
    command_match = COMMAND.match(header)
    command = command_match[1].lower() if command_match else header.split()[0].lower()
    prose = re.sub(r"(?<!\w)[‑–—]\s*(?=[A-Za-z])", "-", " ".join(parts[1:])).lower()
    if command == "/managecfgsupport" and re.search(r"при отсутствии (?:параметра|ключа) генерируется ошибка", prose):
        requirements.add(((), ("-disableSupport",)))
    if command == "/deletecfg" and "без" in prose and "не допускается" in prose:
        requirements.add(((), ("-AllExtensions", "-Extension")))
    if command in ("/createdistributive", "/createdistributivepackage") and "может быть использован только один" in prose:
        conflicts.add(("-MakeFiles", "-MakeSetup"))
    if command == "/mergecfg" and "одновременно" in prose and "-extension" in prose and "прервано с ошибкой" in prose:
        conflicts.update({("-EnableSupport", "-Extension"), ("-DisableSupport", "-Extension")})
    if command == "/checkcanapplyconfigurationextensions" and "не поддерживается совместное использование" in prose:
        conflicts.update({("-AllZones", "-Extension"), ("-AllZones", "-Z")})
    if command == "/loadconfigfromfiles" and "несовместим" in prose.replace("не совместим", "несовместим"):
        conflicts.update({("-AllExtensions", "-files"), ("-AllExtensions", "-listFile")})
    return sorted(requirements), sorted(conflicts)


def quoted_operands(parts):
    """Named values explicitly enclosed in quotes or described as quoted."""
    header = normalized_syntax(parts[0])
    quoted = set()
    for name, _, _, _ in options(parts):
        if re.search(re.escape(name) + r'(?![\w])\s*[:=]?\s*[«"“]', header):
            quoted.add(name)
        for paragraph in parts[1:]:
            paragraph = option_paragraph(paragraph, header).lstrip("●• ")
            if re.match(re.escape(name) + r'(?=\s|<|$)', paragraph) and "кавычк" in paragraph:
                quoted.add(name)
    return sorted(quoted)


def cross_command_conflicts(parts):
    pairs = set()
    for paragraph in parts[1:]:
        match = re.search(r'Команда (/[A-Za-z][A-Za-z0-9_]*) не может использоваться одновременно с командой (/[A-Za-z][A-Za-z0-9_]*)', paragraph)
        if match:
            pairs.add(tuple(sorted((match[1].lower(), match[2].lower()))))
    return sorted(pairs)


def primary_operand_count(parts):
    """Count mandatory positional operands, excluding switch values."""
    if parts[0].lower().startswith("/createtemplatelistfile") and any("Если не указано" in text and "по умолчанию" in text for text in parts[1:]):
        return 0
    depth, after_option, count = 0, False, 0
    for token in re.findall(r"<[^>]*>|-[A-Za-z][A-Za-z0-9_]*|\[|\]", normalized_syntax(parts[0])):
        if token == "[":
            depth += 1
        elif token == "]":
            depth -= 1
            after_option = False
        elif token.startswith("-"):
            after_option = True
        else:
            if not after_option and depth == 0:
                count += 1
            after_option = False
    return count


def primary_values(parts):
    """A placeholder's enumeration is an operand domain, even for -prefixed values.

    Only the immediately introduced list belongs to that placeholder. Later
    lists can describe files, return codes or command options instead.
    """
    header = normalized_syntax(parts[0])
    match = re.match(r'^/\w+\s*\[?\s*(<[^>]+>)', header)
    if not match:
        return []
    placeholder = re.sub(r'\s+', '', match[1]).lower()
    named = {m[0].lower() for m in OPTION.finditer(header)}
    active, result = False, []
    for paragraph in parts[1:]:
        paragraph = re.sub(r'^([●•]\s*)[‑–—−](?=\s)', r'\1-', paragraph)
        value = re.match(r'^[●•]\s*(-?[A-Za-z][A-Za-z0-9_]*|[+-]|[0-9]+)\s+[‑–—]', paragraph)
        if active:
            if not value or value[1].lower() in named:
                break
            result.append(value[1])
        elif placeholder in re.sub(r'\s+', '', paragraph).lower() and paragraph.rstrip().endswith(':'):
            active = True
    return result


def enum_values(parts):
    active = None
    result = [("", value) for value in primary_values(parts)]
    header = normalized_syntax(parts[0])
    # A pipe inside a Russian placeholder (e.g. file name|path) describes
    # input, while Latin CLI identifiers specify literal alternatives.
    for match in re.finditer(r'(-[A-Za-z][A-Za-z0-9_]*)\s+<?([A-Za-z]+(?:\|[A-Za-z]+)+)>?', header):
        result.extend((match[1], value) for value in match[2].split('|'))
    valued = {name.lower() for name, _, style, _ in options(parts) if style != "flag"}
    for paragraph in parts[1:]:
        paragraph = option_paragraph(paragraph, normalized_syntax(parts[0]))
        paragraph = re.sub(r"^([●•]\s*)[‑–—−](?=\s)", r"\1-", paragraph)
        if not paragraph.startswith(("●", "•")) and re.search(r"возврат|возвращ", paragraph, re.I):
            active = None
        match = re.match(r"^[●•]\s*(-[A-Za-z][A-Za-z0-9_]*)\b", paragraph)
        if match:
            # Some lists describe when a free-form name applies, rather than
            # literal permitted names. Never turn those conditions into enums.
            active = match[1] if (paragraph.rstrip().endswith(":") and match[1].lower() in valued
                                 and "в зависимости от значения" not in paragraph.lower()) else None
            continue
        value = re.match(r"^[●•]\s*([A-Za-z][A-Za-z0-9_]*|[+-]|[0-9]+)\s+[‑–—]", paragraph)
        if active and value and value[1] not in ("hhh", "mm"):
            result.append((active, value[1]))
    return list(dict.fromkeys(result))


def cpp(value):
    return 'L' + json.dumps(value, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    profiles = {}
    for path in args.references.glob("*.mht*"):
        match = re.search(r"8\.(?:3|5)\.\d+", path.name)
        if not match:
            continue
        if match[0] in profiles:
            raise ValueError(f"Duplicate version {match[0]}")
        blocks = versions_module.read_snapshot(path)
        if len(blocks) < 100 or match[0] not in blocks[0][1]:
            raise ValueError(f"Incomplete or mismatched snapshot {path.name}")
        profiles[match[0]] = definitions(blocks)
    versions = re.findall(r'^    L"([\d.]+)",$', (root / "src/core/launcher/parameter_versions.inc").read_text(encoding="utf8"), re.M)
    if set(versions) != set(profiles):
        raise ValueError("Snapshot versions must exactly match checked-in compatibility profiles")
    syntax, flags, enums, requirements, conflicts, quoting, cross_conflicts = {}, {}, {}, {}, {}, {}, {}
    for i, version in enumerate(versions):
        for command, parts in profiles[version].items():
            key = command, syntax_only(parts[0]), primary_operand_count(parts)
            syntax[key] = syntax.get(key, 0) | 1 << i
            for position, option in enumerate(options(parts)):
                key = (command, *option, position)
                flags[key] = flags.get(key, 0) | 1 << i
            required, excluded = grammar_rules(parts)
            known = {option[0].lower() for option in options(parts)}
            rule_names = {name.lower() for triggers, needed in required for name in (*triggers, *needed)}
            rule_names |= {name.lower() for pair in excluded for name in pair}
            if not rule_names <= known:
                raise ValueError(f"Rules reference missing options in {version} {command}: {rule_names - known}")
            for triggers, needed in required:
                key = command, "|".join(triggers), "|".join(needed)
                requirements[key] = requirements.get(key, 0) | 1 << i
            for first, second in excluded:
                key = command, first, second
                conflicts[key] = conflicts.get(key, 0) | 1 << i
            for name, value in enum_values(parts):
                key = command, name, value
                enums[key] = enums.get(key, 0) | 1 << i
            for name in quoted_operands(parts):
                key = command, name
                quoting[key] = quoting.get(key, 0) | 1 << i
            for pair in cross_command_conflicts(parts):
                cross_conflicts[pair] = cross_conflicts.get(pair, 0) | 1 << i
    lines = ["// Derived CLI syntax and identifiers only; no documentation prose.",
             "// Refresh: python tools/update-parameter-grammar.py", "const CommandSyntax command_syntax[] = {"]
    for (command, text, primary_required), mask in sorted(syntax.items()):
        lines.append(f"    {{{cpp(command)}, {cpp(text)}, {primary_required}, 0x{mask:08x}u}},")
    lines += ["};", "const OptionSyntax option_syntax[] = {"]
    for (command, name, text, style, optional, position), mask in sorted(flags.items()):
        lines.append(f"    {{{cpp(command)}, {cpp(name)}, {cpp(text)}, OptionStyle::{style}, {'true' if optional else 'false'}, {position}, 0x{mask:08x}u}},")
    lines += ["};", "const OptionChoice option_choices[] = {"]
    for (command, name, value), mask in sorted(enums.items(), key=lambda item: item[0][:2]):
        lines.append(f"    {{{cpp(command)}, {cpp(name)}, {cpp(value)}, 0x{mask:08x}u}},")
    lines += ["};", "const PresenceRule presence_rules[] = {"]
    for (command, triggers, needed), mask in sorted(requirements.items()):
        lines.append(f"    {{{cpp(command)}, {cpp(triggers)}, {cpp(needed)}, 0x{mask:08x}u}},")
    lines += ["};", "const ConflictRule conflict_rules[] = {"]
    for (command, first, second), mask in sorted(conflicts.items()):
        lines.append(f"    {{{cpp(command)}, {cpp(first)}, {cpp(second)}, 0x{mask:08x}u}},")
    lines += ["};", "const QuoteRule quote_rules[] = {"]
    for (command, option), mask in sorted(quoting.items()):
        lines.append(f"    {{{cpp(command)}, {cpp(option)}, 0x{mask:08x}u}},")
    lines += ["};", "const CommandConflictRule command_conflict_rules[] = {"]
    for (first, second), mask in sorted(cross_conflicts.items()):
        lines.append(f"    {{{cpp(first)}, {cpp(second)}, 0x{mask:08x}u}},")
    lines += ["};", ""]
    output = root / "src/core/launcher/parameter_grammar.inc"
    rendered = "\n".join(lines)
    if args.check:
        if output.read_text(encoding="utf8") != rendered:
            raise ValueError("Generated CLI grammar is stale")
    else:
        output.write_text(rendered, encoding="utf8", newline="\n")
    print(f"{len(profiles)} profiles, {len(syntax)} syntax variants, {len(flags)} option variants, {len(enums)} option values, "
          f"{len(requirements)} presence rules, {len(conflicts)} conflicts, {len(quoting)} quoting rules, "
          f"{len(cross_conflicts)} cross-command conflicts")


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8")
    main()
