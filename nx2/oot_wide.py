"""Widescreen and ultrawide for Ocarina of Time 1.1.1: C added after a few instructions of the game (a64_lift.py Lifter.after).

The game draws a 2560x1440 picture whatever the screen and renders its scene at 1920x1080; its cameras use a 16:9 ratio.
With these patches the picture takes the window's shape (the height stays the game's, the width grows: Hor+), the 16:9
cameras take the picture's ratio, the HUD and menus keep their 16:9 shape in the middle of the picture, and pre-rendered
movies play in that middle 16:9. Every patch reads host globals (host/hos_wide.c) that stay 0 for a 16:9 window, so the
game runs as shipped then.

The sites and what each does come from NX2navi's recomp/patches-v111.txt, where they are credited to Ivan0xFF (MIT,
"The matrix-update, UI and movie sites: Copyright (c) 2026 Ivan0xFF"); the picture and scene size sites are the same
constants that file names. Here each is written as C after the original instruction instead of a replacement of it.
Each site's original instruction word is checked: another game version leaves the site out (and says so).
"""

MAIN = 0x80020000   # (main's base in this image: nx2/eden_image.py)
UI_M00, UI_M11 = "(2.0f / 3840.0f)", "(2.0f / 2160.0f)"   # (the UI's full-screen orthographic projection, 3840x2160 grid)
# (the screens whose UI keeps the whole picture: the item-wheel and pause / system menu backgrounds redraw a capture of
# the whole picture; the event and loading fades cover it)
KEEP_VTABLES = (MAIN + 0x4bed098, MAIN + 0x4be4fb0, MAIN + 0x4be61c8, MAIN + 0x4be7b30)


def _ratio(vreg, base=None):
    """A ratio just loaded into s<vreg>: when it is 16:9 (within 0.01), the picture's; with base, also stored back at +0xb0
    (sead's perspective projection keeps it there, and culling reads it)."""
    store = ("recomp_store32(c, c->x[%d] + 0xb0, _u); " % base) if base is not None else ""
    return ("{ extern volatile float hos_wide_ratio; const float _w = hos_wide_ratio; if (_w > 0.0f) { float _r; "
            "memcpy(&_r, c->vreg[%d], 4); const float _d = _r - (16.0f / 9.0f); if (_d <= 0.01f && _d >= -0.01f) { "
            "uint32_t _u; memcpy(&_u, &_w, 4); a64_vset64(c->vreg[%d], _u); %s} } }   /* (nx2/oot_wide.py: camera ratio) */"
            % (vreg, vreg, store))


def _size(name):
    return ("{ extern volatile uint64_t %s; const uint64_t _s = %s; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: %s) */"
            % (name, name, name))


HUD = ("{ extern volatile float hos_wide_squeeze; const float _q = hos_wide_squeeze; if (_q > 0.0f) { float _m00, _m11; "
       "memcpy(&_m00, c->vreg[1], 4); memcpy(&_m11, (const uint8_t *)c->vreg[4] + 4, 4); "
       "if (_m00 == %s && _m11 == %s) { const uint64_t _vt = recomp_load64(c, c->x[0] - 0x1b8); "
       "if (%s) { float _row[4]; memcpy(_row, c->vreg[1], 16); for (int _k = 0; _k < 4; _k++) _row[_k] *= _q; "
       "memcpy(c->vreg[1], _row, 16); } } } }   /* (nx2/oot_wide.py: HUD in the middle 16:9) */"
       % (UI_M00, UI_M11, " && ".join("_vt != 0x%xULL" % v for v in KEEP_VTABLES)))

MOVIE = ("{ extern volatile float hos_wide_margin; const float _m = hos_wide_margin; if (_m > 0.0f) { "
         "const uint64_t _vp = c->x[29] - 40; uint32_t _ua = (uint32_t)recomp_load32(c, _vp + 8), _ub = (uint32_t)recomp_load32(c, _vp + 16); "
         "float _a, _b; memcpy(&_a, &_ua, 4); memcpy(&_b, &_ub, 4); const float _in = (_b - _a) * _m; _a += _in; _b -= _in; "
         "memcpy(&_ua, &_a, 4); memcpy(&_ub, &_b, 4); recomp_store32(c, _vp + 8, _ua); recomp_store32(c, _vp + 16, _ub); } }"
         "   /* (nx2/oot_wide.py: movies in the middle 16:9) */")

# (main offset, the original instruction word there, the C after it)
SITES = (
    # the picture: the graphics setup and the display settings object build {2560, 1440} in x8 (mov, then this movk)
    (0x1202ee4, 0xf2c0b408, _size("hos_wide_out")),
    (0x31f39f8, 0xf2c0b408, _size("hos_wide_out")),
    # the scene the game's upscaler takes in, {1920, 1080}
    (0x31f3a04, 0xf2c08708, _size("hos_wide_scene")),
    # the camera update copies the camera's ratio (ldr s0, [x19, #0x140]) into the projection the renderer reads
    (0x4c5334, 0xbd414260, _ratio(0)),
    # sead's perspective projection's matrix update (ldp s0, s1, [x0, #0xac]) and its three inlined copies
    (0x3485cc0, 0x2d558400, _ratio(1, 0)),
    (0x4f1e8, 0x2d558ea0, _ratio(3, 21)),
    (0x23b9a8, 0x2d558c00, _ratio(3, 0)),
    (0x33b82c, 0x2d558440, _ratio(1, 2)),
    # the 2D canvas setup: the layout's projection rows (ldp q1, q4, [x2]); the canvas is x0, its screen 0x1b8 before it
    (0x774368, 0xad401041, HUD),
    # the movie layer's draw: its viewport (min x at x29 - 32, max x at x29 - 24) after ldp x9, x8, [x19, #0x18]
    (0x3b5c758, 0xa941a269, MOVIE),
)


def install(lifter, img):
    """The sites into lifter.after; a site whose word differs (another game version) is left out"""
    n = 0
    for off, word, text in SITES:
        pc = MAIN + off
        try:
            have = img.word(pc)
        except Exception:
            have = None
        if have != word:
            print("widescreen: main+%#x holds %s, not %08x: left out" % (off, "?" if have is None else "%08x" % have, word))
            continue
        lifter.after[pc] = text
        n += 1
    print("widescreen: %d of %d sites" % (n, len(SITES)))
    return n
