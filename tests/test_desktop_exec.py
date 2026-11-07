"""A game folder whose name contains % must still make a menu entry the desktop accepts.

python -I -B tests/test_desktop_exec.py
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_builder():
    for name in ("tkinter", "tkinter.filedialog", "tkinter.messagebox", "tkinter.ttk"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    tk = sys.modules["tkinter"]
    tk.Tk = type("Tk", (), {})
    tk.filedialog = sys.modules["tkinter.filedialog"]
    tk.messagebox = sys.modules["tkinter.messagebox"]
    tk.ttk = sys.modules["tkinter.ttk"]
    spec = importlib.util.spec_from_file_location("hookshot_builder_desktop", ROOT / "builder" / "linux" / "hookshot_builder.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load_builder()
EXE = "/home/steve/100% Games/Hookshot/hookshot"


def field_codes(argument):
    """A % that is not itself escaped. %% is a literal percent; desktop-file-validate reads the rest as field codes."""
    return re.findall(r"%([^%])", argument.replace("%%", ""))


class DesktopExecTests(unittest.TestCase):
    def test_a_percent_in_the_folder_is_escaped(self):
        old = '"%s"' % EXE
        self.assertNotEqual(field_codes(old), [])
        arg = builder.desktop_exec(EXE)
        self.assertEqual(arg, '"/home/steve/100%% Games/Hookshot/hookshot"')
        self.assertEqual(field_codes(arg), [])

    def test_a_quote_and_a_backslash_stay_inside_the_argument(self):
        arg = builder.desktop_exec('/home/steve/My "Games"\\Hookshot/hookshot')
        self.assertEqual(arg, '"/home/steve/My \\"Games\\"\\\\Hookshot/hookshot"')

    def test_validator_rejects_the_old_line(self):
        validate = shutil.which("desktop-file-validate")
        if not validate:
            self.skipTest("desktop-file-validate is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "hookshot.desktop")
            body = ("[Desktop Entry]\nType=Application\nName=Hookshot\nComment=x\n"
                    "Exec=%s\nPath=/home/steve/100%% Games/Hookshot\nIcon=hookshot\nCategories=Game;\nTerminal=false\n")
            with open(path, "w", encoding="utf-8") as f:
                f.write(body % ('"%s"' % EXE))
            old = subprocess.run([validate, path], text=True, capture_output=True)
            self.assertNotEqual(old.returncode, 0, old.stdout + old.stderr)
            self.assertIn("invalid field code", old.stdout + old.stderr)
            with open(path, "w", encoding="utf-8") as f:
                f.write(body % builder.desktop_exec(EXE))
            new = subprocess.run([validate, path], text=True, capture_output=True)
            self.assertEqual(new.returncode, 0, new.stdout + new.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
