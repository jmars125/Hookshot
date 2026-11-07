"""A failed host-memory import is not retried until its hold expires (issue 218)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ImportHoldTests(unittest.TestCase):
    def test_failed_import_is_held(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-import-hold-") as tmp:
            binary = Path(tmp) / "import_hold_test"
            compile = subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "gpu" / "vk"),
                 str(ROOT / "tests" / "import_hold_test.cpp"), "-o", str(binary)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(compile.returncode, 0, compile.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
