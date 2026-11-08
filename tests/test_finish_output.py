"""Exercise the actual finish-stage file loop with temporary, synthetic build files.

python -I -B tests/test_finish_output.py
"""
import ast
import contextlib
import ctypes
import errno
import importlib.util
import io
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_finish", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

# Run the production loop and following image copy, including the real helper, without running a game build.
main = next(node for node in ast.parse(SOURCE.read_text(encoding="utf-8")).body
            if isinstance(node, ast.FunctionDef) and node.name == "main")
loops = [node for node in main.body if isinstance(node, ast.For)
         and isinstance(node.iter, ast.Name) and node.iter.id == "BESIDE"]
assert len(loops) == 1
position = main.body.index(loops[0])
FINISH = compile(ast.Module(body=main.body[position:position + 2], type_ignores=[]), str(SOURCE), "exec")


class FinishOutputTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="hookshot-finish-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.built, self.out = self.root / "built", self.root / "game"
        self.built.mkdir()
        self.out.mkdir()
        self.source = self.built / "hookshot.exe"
        self.destination = self.out / "hookshot.exe"
        self.source.write_bytes(b"new synthetic executable")
        self.destination.write_bytes(b"existing synthetic executable")
        self.work = self.root / "work"
        (self.work / "sa" / "image").mkdir(parents=True)
        self.image = self.work / "sa" / "image" / "image.bin"
        self.image.write_bytes(b"new synthetic image")
        (self.out / "image.bin").write_bytes(b"existing synthetic image")

    def finish(self):
        with mock.patch.object(make_game, "BESIDE", ["hookshot.exe"]):
            exec(FINISH, make_game.__dict__, {"built": str(self.built), "out": str(self.out), "work": str(self.work)})

    def assert_no_staging_files(self):
        self.assertEqual(sorted(path.name for path in self.out.iterdir()), ["hookshot.exe", "image.bin"])

    def test_completed_copy_replaces_existing_file(self):
        self.source.chmod(0o755)
        self.finish()
        self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), stat.S_IMODE(self.source.stat().st_mode))
        self.assertEqual((self.out / "image.bin").read_bytes(), self.image.read_bytes())
        self.assert_no_staging_files()

    def test_first_install_without_an_existing_destination(self):
        self.destination.unlink()
        (self.out / "image.bin").unlink()
        self.finish()
        self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())
        self.assertEqual((self.out / "image.bin").read_bytes(), self.image.read_bytes())
        self.assert_no_staging_files()

    def test_incomplete_copy_preserves_existing_file(self):
        def incomplete_copy(source, destination):
            Path(destination).write_bytes(b"incomplete replacement")
            raise OSError(errno.ENOSPC, "synthetic disk-full error")

        with mock.patch.object(make_game.shutil, "copy2", side_effect=incomplete_copy):
            with self.assertRaises(OSError):
                self.finish()
        self.assertEqual(self.destination.read_bytes(), b"existing synthetic executable")
        self.assert_no_staging_files()

    def test_incomplete_image_copy_preserves_that_image(self):
        copy = make_game.shutil.copy2

        def incomplete_image(source, destination):
            if Path(source) == self.image:
                Path(destination).write_bytes(b"incomplete image")
                raise OSError(errno.ENOSPC, "synthetic disk-full error")
            return copy(source, destination)

        with mock.patch.object(make_game.shutil, "copy2", side_effect=incomplete_image):
            with self.assertRaises(OSError):
                self.finish()
        self.assertEqual((self.out / "image.bin").read_bytes(), b"existing synthetic image")
        self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())
        self.assert_no_staging_files()

    def test_failed_replace_cleans_readonly_staging_file(self):
        self.source.chmod(stat.S_IREAD)
        try:
            with mock.patch.object(make_game.os, "replace", side_effect=PermissionError("synthetic denied replacement")):
                with contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        self.finish()
            self.assertEqual(self.destination.read_bytes(), b"existing synthetic executable")
            self.assert_no_staging_files()
        finally:
            self.source.chmod(stat.S_IREAD | stat.S_IWRITE)

    @unittest.skipUnless(os.name == "nt", "Windows file-sharing contract")
    def test_locked_destination_reports_action_and_preserves_file(self):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                      ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle.restype = ctypes.c_int
        # Allow readers/writers but not deletion/replacement, as for an executable still in use.
        handle = kernel.CreateFileW(str(self.destination), 0x80000000, 0x1 | 0x2, None, 3, 0x80, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value, ctypes.get_last_error())
        output = io.StringIO()
        try:
            with contextlib.redirect_stdout(output):
                with self.assertRaises(SystemExit) as stopped:
                    self.finish()
            self.assertEqual(stopped.exception.code, 1)
            self.assertIn("@@error", output.getvalue())
            self.assertIn("Close", output.getvalue())
            self.assertIn(str(self.destination), output.getvalue())
            self.assertIn(str(self.built), output.getvalue())
            self.assertEqual(self.destination.read_bytes(), b"existing synthetic executable")
            self.assert_no_staging_files()
        finally:
            self.assertTrue(kernel.CloseHandle(handle))
        self.finish()
        self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())

    @unittest.skipUnless(os.name == "nt", "Windows read-only file contract")
    def test_readonly_destination_is_not_removed_or_unlocked(self):
        self.destination.chmod(stat.S_IREAD)
        try:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                with self.assertRaises(SystemExit):
                    self.finish()
            self.assertIn("writable", output.getvalue())
            self.assertEqual(self.destination.read_bytes(), b"existing synthetic executable")
            self.assertFalse(self.destination.stat().st_mode & stat.S_IWRITE)
            self.assert_no_staging_files()
        finally:
            self.destination.chmod(stat.S_IREAD | stat.S_IWRITE)


if __name__ == "__main__":
    unittest.main()
