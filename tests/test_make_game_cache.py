"""Game-free checks of make_game's NUP cache selection and replacement.

Run with: python -I tests/test_make_game_cache.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


SOURCE = Path(os.environ.get("HOOKSHOT_CACHE_TEST_SOURCE", Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"))
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class Prepared(Exception):
    """Stop at the first lift request, before compiling or reading any game."""


class ExtractionFailed(Exception):
    pass


def module_bytes(supported=True, payload=b"new authored module"):
    header = bytearray(0x60)
    header[:4] = b"NSO0"
    header[0x40:0x44] = bytes.fromhex(make_game.BUILD_ID if supported else "01234567")
    return bytes(header) + payload


class NupCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-cache-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.out = self.root / "output"
        self.work = self.root / "work"
        self.addCleanup(self.assert_owned_fixture)
        self.out.mkdir()
        self.work.mkdir()
        self.old_nup = self.root / "old.nup"
        self.old_nup.write_bytes(b"old authored input")
        self.new_nup = self.root / "new.nup"
        self.new_nup.write_bytes(b"new authored input of a different size")
        (self.out / "romfs").mkdir()
        (self.out / "romfs" / "asset.txt").write_bytes(b"old authored assets")
        (self.out / ".hookshot_exefs").mkdir()
        (self.out / ".hookshot_exefs" / "main").write_bytes(module_bytes(payload=b"old authored module"))
        self.marker = self.out / ".hookshot_romfs"
        self.marker.write_text(self.stamp(self.old_nup) + "\n")
        save = self.out / "user" / "save"
        save.mkdir(parents=True)
        (save / "sentinel.txt").write_bytes(b"authored save sentinel")
        self.extractions = []
        self.output = ""

    def assert_owned_fixture(self):
        # The builder only receives descendants of this test-owned temporary directory.
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.out.resolve().is_relative_to(self.root))
        self.assertTrue(self.work.resolve().is_relative_to(self.root))

    def stamp(self, nup):
        return "unpacked by this builder: %s %d" % (nup.name, nup.stat().st_size)

    def cache(self):
        return {path.relative_to(self.out).as_posix(): path.read_bytes()
                for path in self.out.rglob("*") if path.is_file() and ".game" not in path.parts}

    def run_to_lift(self, nup, failure=None, supported=True, include_romfs=True):
        def run(cmd, env=None, on_line=None):
            if "nx2/nup.py" in cmd:
                target = Path(cmd[cmd.index("--extract") + 1]).resolve()
                self.assertTrue(target.is_relative_to(self.root))
                self.extractions.append(nup)
                (target / "exefs").mkdir(parents=True)
                (target / "exefs" / "main").write_bytes(module_bytes(supported=supported))
                if failure:
                    raise failure
                if include_romfs:
                    (target / "romfs").mkdir()
                    (target / "romfs" / "asset.txt").write_bytes(b"new authored assets")
                    (target / "romfs" / "new-only.txt").write_bytes(b"new authored file")
                return
            if "nx2/lift.py" in cmd:
                raise Prepared
            self.fail("Unexpected build subprocess: %r" % cmd)

        argv = ["make_game.py", "--nup", str(nup), "--out", str(self.out), "--work", str(self.work)]
        # The only external operation exercised is authored extraction above; no compiler, SDK,
        # translation, power-state update or real game is involved.
        with contextlib.ExitStack() as stack:
            blocked = [
                stack.enter_context(mock.patch.object(make_game.subprocess, "Popen",
                                                     side_effect=AssertionError("Unexpected process launch"))),
                stack.enter_context(mock.patch.object(os, "system",
                                                     side_effect=AssertionError("Unexpected shell command"))),
                stack.enter_context(mock.patch.object(os, "startfile", create=True,
                                                     side_effect=AssertionError("Unexpected file launch"))),
            ]
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            stack.enter_context(mock.patch.object(make_game, "check_windows_sdk"))
            stack.enter_context(mock.patch.object(make_game, "drop_stale_objects"))
            stack.enter_context(mock.patch.object(make_game, "no_throttle"))
            stack.enter_context(mock.patch("ctypes.WinDLL", create=True))
            stack.enter_context(mock.patch.object(make_game, "run", side_effect=run))
            output = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            try:
                make_game.main()
            finally:
                self.output = output.getvalue()
                for launch in blocked:
                    launch.assert_not_called()

    def test_changed_source_replaces_romfs_and_exefs_together(self):
        with self.assertRaises(Prepared):
            self.run_to_lift(self.new_nup)
        self.assertEqual(self.extractions, [self.new_nup])
        self.assertEqual((self.out / "romfs" / "asset.txt").read_bytes(), b"new authored assets")
        self.assertTrue((self.out / "romfs" / "new-only.txt").is_file())
        self.assertEqual((self.out / ".hookshot_exefs" / "main").read_bytes(), module_bytes())
        self.assertEqual(self.marker.read_text(), self.stamp(self.new_nup) + "\n")
        self.assertEqual((self.out / "user/save/sentinel.txt").read_bytes(), b"authored save sentinel")

    def test_same_source_reuses_the_cache(self):
        before = self.cache()
        with self.assertRaises(Prepared):
            self.run_to_lift(self.old_nup)
        self.assertEqual(self.extractions, [])
        self.assertEqual(self.cache(), before)
        self.assertEqual((self.work / "exefs/main").read_bytes(),
                         (self.out / ".hookshot_exefs/main").read_bytes())

    def test_failed_extraction_preserves_the_previous_cache_and_save(self):
        before = self.cache()
        with self.assertRaises(ExtractionFailed):
            self.run_to_lift(self.new_nup, failure=ExtractionFailed())
        self.assertEqual(self.extractions, [self.new_nup])
        self.assertEqual(self.cache(), before)

    def test_unsupported_build_preserves_the_previous_cache_and_save(self):
        before = self.cache()
        with self.assertRaises(SystemExit) as result:
            self.run_to_lift(self.new_nup, supported=False)
        self.assertEqual(self.extractions, [self.new_nup])
        self.assertEqual(result.exception.code, 1)
        self.assertIn("This is build 01234567. Hookshot supports build %s." % make_game.BUILD_ID, self.output)
        self.assertEqual(self.cache(), before)

    def test_missing_romfs_preserves_the_previous_cache_and_save(self):
        before = self.cache()
        with self.assertRaises(SystemExit) as result:
            self.run_to_lift(self.new_nup, include_romfs=False)
        self.assertEqual(self.extractions, [self.new_nup])
        self.assertEqual(result.exception.code, 1)
        self.assertIn("The package did not contain a romfs folder.", self.output)
        self.assertEqual(self.cache(), before)

    def test_reparse_cache_is_rejected_before_extraction_or_replacement(self):
        before = self.cache()
        original_lstat = os.lstat

        def reparse_romfs(path, *args, **kwargs):
            result = original_lstat(path, *args, **kwargs)
            if Path(path).absolute() != self.out / "romfs":
                return result

            class ReparseStat:
                st_file_attributes = getattr(result, "st_file_attributes", 0) | 0x400

                def __getattr__(self, name):
                    return getattr(result, name)

            return ReparseStat()

        with mock.patch.object(os, "lstat", side_effect=reparse_romfs):
            with self.assertRaises(SystemExit) as result:
                self.run_to_lift(self.new_nup)
        self.assertEqual(result.exception.code, 1)
        self.assertEqual(self.extractions, [])
        self.assertEqual(self.cache(), before)


if __name__ == "__main__":
    unittest.main()
