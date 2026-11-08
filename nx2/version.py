"""Which Hookshot a build is: the release it came from and a fingerprint of its sources.

The game prints it as the first line of hookshot_log.txt and in the Insert menu, so a report says what was run.

    python -I nx2/version.py            prints it for this folder

The release name comes from, in order: HOOKSHOT_VERSION; a VERSION file at the root (written when a release is packaged,
since the builder's tools folder has no git history); `git describe --tags --always --dirty` in a checkout, matching
only version tags (`v0.1.0`, not the branch-like tags from before the scheme). The
fingerprint is a hash of the sources in this folder (host/, gpu/, nx2/), line endings aside: every build from the
same release zip gives the same one, so even a build that says "unknown" can be traced to the zip it came from. (A
checkout and a release's tools folder hold different sets of files, so their fingerprints differ.)
"""
import hashlib
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ("host", "gpu", "nx2")
SOURCE_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".py", ".glsl", ".comp", ".vert", ".frag", ".spv")


def release(root=ROOT):
    name = os.environ.get("HOOKSHOT_VERSION", "").strip()
    if not name:
        for file in ("VERSION", "version.txt"):   # (version.txt: the release's tag, written into the builder's tools when it is packed)
            try:
                with open(os.path.join(root, file), encoding="utf-8") as f:
                    name = f.readline().strip()
            except OSError:
                pass
            if name:
                break
    if not name and os.path.exists(os.path.join(root, ".git")):
        try:
            r = subprocess.run(["git", "-C", root, "describe", "--tags", "--always", "--dirty", "--match", "v[0-9]*"],
                               capture_output=True, text=True, timeout=20)
            name = r.stdout.strip() if r.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError):
            pass
    return name or "unknown"


def fingerprint(root=ROOT):
    h = hashlib.sha1()
    for top in SOURCES:
        base = os.path.join(root, top)
        for d, dirs, files in os.walk(base):
            dirs[:] = sorted(x for x in dirs if x != "__pycache__")
            for f in sorted(files):
                if not f.endswith(SOURCE_EXT):
                    continue
                p = os.path.join(d, f)
                with open(p, "rb") as src:
                    data = src.read().replace(b"\r\n", b"\n")
                h.update(os.path.relpath(p, root).replace("\\", "/").encode() + b"\0" + data + b"\0")
    return h.hexdigest()[:8]


def describe(root=ROOT):
    """'movie-sync (source 1a2b3c4d)': safe to pass as a C string literal on any command line."""
    text = "%s (source %s)" % (release(root), fingerprint(root))
    return re.sub(r"[^A-Za-z0-9 ._+()/-]", "_", text)[:80]


def define(root=ROOT):
    return '-DHOOKSHOT_VERSION="%s"' % describe(root)


if __name__ == "__main__":
    print(describe())
