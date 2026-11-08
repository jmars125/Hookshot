"""scripts/changelog.py, game-free: a Factorio entry from Conventional Commits, and the old entries kept.

    python -I -B tests/test_changelog.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "changelog.py")
DASHES = "-" * 99


def git(root, *args):
    subprocess.run(["git", "-C", root] + list(args), check=True, capture_output=True)


def commit(root, subject, body=None):
    with open(os.path.join(root, "x"), "a", encoding="utf-8") as f:
        f.write(subject + "\n")
    git(root, "add", "x")
    args = ["git", "-C", root, "commit", "-q", "-m", subject]
    if body:
        args += ["-m", body]
    subprocess.run(args, check=True, capture_output=True)


def remove(root):
    for d, _, files in os.walk(root):
        for f in files:
            os.chmod(os.path.join(d, f), 0o666)
    shutil.rmtree(root)


def main():
    if not shutil.which("git"):
        print("changelog: skipped (git is not installed)")
        return
    root = tempfile.mkdtemp(prefix="hookshot-changelog-")
    try:
        os.makedirs(os.path.join(root, "scripts"))
        shutil.copy2(SCRIPT, os.path.join(root, "scripts", "changelog.py"))
        with open(os.path.join(root, "changelog.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(DASHES + "\nVersion: 0.1.0\nDate: 07. 11. 2026\n  Features:\n    - First release.\n")
        git(root, "init", "-q")
        git(root, "config", "user.name", "test")
        git(root, "config", "user.email", "test@example.com")
        git(root, "add", ".")
        git(root, "commit", "-q", "-m", "chore: start")
        git(root, "tag", "v0.1.0")

        commit(root, "feat(renderer): add a menu")
        commit(root, "fix: stop a crash")
        commit(root, "perf: spend less time in the replay")
        commit(root, "docs: explain the build")
        commit(root, "refactor(core): split the loader")
        commit(root, "feat!: change the save format", "BREAKING CHANGE: saves from 0.1.0 are not read.")
        commit(root, "feat: another feature")
        commit(root, "fix: stop a crash")   # (the same subject twice: Factorio rejects a duplicate entry)

        script = os.path.join(root, "scripts", "changelog.py")
        subprocess.run([sys.executable, "-I", script, "0.2.0", "v0.1.0", "--date", "2026-11-07"], check=True)
        with open(os.path.join(root, "changelog.txt"), encoding="utf-8") as f:
            text = f.read()
        assert text.startswith(DASHES + "\nVersion: 0.2.0\nDate: 07. 11. 2026\n  Features:\n"), text
        assert "    - Renderer: Add a menu.\n" in text
        assert "    - Another feature.\n" in text
        assert "  Changes:\n    - Breaking: Change the save format.\n    - Core: Split the loader.\n" in text
        assert "  Bugfixes:\n    - Stop a crash.\n" in text
        assert text.count("    - Stop a crash.\n") == 1, "a repeated subject is listed once"
        assert "  Optimizations:\n    - Spend less time in the replay.\n" in text
        assert "explain the build" not in text, "docs commits do not make a release"
        assert (text.index("  Features:") < text.index("  Changes:") < text.index("  Bugfixes:")
                < text.index("  Optimizations:"))
        assert text.count("Version: 0.1.0") == 1 and text.count("Version: 0.2.0") == 1
        assert text.endswith("    - First release.\n"), "the 0.1.0 entry is kept below the new one"

        # a retried release does not add its version a second time
        subprocess.run([sys.executable, "-I", script, "0.2.0", "v0.1.0", "--date", "2026-11-07"], check=True)
        with open(os.path.join(root, "changelog.txt"), encoding="utf-8") as f:
            assert f.read() == text

        # a prerelease changes nothing: Factorio's parser takes only major.minor.patch
        subprocess.run([sys.executable, "-I", script, "0.2.0-rc.1", "v0.1.0"], check=True)
        with open(os.path.join(root, "changelog.txt"), encoding="utf-8") as f:
            assert f.read() == text

        # a release with no Conventional Commits still gets a valid entry
        git(root, "tag", "v0.2.0")
        subprocess.run([sys.executable, "-I", script, "0.2.1", "v0.2.0", "--date", "2026-11-08"], check=True)
        with open(os.path.join(root, "changelog.txt"), encoding="utf-8") as f:
            text = f.read()
        assert "Version: 0.2.1\nDate: 08. 11. 2026\n  Changes:\n    - Release 0.2.1.\n" in text

        # a version that is not semantic is refused
        r = subprocess.run([sys.executable, "-I", script, "v9"], capture_output=True)
        assert r.returncode != 0
    finally:
        remove(root)
    print("changelog: ok")


if __name__ == "__main__":
    main()
