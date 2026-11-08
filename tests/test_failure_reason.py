"""The line make_game reports when a build step stops.

A compiler error can be hundreds of lines before the step's last line. The last
dozen lines then only say that something failed, and that is what the builder shows.

Run with: python -I -B tests/test_failure_reason.py
"""
import contextlib
import io
import os
from pathlib import Path
import tempfile
import unittest

import importlib.util

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

ERROR = "host/nn_video.c:19:10: fatal error: 'libavcodec/avcodec.h' file not found"


def scrolled_off():
    lines = ["starting", ERROR]
    lines += ["  code_%03d.c         18 MB     370s  (790s)" % i for i in range(30)]
    lines.append("compiled in 790s, 1 failed")
    return lines


class FailureReasonTests(unittest.TestCase):
    def test_an_error_above_the_last_lines_is_kept(self):
        lines = scrolled_off()
        tail = lines[-12:]
        old = next((l for l in reversed(tail) if "error:" in l.lower() and "CalledProcessError" not in l), tail[-1])
        self.assertEqual(old, "compiled in 790s, 1 failed")
        self.assertEqual(make_game.failure_reason(lines), ERROR)

    def test_an_error_still_in_the_last_lines_is_that_line(self):
        lines = ["ok", "lift.py: error: the table was short", "stopped"]
        self.assertEqual(make_game.failure_reason(lines), "lift.py: error: the table was short")

    def test_a_called_process_error_is_not_the_reason(self):
        self.assertEqual(make_game.failure_reason(["CalledProcessError: boom", "the compiler exited"]),
                         "the compiler exited")

    def test_no_output(self):
        self.assertEqual(make_game.failure_reason([]), "no output")

    def test_the_last_error_wins(self):
        lines = ["first error: one"] + ["  filler %d" % i for i in range(20)]
        lines.append("second error: two")
        lines += ["  more %d" % i for i in range(20)]
        self.assertEqual(make_game.failure_reason(lines), "second error: two")

    def test_run_reports_that_line(self):
        body = SOURCE.read_text(encoding="utf-8")
        start = body.index("def run(")
        end = body.index("\ndef build_id(")
        run = body[start:end]
        self.assertIn("failure_reason(lines)", run)
        self.assertNotIn("[-12:]", run)


class ChildExitTests(unittest.TestCase):
    def run_child(self, body):
        output, forwarded = io.StringIO(), []
        with tempfile.TemporaryDirectory(prefix="hookshot-child-exit-") as temporary:
            child = Path(temporary) / "child.py"
            child.write_text(body, encoding="utf-8")
            status = 0
            with contextlib.redirect_stdout(output):
                try:
                    make_game.run(make_game.PY + [str(child)], on_line=forwarded.append)
                except SystemExit as error:
                    status = error.code
        return status, output.getvalue(), forwarded

    def test_nonzero_status_keeps_the_last_progress_line(self):
        status, output, forwarded = self.run_child('print("translating input", flush=True)\nraise SystemExit(7)\n')
        self.assertEqual(status, 1)
        self.assertEqual(forwarded, ["translating input"])
        self.assertEqual(output, "translating input\n@@error child.py stopped (exit status 7): translating input\n")

    def test_silent_failure_keeps_its_status(self):
        status, output, forwarded = self.run_child("raise SystemExit(9)\n")
        self.assertEqual(status, 1)
        self.assertEqual(forwarded, [])
        self.assertEqual(output, "@@error child.py stopped (exit status 9): no output\n")

    def test_success_forwards_both_streams_without_an_error(self):
        body = 'import sys\nprint("output", flush=True)\nprint("detail", file=sys.stderr, flush=True)\n'
        status, output, forwarded = self.run_child(body)
        self.assertEqual(status, 0)
        self.assertEqual(forwarded, ["output", "detail"])
        self.assertEqual(output, "output\ndetail\n")

    def test_compiler_error_above_the_tail_is_kept_with_status(self):
        lines = scrolled_off()
        body = "for line in %r:\n    print(line, flush=True)\nraise SystemExit(7)\n" % lines
        status, output, forwarded = self.run_child(body)
        self.assertEqual(status, 1)
        self.assertEqual(forwarded, lines)
        self.assertEqual(output, "\n".join(lines) + "\n@@error child.py stopped (exit status 7): " + ERROR + "\n")

    @unittest.skipUnless(os.name == "posix", "POSIX reports a signal as a negative child return code")
    def test_terminated_child_reports_its_signal(self):
        body = 'import os, signal\nprint("translation began", flush=True)\nos.kill(os.getpid(), signal.SIGTERM)\n'
        status, output, forwarded = self.run_child(body)
        self.assertEqual(status, 1)
        self.assertEqual(forwarded, ["translation began"])
        self.assertEqual(output, "translation began\n@@error child.py stopped (signal 15): translation began\n")

    @unittest.skipUnless(os.name == "nt", "Windows returns native process status values")
    def test_windows_native_status_is_not_a_signal(self):
        body = 'import ctypes\nctypes.windll.kernel32.ExitProcess(ctypes.c_uint32(0xC0000005))\n'
        status, output, forwarded = self.run_child(body)
        self.assertEqual(status, 1)
        self.assertEqual(forwarded, [])
        self.assertEqual(output, "@@error child.py stopped (exit status 3221225477): no output\n")


if __name__ == "__main__":
    unittest.main()
