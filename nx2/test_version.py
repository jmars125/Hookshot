"""nx2/version.py, game-free: where the release name comes from, and a fingerprint that ignores line endings.

    python -I -B nx2/test_version.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import version  # noqa: E402


def tree(root, eol):
    for rel in ("host/a.c", "gpu/vk/b.cpp", "nx2/c.py"):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(("line one%sline two%s" % (eol, eol)).encode())


def main():
    os.environ.pop("HOOKSHOT_VERSION", None)
    with tempfile.TemporaryDirectory() as lf, tempfile.TemporaryDirectory() as crlf:
        tree(lf, "\n")
        tree(crlf, "\r\n")
        assert version.release(lf) == "unknown", version.release(lf)
        assert version.fingerprint(lf) == version.fingerprint(crlf), "line endings changed the fingerprint"
        with open(os.path.join(lf, "nx2", "c.py"), "a") as f:
            f.write("changed\n")
        assert version.fingerprint(lf) != version.fingerprint(crlf), "a changed source kept the fingerprint"

        with open(os.path.join(lf, "VERSION"), "w") as f:
            f.write("movie-sync\nignored second line\n")
        assert version.release(lf) == "movie-sync"
        os.environ["HOOKSHOT_VERSION"] = "from-env"
        assert version.release(lf) == "from-env", "HOOKSHOT_VERSION comes first"

        os.environ["HOOKSHOT_VERSION"] = 'v1 "quoted" \\ & | %PATH%'
        d = version.describe(lf)
        assert not set(d) & set('"\\&|%'), d
        assert version.define(lf).startswith('-DHOOKSHOT_VERSION="') and version.define(lf).endswith(')"')
        os.environ.pop("HOOKSHOT_VERSION")
    print("version: ok (%s)" % version.describe())


if __name__ == "__main__":
    main()
