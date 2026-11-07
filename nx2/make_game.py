"""A game folder from the user's own dump: what the builder app (builder/) runs.

    python -I nx2/make_game.py --nup <file.nup> --out <folder> [--work <folder>] [--jobs <n>]
    python -I nx2/make_game.py --romfs <dir> --exefs <dir> --out <folder> [--work <folder>] [--jobs <n>]
    python -I nx2/make_game.py --check --nup <file.nup>       (or --exefs <dir>: is this the right game and version?)

The folder holds the executable and all it needs, started with a double click: nothing goes back into the repository, and no game
data ever comes with the tools. Lines the app reads, each on its own line of stdout:

    @@stage <id> <text>      a step begins
    @@progress <0..1>        how far that step is
    @@check <ok|bad> <text>  (--check) the answer
    @@done <folder>          finished
    @@error <text>           stopped
"""
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = [sys.executable, "-I", "-u"]   # (-u: the steps' output as it comes, not in 8 KB blocks minutes apart)
BUILD_ID = "e0f3ccfa"   # (the version the patches, the hot list and the profile were made from)
LINUX = os.name != "nt"
BESIDE = ["hookshot", "hookshot_vk.so", "libSDL2-2.0.so.0"] if LINUX else ["hookshot.exe", "hookshot_vk.dll", "SDL2.dll", "wgpu_native.dll"]
LANGUAGES = [("en-US", "English (US)"), ("en-GB", "English (UK)"), ("fr", "French (France)"), ("fr-CA", "French (Canada)"), ("de", "German"),
             ("it", "Italian"), ("es", "Spanish (Spain)"), ("es-419", "Spanish (Latin America)"), ("pt-BR", "Portuguese (Brazil)"), ("nl", "Dutch"),
             ("pl", "Polish"), ("ru", "Russian"), ("ja", "Japanese"), ("ko", "Korean"), ("zh-Hans", "Chinese (Simplified)"),
             ("zh-Hant", "Chinese (Traditional)")]


def no_throttle(handle=None):
    """(issue 87) Windows' power throttling (EcoQoS) slows a background window's processes to almost nothing on some machines: the
    build sat at 0% CPU. Turned off for this process, or the one whose handle this is"""
    if os.name != "nt":
        return
    import ctypes
    class State(ctypes.Structure):
        _fields_ = [("Version", ctypes.c_ulong), ("ControlMask", ctypes.c_ulong), ("StateMask", ctypes.c_ulong)]
    k = ctypes.WinDLL("kernel32")
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.SetProcessInformation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]
    s = State(1, 1, 0)   # (PROCESS_POWER_THROTTLING_EXECUTION_SPEED in the mask, off in the state)
    try:
        k.SetProcessInformation(ctypes.c_void_p(int(handle)) if handle else k.GetCurrentProcess(), 4, ctypes.byref(s), ctypes.sizeof(s))
    except (OSError, ValueError, TypeError):
        pass


def say(*words):
    print("@@" + " ".join(str(w) for w in words), flush=True)


def fail(text):
    say("error", text)
    sys.exit(1)


def run(cmd, env=None, on_line=None):
    e = dict(os.environ, PYTHONIOENCODING="utf-8")
    e.update(env or {})
    p = subprocess.Popen(cmd, cwd=ROOT, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                         errors="replace", bufsize=1)
    no_throttle(getattr(p, "_handle", None))
    tail = []
    for line in p.stdout:
        line = line.rstrip()
        print(line, flush=True)
        tail = (tail + [line])[-12:]
        if on_line:
            on_line(line)
    if p.wait():
        why = [l for l in tail if "error:" in l.lower() and "CalledProcessError" not in l]
        step = next((os.path.basename(c) for c in cmd[1:] if c.endswith(".py")), os.path.basename(cmd[-1]))   # (issue 154: not "-u")
        fail("%s stopped: %s" % (step, why[-1] if why else tail[-1] if tail else "no output"))


def build_id(main):
    """the module id of an NSO, plain or compressed: the header is the same"""
    with open(main, "rb") as f:
        head = f.read(0x60)
    if head[:4] != b"NSO0":
        fail("%s is not a game executable" % main)
    return head[0x40:0x60].hex()


def check_modules(exefs):
    """Every module of the exefs is one the tools can read: said here, not as a Python error from the translator"""
    for name in os.listdir(exefs):
        if name != "main.npdm" and not name.endswith(".part"):
            with open(os.path.join(exefs, name), "rb") as f:
                head = f.read(16)
            if head[:4] != b"NSO0":
                fail("The file %s in this dump is not a game module (it starts %s). Try another dump of the game, or send this message to the "
                     "Hookshot developers." % (name, head.hex()))


