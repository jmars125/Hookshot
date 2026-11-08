"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
- Text speed (the Insert menu's "Text speed"): f_80b1e140 (main+0xafe140) steps a text pane's letter animation, the
  LetterAnimControl of the panes with "LetterAnimOn" user data (dialogue, Navi, signs). Its time argument (s0) moves the
  letters' clock (+0x60 += rate +0x4c * time, the rate from the message's speed tag) and the pauses' timer (+0x58 -= time);
  hos_text_scale multiplies it, 1.0 as shipped. The auto-advance timer (+0x5c, before) keeps the game's time. A huge
  scale shows a page at once (one frame per pause), as the game's own skip on A (+0x72 = 2, f_80794b00) without its flags.
"""
import glob
import os
import sys

DECL = "extern volatile int hos_fixed_resolution;\nextern volatile int hos_dynres_runs;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
OLD = "if (!((c->x[8] >> 0) & 1)) { goto L_8082640c; }"
NEW = "if (hos_fixed_resolution || !((c->x[8] >> 0) & 1)) { goto L_8082640c; }\n    hos_dynres_runs++;"
LABEL = "L_8082640c:;"
SCALE_OLD = "c->x[9] = 0x3f800000ULL;"
SCALE_NEW = "c->x[9] = hos_fixed_scale_bits;"   # (the fixed path's scale, 1.0 as shipped)

TEXT_DECL = "extern volatile float hos_text_scale;\n"
TEXT_HEAD = "void f_80b1e140(Ctx *__restrict A64_NOESC c) {"
# (the letters' block: past the early returns, the auto-advance timer and the hidden / no-text checks; reached once a call)
TEXT_OLD = ("L_80b1e1b4:;\n"
            "    c->x[9]=recomp_load16(c,c->x[19] + 110);\n"
            "if ((uint32_t)c->x[9] == 0) { goto L_80b1e18c; }\n")
TEXT_NEW = TEXT_OLD + ("    if (hos_text_scale != 1.0f) { float _t; uint32_t _u; memcpy(&_t,c->vreg[0],4); _t *= hos_text_scale; "
                       "memcpy(&_u,&_t,4); a64_vset64(c->vreg[0],_u); }   /* (nx2/oot_patches.py: text speed) */\n")


def patch_dynres(s):
    if OLD not in s:
        return None
    s = s.replace(OLD, NEW, 1)
    at = s.index(LABEL)
    scale = s.index(SCALE_OLD, at)
    s = s[:scale] + SCALE_NEW + s[scale + len(SCALE_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)


def patch_text_speed(s):
    """The scale at the head of the letters' block of f_80b1e140; None when the file has no such function, s unchanged when the
    function is there but its code differs (another game version)."""
    head = s.find(TEXT_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(TEXT_HEAD))
    body = s[head:end if end >= 0 else len(s)]
    if body.count(TEXT_OLD) != 1 or body.count("goto L_80b1e1b4;") != 1:
        return s
    at = head + body.index(TEXT_OLD)
    s = s[:at] + TEXT_NEW + s[at + len(TEXT_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + TEXT_DECL, 1)


def apply(gen):
    done = {"dynamic resolution branch": 0, "text speed block": 0}
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out = s
        d = patch_dynres(out)
        if d is not None:
            out = d
            done["dynamic resolution branch"] += 1
            print("oot_patches: dynamic resolution switch in", os.path.basename(path))
        t = patch_text_speed(out)
        if t is not None and t != out:
            out = t
            done["text speed block"] += 1
            print("oot_patches: text speed scale in", os.path.basename(path))
        if out != s:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(out)
    for what, n in done.items():
        if n != 1:
            print("oot_patches: WARNING: the %s was found %d times" % (what, n))


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
