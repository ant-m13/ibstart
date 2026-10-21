"""Synthetic regression tests for boundaries in the authoring coverage check."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("coverage", Path(__file__).resolve().parents[1] / "check-launch-help-coverage.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CoverageTests(unittest.TestCase):
    def test_final_mobile_command_before_colon_is_included(self):
        self.assertEqual(module.section_identifiers(["● /First, /Last: описание см. здесь."], "mobile"), {"/first", "/last"})

    def test_table_headers_are_separate_from_descriptions_and_enum_bullets(self):
        paragraphs = ["Name=<значение>", "Описание параметра.", "● Choice — значение.", "Z n"]
        self.assertEqual(module.section_identifiers(paragraphs, "connection"), {"name", "z"})

    def test_section_ends_at_next_peer_heading(self):
        blocks = [("h2", "Командная строка мобильной версии"), ("p", "● /One, /Two: здесь."),
                  ("h2", "Следующий раздел"), ("p", "● /Other: там.")]
        self.assertEqual(module.section_identifiers(module.section(blocks, "mobile"), "mobile"), {"/one", "/two"})

    def test_short_key_does_not_match_longer_identifier(self):
        self.assertEqual(module.missing_identifiers({"/o", "/oida"}, ["• /OIDA — аутентификация."]), ["/o"])


if __name__ == "__main__":
    unittest.main()
