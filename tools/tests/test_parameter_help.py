"""Validate embedded, project-authored explanations."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "src/core/launcher"
STRING = r'"(?:[^"\\]|\\.)*"'


def strings(path):
    return [json.loads(value) for value in re.findall(STRING, path.read_text(encoding="utf8"))]


class EmbeddedHelpTests(unittest.TestCase):
    def test_every_catalogue_entry_has_one_own_explanation(self):
        catalog = (CORE / "parameter_catalog.cpp").read_text(encoding="utf8").split("struct VersionPresence")[0]
        catalog += (CORE / "parameter_reference.inc").read_text(encoding="utf8")
        keys = {json.loads(v).lower() for v in re.findall(r'\{L(' + STRING + r'), L', catalog)}
        values = strings(CORE / "parameter_help.inc")
        self.assertEqual(len(values) % 2, 0)
        explanations = [(key.lower(), text) for key, text in zip(values[::2], values[1::2])]
        self.assertEqual(len(explanations), len(keys))
        self.assertEqual({key for key, _ in explanations}, keys)
        self.assertTrue(all(text.strip() for _, text in explanations))

    def test_scoped_explanations_have_valid_nonoverlapping_profiles(self):
        versions = re.findall(r'^    L"8\.[35]\.\d+",$', (CORE / "parameter_versions.inc").read_text(encoding="utf8"), re.M)
        limit = (1 << len(versions)) - 1
        known = {key.lower() for key in strings(CORE / "parameter_help.inc")[::2]}
        masks = {}
        lines = (CORE / "parameter_context_help.inc").read_text(encoding="utf8").splitlines()
        for line in lines:
            if line.startswith("//") or not line.strip():
                continue
            match = re.fullmatch(r'\{L(' + STRING + r'), L(' + STRING + r'), L(' + STRING + r'), (0x[0-9a-f]+)u\},', line)
            self.assertIsNotNone(match, line)
            command, option, text = [json.loads(s) for s in match.groups()[:3]]
            mask = int(match[4], 16)
            self.assertIn(command.lower(), known)
            self.assertTrue(text.strip())
            self.assertGreater(mask, 0, (command, option))
            self.assertEqual(mask & ~limit, 0)
            if option:  # Empty option means additive command notes.
                key = command.lower(), option.lower()
                self.assertEqual(masks.get(key, 0) & mask, 0, key)
                masks[key] = masks.get(key, 0) | mask


if __name__ == "__main__":
    unittest.main()
