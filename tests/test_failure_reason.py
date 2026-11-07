"""The line make_game reports when a build step stops.

A compiler error can be hundreds of lines before the step's last line. The last
dozen lines then only say that something failed, and that is what the builder shows.

Run with: python -I -B tests/test_failure_reason.py
"""
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