def check_supported_build(exefs):
    bid = build_id(os.path.join(exefs, "main"))
    if not bid.startswith(BUILD_ID):
        fail("This is build %s. Hookshot supports build %s." % (bid[:8], BUILD_ID))


def check_cache_paths(out, paths):
    """Resolve generated paths and reject symbolic links/reparse points before replacement."""
    root = os.path.realpath(out)
    for path in paths:
        try:
            inside = os.path.commonpath((root, os.path.realpath(path))) == root
        except ValueError:
            inside = False
        attrs = os.lstat(path) if os.path.lexists(path) else None
        linked = os.path.islink(path) or bool(
            getattr(attrs, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))
        if not inside or linked:
            fail("The generated cache path %s is not a normal path inside the build folder (symbolic links and reparse points are not supported). "
                 "Choose a different output folder." % path)


def check(a, work):
    check_windows_sdk()   # (first: said in plain words when the game is chosen, not after it has been read)
    if a.nup:
        probe = os.path.join(work, "probe")
        shutil.rmtree(probe, ignore_errors=True)
        run(PY + ["nx2/nup.py", a.nup, "--extract", probe, "--only", "exefs"])
        check_modules(os.path.join(probe, "exefs"))
        bid = build_id(os.path.join(probe, "exefs", "main"))
        shutil.rmtree(probe, ignore_errors=True)
    else:
        bid = build_id(os.path.join(a.exefs, "main"))
    if bid.startswith(BUILD_ID):
        say("check", "ok", "This is the version Hookshot supports (build %s)." % bid[:8])
    else:
        say("check", "bad", "This is build %s. Hookshot supports build %s." % (bid[:8], BUILD_ID))


