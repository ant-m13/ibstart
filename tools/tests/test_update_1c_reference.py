"""Synthetic regression inputs; CI uses synthetic data."""
import importlib.util
import json
from email.message import EmailMessage
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("reference", Path(__file__).parents[1] / "update-1c-reference.py")
REFERENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REFERENCE)


def section(extra=()):
    return [("h2", "3.4. *.v8i"), ("p", "ID"), ("p", "Connect\u00adнеобязательный"),
            ("p", "Version"), ("p", "AdditionalParameters"), *extra,
            ("h2", "3.5. 1cestart.cfg"), ("p", "OtherFileOnly")]


def snapshot(root, kind, version="8.3.28", title_version=None):
    appendix = 3 if kind == "service-files" else 7
    content = section() if appendix == 3 else [("p", k) for k in ("/F", "/S", "/N", "/P")]
    message = EmailMessage()
    message["Snapshot-Content-Location"] = f"https://its.1c.ru/db/v{version.replace('.', '')}doc/content/1/hdoc"
    html = f"<title>Приложение {appendix}. Synthetic {title_version or version}</title>"
    html += "".join(f"<{tag}>{text}</{tag}>" for tag, text in content)
    html += "<p>Synthetic padding.</p>" * 110
    message.set_content(html, subtype="html", charset="utf-8")
    directory = root / kind
    directory.mkdir(exist_ok=True)
    path = directory / f"synthetic {version}.mhtml"
    path.write_bytes(message.as_bytes())
    return path


class ReferenceTests(unittest.TestCase):
    def test_scopes_old_qualifiers_and_soft_hyphen(self):
        entries = REFERENCE.fields(section([("p", "ShowInList только мобильный клиент"),
                                            ("p", "MobilePublicKey обязательный для iOS")]))
        self.assertIn("Connect", entries)
        self.assertIn("ShowInList", entries)
        self.assertIn("MobilePublicKey", entries)
        self.assertNotIn("OtherFileOnly", entries)

    def test_mobile_values_do_not_leak_into_previous_field(self):
        entries = REFERENCE.fields(section([
            ("p", "AppArch необязательный"), ("p", "Synthetic description."),
            ("p", "ShowInList только мобильный клиент"),
            ("p", "● 0 – Synthetic off."), ("p", "● 1 ‑ Synthetic on."),
        ]))
        self.assertEqual(REFERENCE.enum_values(entries["AppArch"]), [])
        self.assertEqual(REFERENCE.enum_values(entries["ShowInList"]), ["0", "1"])

    def test_numeric_values_include_two(self):
        self.assertEqual(REFERENCE.enum_values(["UseProxy", "● 2 – Synthetic."]), ["2"])

    def test_rejects_wrong_or_incomplete_section(self):
        with self.assertRaises(ValueError):
            REFERENCE.fields([("h2", "1cestart.cfg"), ("p", "ID")])
        with self.assertRaises(ValueError):
            REFERENCE.fields([("h2", "*.v8i"), ("p", "ID"), ("h2", "next")])
        with self.assertRaises(ValueError):
            REFERENCE.fields(section([("p", "ID")]))

    def test_ranges_do_not_fill_gaps_or_cross_branches(self):
        self.assertEqual(REFERENCE.ranges(["8.3.26", "8.3.27", "8.5.1", "8.5.4"]),
                         "8.3.26–8.3.27, 8.5.1, 8.5.4")

    def test_import_is_reproducible_and_accepts_one_new_appendix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot(root, "service-files")
            snapshot(root, "launch-parameters")
            snapshot(root, "service-files", "8.3.29")
            data = REFERENCE.collect(root, "2026-10-19")
            self.assertEqual(data, REFERENCE.collect(root, "2026-10-19"))
            report = REFERENCE.render(data)["docs/1c-reference-versions.md"]
            self.assertIn("| 8.3.29 |", report)
            self.assertIn("Нет данных", report)
            self.assertNotIn("Synthetic padding", json.dumps(data))

    def test_rejects_duplicate_version_and_mismatched_title(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot(root, "service-files")
            path = snapshot(root, "launch-parameters")
            duplicate = path.with_name("duplicate 8.3.28.mhtml")
            duplicate.write_bytes(path.read_bytes())
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                REFERENCE.collect(root, "2026-10-19")
            duplicate.unlink()
            snapshot(root, "launch-parameters", title_version="8.3.29")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                REFERENCE.collect(root, "2026-10-19")

    def test_detects_definition_changes_in_same_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot(root, "service-files")
            path = snapshot(root, "launch-parameters")
            before = REFERENCE.collect(root, "2026-10-19")
            path.write_bytes(path.read_bytes().replace(b"Synthetic padding", b"Synthetic changed"))
            after = REFERENCE.collect(root, "2026-10-19")
            self.assertNotEqual(before["sources"][0]["entries"]["/P"]["sha256"],
                                after["sources"][0]["entries"]["/P"]["sha256"])
            self.assertEqual(set(after["sources"][0]),
                             {"kind", "version", "source_url", "preview", "entries"})


if __name__ == "__main__":
    unittest.main()
