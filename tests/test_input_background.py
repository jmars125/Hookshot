"""The controllers keep playing behind other windows only when the player asks for it.

Controllers counted only while the game's window was in front, so a game streamed from a screen of its own (Sunshine
and Moonlight), or left running behind another window, ignored the pad. The Insert menu's "Controller in background"
row, or HOS_INPUT_BACKGROUND=1, lets the pads through; the keyboard and mouse stay with the window in front.

python -I -B tests/test_input_background.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")

    read = block(hid, "static void read_input(")
    behind = read[read.index("if (!game_in_front())"):]
    behind = behind[:behind.index("return;") + len("return;")]
    if "pads_in_background(" not in behind or "pads_read(" not in behind:
        raise SystemExit("behind another window the pads are never read")
    if "key(" in behind or "mouse" in behind:
        raise SystemExit("behind another window the keyboard or mouse would reach the game")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    if "id == 4 ? opts.pad_background" not in option:
        raise SystemExit("the host cannot see the menu's row (option 4)")
    if '"Controller in background", &opts.pad_background' not in menu:
        raise SystemExit("the Insert menu has no row for it")

    fn = block(hid, "static int pads_in_background(")
    fixture = r'''#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
''' + fn + r'''
int main(void) {
    CHECK(!pads_in_background(NULL, 0));   /* out of the box: the window in front only */
    CHECK(pads_in_background(NULL, 1));    /* the menu's row */
    CHECK(pads_in_background("1", 0));     /* HOS_INPUT_BACKGROUND=1 without the menu */
    CHECK(!pads_in_background("0", 1));    /* HOS_INPUT_BACKGROUND=0 wins over the menu */
    printf("PASS: pads behind other windows only when asked\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="input-background-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
