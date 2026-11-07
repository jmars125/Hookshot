"""The Linux builder shows an @@error sentence whole. The old split kept only what followed its second word.

python -I -B tests/test_builder_status.py
"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_status", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

MODULE = "@@error The file main in this dump is not a game module (it starts ab)."
STUDIO = "@@error Your Visual Studio is too old for this game's renderer."


def old_split(line):
    return line[2:].split(" ", 2)[-1]


class BuilderStatusTests(unittest.TestCase):
    def test_error_keeps_its_first_word(self):
        self.assertEqual(old_split(MODULE), "file main in this dump is not a game module (it starts ab).")
        self.assertEqual(make_game.reported(MODULE), "The file main in this dump is not a game module (it starts ab).")
        self.assertEqual(old_split(STUDIO), "Visual Studio is too old for this game's renderer.")
        self.assertEqual(make_game.reported(STUDIO), "Your Visual Studio is too old for this game's renderer.")
        self.assertEqual(make_game.reported("@@error no output"), "no output")

    def test_check_keeps_the_sentence_after_ok_or_bad(self):
        ok = "@@check ok This is the version Hookshot supports (build e0f3ccfa)."
        bad = "@@check bad This is build abcdef01. Hookshot supports build e0f3ccfa."
        self.assertEqual(make_game.reported(ok), "This is the version Hookshot supports (build e0f3ccfa).")
        self.assertEqual(make_game.reported(bad), "This is build abcdef01. Hookshot supports build e0f3ccfa.")
        self.assertEqual(old_split(ok), make_game.reported(ok))

    def test_other_lines_are_not_status(self):
        self.assertIsNone(make_game.reported("@@stage compile Compiling"))
        self.assertIsNone(make_game.reported("@@done /tmp/Hookshot"))
        self.assertIsNone(make_game.reported("compiled in 10s"))

    def test_the_builder_asks_make_game(self):
        source = (ROOT / "builder" / "linux" / "hookshot_builder.py").read_text(encoding="utf-8")
        start = source.index("def check_version")
        body = source[start:source.index("\n    def ", start + 1)]
        self.assertIn("make_game.reported", body)
        self.assertNotIn('.split(" ", 2)[-1]', body)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
