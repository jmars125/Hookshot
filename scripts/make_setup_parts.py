"""The parts HookshotBuilder-Setup.exe downloads (builder/setup/setup.c), from a Hookshot Builder package folder.

    python -I scripts/make_setup_parts.py <package folder> <tag> <out folder> [--base <url the parts are served from>]

A release is split by how often each part changes, so an update downloads only what changed: the builder's own code (a few MB)
in every release, the .NET runtime, Python, emsdk, the CPM packages and third_party only when they change. llvm-mingw comes from
its official release. Each zip is deterministic (sorted names, fixed dates and attributes): a part whose files did not change
has the same SHA-256 in the next release, and the setup takes it from its download cache instead of downloading it again.

Writes <out>/hookshot-setup.txt and one HookshotBuilder-<part>-<sha256 prefix>.zip per part (upload the ones the builder-parts
release lacks). Scripts the build does not use (.ps1, .bat, .js,
.html: antivirus machine-learning guesses fire on them) are left out.
"""
import argparse
import hashlib
import os
import sys
import zipfile

SKIP_EXT = {".ps1", ".psm1", ".bat", ".cmd", ".js", ".vbs", ".html", ".htm", ".hta", ".wsf", ".pyc"}
SKIP_DIRS = {"__pycache__"}
TOOLS_PARTS = ("python", "emsdk", "cpm", "third_party", "wgpu-native")   # (tools/<name>: each its own part)
LLVM = ("552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f", 191037046,
        "llvm-mingw-20261104-ucrt-x86_64",
        "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip")


def files_under(root, top):
    out = []
    for d, ds, fs in os.walk(os.path.join(root, top)):
        ds[:] = sorted(x for x in ds if x not in SKIP_DIRS)
        for f in fs:
            if os.path.splitext(f)[1].lower() not in SKIP_EXT:
                out.append(os.path.relpath(os.path.join(d, f), root).replace(os.sep, "/"))
    return out


def write_zip(path, root, names):
    """Deterministic: the same files give the same bytes (sorted, a fixed date, fixed attributes)"""
    with zipfile.ZipFile(path + ".new", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for n in sorted(names):
            info = zipfile.ZipInfo("HookshotBuilder/" + n, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(root, n.replace("/", os.sep)), "rb") as f:
                z.writestr(info, f.read(), compresslevel=6)
    os.replace(path + ".new", path)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest(), os.path.getsize(path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("package")
    p.add_argument("tag")
    p.add_argument("out")
    p.add_argument("--base", help="where the parts are served from (default: this repository's builder-parts release)")
    a = p.parse_args()
    root = os.path.abspath(a.package)
    if not os.path.isfile(os.path.join(root, "HookshotBuilder.exe")):
        sys.exit("not a Hookshot Builder package folder: %s" % root)
    # (the parts live in one release of their own, named by their checksum: a part that did not change is not uploaded again, and a
    # release itself holds only HookshotBuilder.exe (the setup) and the list it reads)
    base = (a.base or "https://github.com/NotRexed/Hookshot/releases/download/builder-parts").rstrip("/")
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(root, "tools", "version.txt"), "w", newline="\n") as f:
        f.write(a.tag + "\n")

    top = sorted(os.listdir(root))
    builder_files = [n for n in top if os.path.isfile(os.path.join(root, n)) and
                     (n.startswith("HookshotBuilder") or n in ("LICENSE", "README.txt"))]   # (HookshotBuilder-Setup.exe too: Repair runs it)
    runtime_files = [n for n in top if os.path.isfile(os.path.join(root, n)) and n not in builder_files]
    runtime_dirs = [n for n in top if os.path.isdir(os.path.join(root, n)) and n != "tools"]
    tools = sorted(os.listdir(os.path.join(root, "tools")))
    parts = {
        "app": builder_files + [n for t in tools if t not in TOOLS_PARTS and t != "llvm-mingw"
                                for n in (["tools/" + t] if os.path.isfile(os.path.join(root, "tools", t)) else files_under(root, "tools/" + t))],
        "dotnet": runtime_files + [n for d in runtime_dirs for n in files_under(root, d)],
    }
    for t in TOOLS_PARTS:
        if os.path.isdir(os.path.join(root, "tools", t)):
            parts[t] = files_under(root, "tools/" + t)
    lines = ["tag %s" % a.tag]
    for name in ("app", "dotnet") + TOOLS_PARTS:
        if not parts.get(name):
            continue
        zp = os.path.join(a.out, "HookshotBuilder-%s.zip" % name)
        sha, size = write_zip(zp, root, parts[name])
        named = os.path.join(a.out, "HookshotBuilder-%s-%s.zip" % (name, sha[:12]))
        os.replace(zp, named)
        zp = named
        lines.append("part %s %d . - - %s/%s" % (sha, size, base, os.path.basename(zp)))
        print("%-12s %5d files %5d MB  %s" % (name, len(parts[name]), size >> 20, sha[:16]), flush=True)
    lines.append("part %s %d HookshotBuilder/tools %s llvm-mingw %s" % LLVM)
    lines.append("run HookshotBuilder/HookshotBuilder.exe")
    with open(os.path.join(a.out, "hookshot-setup.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
