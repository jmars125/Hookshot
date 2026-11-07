"""A held rumble is sent again before SDL's one-second effect ends, including to a controller that just opened.

SDL_GameControllerRumble stops when its duration ends. The poll used to call it only when the strength
changed, and it remembered that change even when no controller was open yet.

Run with: python -I -B tests/test_rumble_refresh.py
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "host" / "pad_sdl.c"
HALF = 500000000


def function(name):
    text = SOURCE.read_text(encoding="utf-8")
    at = text.index(name)
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise RuntimeError("no end for " + name)


class RumbleRefreshTests(unittest.TestCase):
    def test_each_controller_is_sent_its_own_rumble(self):
        poll = function("static void pads_poll")
        self.assertIn("rumble_due(", poll)
        self.assertNotIn("last_lo", poll)
        self.assertIn("pads[i].rumble_ns = now", poll)

    def test_a_held_rumble_is_sent_again_before_it_lapses(self):
        src = function("static int rumble_due")
        self.assertIn("500000000LL", src)
        fixture = r"""
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %%d %%s\n", __LINE__, #x); return 1; } } while (0)
%s
int main(void) {
    CHECK(rumble_due(1000, 0, 0, 0, 1000, 0) == 1);                 /* a controller that has not been sent this yet */
    CHECK(rumble_due(1000, 0, 1000, 0, 1000, 1000) == 0);           /* the same strength, just sent */
    CHECK(rumble_due(1000, 0, 1000, 0, 1000 + %dLL - 1, 1000) == 0);
    CHECK(rumble_due(1000, 0, 1000, 0, 1000 + %dLL, 1000) == 1);    /* still wanted, and the effect would be ending */
    CHECK(rumble_due(0, 0, 0, 0, 1000 + %dLL, 1000) == 0);          /* silence is not sent again */
    CHECK(rumble_due(0, 0, 1000, 200, 1000, 0) == 1);               /* a stop */
    return 0;
}
""" % (src, HALF, HALF, HALF)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "rumble.c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(fixture)
            exe = os.path.join(d, "rumble")
            cc = os.environ.get("CC", "cc")
            made = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", path, "-o", exe],
                                  capture_output=True, text=True)
            self.assertEqual(made.returncode, 0, made.stderr)
            ran = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
