"""Write the next entry of changelog.txt in the Factorio mod changelog format.

    python3 scripts/changelog.py <version> [<last-tag>]

semantic-release calls this from its prepare step with the version being released and the last release's tag. The
commits after that tag are read from git and the entry is put at the top of changelog.txt. Only Conventional Commits
that make a release are listed, so the changelog says what the version rules say. HOOKSHOT_RELEASE_DATE=YYYY-MM-DD
fixes the date, for a release job that runs again.

A prerelease (`0.2.0-rc.1`) changes nothing: Factorio's parser accepts only `major.minor.patch` versions, and two
release candidates share one base version. The final release gets the entry, with every commit since the last stable
tag.

The file stays in the format Factorio's changelog parser expects: a 99-dash separator before each version, Version:
and Date: lines, section names indented two spaces, bullets four, no tabs, no trailing spaces, newest entry first. See
docs/versioning.md.
"""
import argparse
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG = os.path.join(ROOT, "changelog.txt")
SEPARATOR = "-" * 99
# (the order sections appear in; only the ones with commits are written)
SECTIONS = ("Major Features", "Minor Features", "Features", "Changes", "Bugfixes", "Optimizations",
            "Graphics", "Sounds", "Balancing", "Scripting", "Modding", "Locale", "Info")
# (what a released commit becomes; docs, style, test, build, ci and chore are not listed)
SECTIONS_BY_TYPE = {"feat": "Features", "fix": "Bugfixes", "perf": "Optimizations", "revert": "Bugfixes",
                    "refactor": "Changes"}
HEADER = re.compile(r"^(?P<type>[A-Za-z][A-Za-z0-9-]*)(?:\((?P<scope>[^)\r\n]*)\))?(?P<breaking>!)?: (?P<text>.+)$")
BREAKING = re.compile(r"^BREAKING[ -]CHANGES?:", re.MULTILINE)
VERSION = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")


def commits(since=None):
    """(subject, body) for every commit after `since`; the order does not matter, the entry is grouped by section."""
    rev = "%s..HEAD" % since if since else "HEAD"
    r = subprocess.run(["git", "-C", ROOT, "log", "--no-merges", "--format=%H%x1f%s%x1f%b%x1e", rev],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    if r.returncode:
        raise SystemExit("git log %s failed: %s" % (rev, r.stderr.strip()))
    out = []
    for record in r.stdout.split("\x1e"):
        fields = record.strip("\n").split("\x1f", 2)
        if len(fields) == 3:
            out.append((fields[1].strip(), fields[2]))
    return out


def bullet(scope, text):
    """One Factorio bullet: a capital first letter and a full stop, as the format uses."""
    text = " ".join(text.split())
    if not text:
        return ""
    text = text[:1].upper() + text[1:]
    if text[-1] not in ".!?":
        text += "."
    if scope:
        scope = scope[:1].upper() + scope[1:]
        return "%s: %s" % (scope, text)
    return text


def date_line(value=None):
    value = value or os.environ.get("HOOKSHOT_RELEASE_DATE", "").strip()
    day = datetime.date.fromisoformat(value) if value else datetime.date.today()
    return "%02d. %02d. %04d" % (day.day, day.month, day.year)


def entry(version, since=None, date=None):
    found = {section: [] for section in SECTIONS}
    for subject, body in commits(since):
        m = HEADER.match(subject)
        if not m:
            continue
        text = bullet((m.group("scope") or "").strip(), m.group("text"))
        if not text:
            continue
        if m.group("breaking") or BREAKING.search(body):
            found["Changes"].append("Breaking: " + text)
        elif m.group("type").lower() in SECTIONS_BY_TYPE:
            found[SECTIONS_BY_TYPE[m.group("type").lower()]].append(text)
    lines = [SEPARATOR, "Version: %s" % version, "Date: %s" % date_line(date)]
    for section in SECTIONS:
        # (Factorio rejects an exact duplicate in one section, so a repeated subject is listed once)
        entries = list(dict.fromkeys(found[section]))
        if entries:
            lines.append("  %s:" % section)
            lines.extend("    - %s" % one for one in entries)
    if len(lines) == 3:   # (no commit followed the convention: say the least that is true)
        lines.extend(("  Changes:", "    - Release %s." % version))
    return "\n".join(lines) + "\n"


def write(version, since=None, date=None):
    if "-" in version:
        print("changelog: %s is a prerelease; changelog.txt is not changed" % version)
        return
    try:
        with open(CHANGELOG, encoding="utf-8") as f:
            old = f.read()
    except FileNotFoundError:
        old = ""
    if "Version: %s" % version in old.splitlines():   # (a retried job must not add the version twice)
        print("changelog: %s is already in changelog.txt" % version)
        return
    with open(CHANGELOG, "w", encoding="utf-8", newline="\n") as f:
        f.write(entry(version, since, date) + old.lstrip("\n"))


def main():
    ap = argparse.ArgumentParser(description="Add the next version's entry to changelog.txt.")
    ap.add_argument("version", help="the version being released, without the v (0.2.0)")
    ap.add_argument("since", nargs="?", default="", help="the last release's tag (v0.1.0); every commit if omitted")
    ap.add_argument("--date", help="YYYY-MM-DD; default today, or HOOKSHOT_RELEASE_DATE")
    a = ap.parse_args()
    if not VERSION.match(a.version):
        ap.error("not a semantic version: %s" % a.version)
    write(a.version, a.since or None, a.date)


if __name__ == "__main__":
    main()
