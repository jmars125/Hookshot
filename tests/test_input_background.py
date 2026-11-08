"""Exercise real scripted parsing, routing and Npad serialization with authored files/API spies.

The parser, LIVE reader, input routing and Npad writer are extracted unmodified from production. Clock,
foreground-window, keyboard, mouse and pad APIs are fakes; no real device, desktop or game is accessed.
Each case starts a fresh process because production caches environment settings.

Controllers counted only while the game's window was in front, so a game streamed from a screen of its own (Sunshine
and Moonlight), or left running behind another window, ignored the pad. The Insert menu's "Controller in background"
row, or HOS_INPUT_BACKGROUND=1, lets the pads through; the keyboard and mouse stay with the window in front.

python -I -B tests/test_input_background.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ("foreground", "script-background", "headless", "noactivate", "live-background",
         "no-script-background", "exclusive-foreground", "exclusive-background", "pads-env", "pads-menu", "pads-off")


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
    p.add_argument("--case", choices=CASES)
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")

    read = block(hid, "static void read_input(")
    behind = read[read.index("if (!game_in_front()"):]   # (with or without PR 545's scripted-background test)
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

    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    constants = block(hid, "enum { B_A") + ";\n" + block(api, "enum { HOS_SW_A") + ";\n"
    for name in ("STATE_SIZE", "NPAD_NO1", "ATTR_CONNECTED"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", hid, re.M)
        assert len(matches) == 1, name
        constants += matches[0] + "\n"
    start = hid.index("typedef struct { int64_t start_ms, end_ms;")
    script = block(hid, "static void scripted_input(")
    end = hid.index(script) + len(script)
    actual = "\n".join((block(hid, "static int key("), block(hid, "static int kmap_down("),
        block(hid, "static int game_in_front("), hid[start:end], block(hid, "static void stick_max("),
        block(hid, "static int pads_in_background("), read, block(hid, "static void npad_states(")))
    glue = (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8")
    actual += "\n" + "\n".join(block(glue, "uint64_t " + name + "(")
                                  for name in ("hos_title_input", "hos_frame_step_buttons"))
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("HOS_INPUT_") or name in ("HOS_HEADLESS", "HOS_WINDOW_NOACTIVATE"):
            del env[name]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    def run(command, environment=None):
        result = subprocess.run(command, env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", creationflags=flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
    with tempfile.TemporaryDirectory(prefix="input-background-") as tmp:
        folder = Path(tmp).resolve()
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")
        (folder / "input_constants.inc").write_text(constants, encoding="utf-8")
        (folder / "input_actual.inc").write_text(actual, encoding="utf-8")
        exe = folder / "fixture.exe"
        # Production's button table deliberately leaves its unused axis fields zero-initialized.
        run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
             "-D_CRT_SECURE_NO_WARNINGS",
             "-I" + str(folder), str(ROOT / "tests/input_background_test.c"), "-o", str(exe)])
        for case in ((a.case,) if a.case else CASES):
            case_env = dict(env)
            if case not in ("live-background", "no-script-background"):
                case_env["HOS_INPUT_SCRIPT"] = "0:A+LUP/200"
            if case.startswith("exclusive-"):
                case_env["HOS_INPUT_EXCLUSIVE"] = "1"
            if case in ("pads-env", "pads-off"):
                case_env["HOS_INPUT_BACKGROUND"] = "1" if case == "pads-env" else "0"
            if case == "headless":
                case_env["HOS_HEADLESS"] = "1"
            if case == "noactivate":
                case_env["HOS_WINDOW_NOACTIVATE"] = "1"
            if case == "live-background":
                live = folder / "live.txt"
                live.write_bytes(b"")
                case_env["HOS_INPUT_LIVE"] = str(live)
            run([str(exe), case], case_env)
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")


if __name__ == "__main__":
    main()