def visual_studio_environment():
    """What Visual Studio's own vcvars64.bat sets, for a compiler that does not find it by itself; None without one"""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    r = subprocess.run([vswhere, "-latest", "-prerelease", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property", "installationPath"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = r.stdout.strip().splitlines()
    if not lines:
        return None
    bat = os.path.join(lines[-1], "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not os.path.exists(bat):
        return None
    r = subprocess.run('"%s" >nul && set' % bat, shell=True, capture_output=True, text=True, encoding="mbcs", errors="replace")
    keep = ("INCLUDE", "LIB", "LIBPATH", "PATH", "VCINSTALLDIR", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION", "WINDOWSSDKDIR", "WINDOWSSDKVERSION",
            "UCRTVERSION", "UNIVERSALCRTSDKDIR")
    found = {}
    for line in r.stdout.splitlines():
        name, eq, value = line.partition("=")
        if eq and name.upper() in keep:
            found[name] = value
    return found if any(k.upper() == "INCLUDE" for k in found) else None


def windows_kits_environment(base):
    """INCLUDE and LIB with the Windows SDK's C library (ucrt) and headers added from where its installer puts them: Visual Studio's
    vcvars leaves them out when the SDK came apart from it (issue 89: 'stdio.h' file not found with MSVC installed). None without one"""
    kits = os.path.join(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"), "Windows Kits", "10")
    inc = os.path.join(kits, "Include")
    try:
        versions = sorted((v for v in os.listdir(inc) if os.path.exists(os.path.join(inc, v, "ucrt", "stdio.h"))),
                          key=lambda v: [int(x) if x.isdigit() else 0 for x in v.split(".")])
    except OSError:
        return None
    if not versions:
        return None
    v = versions[-1]
    env = dict(base or {})
    get = lambda k: next((env[n] for n in env if n.upper() == k), os.environ.get(k, ""))
    env["INCLUDE"] = ";".join(x for x in [get("INCLUDE")] + [os.path.join(inc, v, d) for d in ("ucrt", "um", "shared", "winrt")] if x)
    env["LIB"] = ";".join(x for x in [get("LIB")] + [os.path.join(kits, "Lib", v, d, "x64") for d in ("ucrt", "um")] if x)
    print("the Windows SDK %s from %s" % (v, kits), flush=True)
    return env


def compile_test(toolchain):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        with open(src, "w") as f:
            # (_UCRT: the C library of Visual Studio 2015 and later. An older one's stdio.h compiled, and the link failed 40 minutes later
            # on __iob_func: issue 80)
            f.write(chr(10).join(("#include <stdio.h>", "#ifndef _UCRT", "#error \"stdio.h is from an old C library (before Visual Studio 2015)\"",
                                  "#endif", "#include <windows.h>", "int main(void) { return 0; }", "")))
        return subprocess.run([toolchain.need(toolchain.find("clang"))] + toolchain.native_target() + ["-c", src, "-o", os.path.join(d, "t.o")],
                              capture_output=True, text=True, encoding="utf-8", errors="replace")


def check_cpp_library(toolchain):
    """The renderer needs C++20's formatting from Visual Studio's library: said now, not after 300 failed files (issue 39)"""
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.cpp")
        with open(src, "w") as f:
            f.write(chr(10).join(("#include <format>", "#include <string>",
                                  "std::string t(int v) { return std::vformat(\"{}\", std::make_format_args(v)); }", "")))
        r = subprocess.run([toolchain.need(toolchain.find("clang++"))] + toolchain.native_target() + ["-std=c++20", "-c", src, "-o", os.path.join(d, "t.o")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        print(r.stdout + r.stderr, flush=True)
        fail("Your Visual Studio is too old for this game's renderer. Open the Visual Studio Installer, press Update on "
             "Visual Studio 2022 (or install it), then build again.")


def check_linux_compiler():
    """(linux) A C compiler that builds and links a program here (the AppImage's own clang and sysroot, else the system's):
    found now, not 40 minutes in"""
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import toolchain
    cc = toolchain.find("clang")
    if not os.path.isfile(cc):
        fail("No C compiler was found. Install clang (and lld), or use the Hookshot AppImage, which brings its own.")
    sysroot = os.environ.get("HOOKSHOT_SYSROOT")
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        with open(src, "w") as f:
            f.write(chr(10).join(("#include <stdio.h>", "#include <pthread.h>", "int main(void) { return 0; }", "")))
        r = subprocess.run([cc] + (["--sysroot=" + sysroot] if sysroot else []) + ["-fuse-ld=lld", src, "-o", os.path.join(d, "t")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        print(r.stdout + r.stderr, flush=True)
        errors = [l for l in r.stderr.splitlines() if "error" in l]
        fail("The compiler could not build a test program: %s (install clang, lld and your system's C library headers: "
             "libc6-dev on Debian and Ubuntu, glibc-devel on Fedora)" % (errors[0] if errors else "no message"))


def check_windows_sdk():
    """The compiler is bundled; Microsoft's C library and Windows SDK may not be, by their licences. Found now, not 40 minutes in."""
    if LINUX:
        return check_linux_compiler()
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import toolchain
    # (an INCLUDE set for the whole machine, by an old Visual Studio or another tool, is all clang reads: without the Windows SDK's C
    # library in it, it is dropped and the compiler looks for Visual Studio itself. Issues 80 and 89)
    for k in [n for n in os.environ if n.upper() == "INCLUDE"]:
        if "ucrt" not in os.environ[k].lower():
            print("INCLUDE is set on this PC without the Windows SDK's C library: not used (%s)" % os.environ[k][:200], flush=True)
            for n in [n for n in os.environ if n.upper() in ("INCLUDE", "LIB", "LIBPATH")]:
                del os.environ[n]
            break
    r = compile_test(toolchain)
    found = None
    if r.returncode:   # (clang looks for Visual Studio its own way, and does not always find it: then ask Visual Studio)
        found = visual_studio_environment()
        if found:
            print("the compiler did not find Visual Studio by itself: using vcvars64", flush=True)
            os.environ.update(found)
            r = compile_test(toolchain)
    if r.returncode and found and ("stdio.h" in r.stderr or "windows.h" in r.stderr.lower()):
        # (only on top of vcvars' INCLUDE: clang takes INCLUDE as all of its system headers once it is set)
        kits = windows_kits_environment(found)
        if kits:
            for k in [n for n in os.environ if n.upper() in ("INCLUDE", "LIB")]:
                del os.environ[k]
            os.environ.update(kits)
            r = compile_test(toolchain)
    if not r.returncode:
        check_cpp_library(toolchain)
        return
    print(r.stdout + r.stderr, flush=True)   # (all of it, for Show details)
    errors = [l for l in r.stderr.splitlines() if "error:" in l]
    why = errors[0].split("error:", 1)[1].strip() if errors else (r.stderr.strip().splitlines() or ["no message"])[-1]
    if "old c library" in why.lower():
        fail("The compiler found an old Visual Studio's C library (from before 2015) instead of the Windows SDK's. In the Visual Studio "
             "Installer choose Modify on Visual Studio 2022, tick \"Desktop development with C++\" with a Windows 11 SDK, then build again. (%s)" % why)
    if "windows.h" in why.lower() or "stdio.h" in why.lower():
        fail("The Windows SDK was not found. In the Visual Studio Installer choose Modify, and under \"Desktop development with C++\" tick a "
             "Windows 11 SDK, then build again. (%s)" % why)
    if "file not found" in why:
        fail("Visual Studio's C++ tools were not found. In the Visual Studio Installer choose Modify, tick \"Desktop development with C++\" "
             "and its MSVC x64/x86 build tools, then build again. (%s)" % why)
    fail("The compiler could not run a test build: %s" % why)


def profile_readable(prof):
    """(linux) the profile is made by a recent clang (the Windows tools'): an older one, the system's, cannot read it and stops the
    build. Then the game is built without it (a little slower in play), not at all"""
    if not LINUX:
        return True
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import toolchain
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        with open(src, "w") as f:
            f.write("int f(int x) { return x + 1; }" + chr(10))
        r = subprocess.run([toolchain.find("clang"), "-O2", "-Wno-everything", "-fprofile-instr-use=" + prof, "-c", src, "-o",
                            os.path.join(d, "t.o")], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        print("this compiler cannot read the play profile (%s): building without it" % (r.stderr.strip().splitlines() or ["?"])[-1],
              flush=True)
    return r.returncode == 0


def link_or_copy(src, dst):
    """the romfs by hard links where the disk allows: 26 GB not written twice"""
    files = [os.path.join(d, f) for d, _, fs in os.walk(src) for f in fs]
    for i, s in enumerate(files):
        t = os.path.join(dst, os.path.relpath(s, src))
        os.makedirs(os.path.dirname(t), exist_ok=True)
        if not os.path.exists(t):
            try:
                os.link(s, t)
            except OSError:
                shutil.copy2(s, t)
        if i % 500 == 0:
            say("progress", "%.3f" % (i / max(1, len(files))))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--nup")
    p.add_argument("--romfs")
    p.add_argument("--exefs")
    p.add_argument("--out")
    p.add_argument("--work", default=os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "hookshot-builder")
                   if LINUX else os.path.join(os.environ.get("LOCALAPPDATA", ROOT), "HookshotBuilder"))
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    p.add_argument("--language", default="en-US", help="the game's language: a code from LANGUAGES, written to language.txt")
    p.add_argument("--check", action="store_true")
    a = p.parse_args()
    os.makedirs(a.work, exist_ok=True)
    no_throttle()
    if os.name == "nt":   # (the PC kept awake while this runs: a build is 45 minutes, and sleep stopped it)
        import ctypes
        ctypes.WinDLL("kernel32").SetThreadExecutionState(0x80000000 | 0x00000001)
    if a.check:
        return check(a, a.work)
    if not a.out or not (a.nup or (a.romfs and a.exefs)):
        fail("needs --out, and --nup or --romfs with --exefs")
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work)
    os.makedirs(out, exist_ok=True)
    check_windows_sdk()
    exefs = os.path.join(work, "exefs")
    romfs = os.path.join(out, "romfs")

    say("stage", "read", "Reading your game files")
    shutil.rmtree(exefs, ignore_errors=True)
    if a.nup:
        # (what an earlier build left is not kept without a reason to trust it: builders before the compressed .nup fix wrote those files as
        # they lay, so a folder they left held broken game data, and the game crashed at start over its empty dictionaries)
        game = os.path.join(out, ".game")
        good = os.path.join(out, ".hookshot_romfs")
        kept = os.path.join(out, ".hookshot_exefs")
        check_cache_paths(out, (game, good, kept, romfs))
        # (an update of the builder over the same .nup: nothing to read again, only what the build needs. Issues 21 and 28)
        st = os.stat(a.nup)
        stamp = "unpacked by this builder: %s %d" % (os.path.basename(a.nup), st.st_size)
        same = False
        if os.path.exists(good) and os.path.isdir(romfs) and os.path.isdir(kept):
            with open(good) as f:
                same = f.read().strip() == stamp
        if same:
            print("the game was read from this .nup before: reading it again is skipped", flush=True)
            shutil.copytree(kept, exefs)
        else:
            shutil.rmtree(game, ignore_errors=True)
            run(PY + ["nx2/nup.py", a.nup, "--extract", game])
            # Keep the last usable cache until the new package has been read and validated.
            staged_exefs = os.path.join(game, "exefs")
            check_modules(staged_exefs)
            check_supported_build(staged_exefs)
            if not os.path.isdir(os.path.join(game, "romfs")):
                fail("The package did not contain a romfs folder.")
            if os.path.exists(romfs):
                shutil.rmtree(romfs)
            shutil.move(os.path.join(game, "romfs"), romfs)
            shutil.move(staged_exefs, exefs)
            shutil.rmtree(game, ignore_errors=True)
            shutil.rmtree(kept, ignore_errors=True)
            shutil.copytree(exefs, kept)
            with open(good, "w") as f:
                f.write(stamp + chr(10))
        check_modules(exefs)
    else:
        shutil.copytree(a.exefs, exefs)   # (unpacked in place: the user's copy stays as it is)
        run(PY + ["nx2/unpack.py", exefs])
        link_or_copy(a.romfs, romfs)
    check_supported_build(exefs)

    # (made once on the development PC: lists of addresses and counts, no game code)
    for name in ("hot_funcs.txt", "hot_order.txt", "hookshot_pieces.prof", "hookshot.res"):
        src = os.path.join(ROOT, "work", name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(work, name))
    os.makedirs(os.path.join(work, "image"), exist_ok=True)
    src = os.path.join(ROOT, "work", "image", "chunks.txt")   # (where the lifted code's files begin: addresses)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(work, "image", "chunks.txt"))
    env = {"OOT_WORK": work, "OOT_EXEFS": exefs, "OOT_ROMFS": romfs, "OOT_DUMP": out, "RX_RELEASE": "1", "A64_HOT_CHUNK": "1"}
    if os.path.exists(os.path.join(work, "hot_funcs.txt")):
        env["A64_HOT_FUNCS"] = os.path.join(work, "hot_funcs.txt")
    if os.path.exists(os.path.join(work, "hookshot_pieces.prof")) and profile_readable(os.path.join(work, "hookshot_pieces.prof")):
        env["RX_PGO"] = os.path.join(work, "hookshot_pieces.prof")

    say("stage", "lift", "Translating the game to PC code")
    run(PY + ["nx2/lift.py", "--standalone", "--write", "--exefs", exefs, "--work", work], env)
    if os.path.isfile(os.path.join(work, "sa", "gen", "code_hot.c")):
        run(PY + ["nx2/split_hot.py", os.path.join(work, "sa", "gen")], env)   # (eight files side by side: ~6 minutes, not 79)

    say("stage", "compile", "Compiling")
    done = {"n": 0, "of": 1}

    def compiled(line):
        m = re.match(r"(\d+) files, ", line)
        if m:
            done["of"] = int(m.group(1))
        elif re.match(r"\s+\S+\s+\d+ MB\s+\d+s", line):
            done["n"] += 1
            say("progress", "%.3f" % (done["n"] / done["of"]))
    run(PY + ["nx2/build.py", "--standalone", "--jobs", str(a.jobs)], env, compiled)

    say("stage", "finish", "Putting the game folder together")
    built = os.path.join(work, "sa", "build")
    for name in BESIDE:
        if os.path.exists(os.path.join(built, name)):
            if os.path.exists(os.path.join(out, name)):   # (linux: not over a running copy's file: a new one in its place)
                os.remove(os.path.join(out, name))
            shutil.copy2(os.path.join(built, name), os.path.join(out, name))
    shutil.copy2(os.path.join(work, "sa", "image", "image.bin"), os.path.join(out, "image.bin"))
    # every shader of the game, for its first start to translate (gpu/vk/vk_shader.cpp seed_open). Without it the game still runs, compiling them
    # as it meets them: not worth stopping a finished build for
    if subprocess.call([sys.executable, "-I", os.path.join(HERE, "shader_census.py"), romfs, "--out", os.path.join(out, "hookshot_shaders.bin")],
                       stdout=sys.stderr):
        print("the shader list was not made: the game will compile its shaders as it plays", flush=True)
    language = os.path.join(out, "language.txt")   # (the same text as host/nn_applet.c writes)
    with open(language, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\n\n%s\n\n" % a.language)
        f.writelines("# %-8s %s\n" % l for l in LANGUAGES)
    say("done", out)


if __name__ == "__main__":
    # (an error nobody foresaw is said in the window, not as "The game files could not be read" or "something went wrong" with the reason
    # only in a console nobody sees: the traceback's last lines go to Show details, its last line to the message)
    try:
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")   # (-I ignores PYTHONIOENCODING: a path's letters crashed a print)
            except (AttributeError, ValueError):
                pass
        main()
    except SystemExit:
        raise
    except BaseException as e:
        import traceback
        print(traceback.format_exc(), flush=True)
        say("error", "The builder stopped on an error it did not expect: %s: %s. Please send the text under Show details to the "
            "Hookshot issues page." % (type(e).__name__, str(e)[:300]))
        sys.exit(1)
