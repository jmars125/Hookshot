"""The isolated builder child must print Unicode paths under legacy Windows code pages."""
import contextlib
import io
import locale
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import make_game


def run_capture(command):
    output = io.StringIO()
    try:
        with contextlib.redirect_stdout(output):
            make_game.run(command)
    except SystemExit as error:
        return error.code, output.getvalue()
    return 0, output.getvalue()


def main():
    with tempfile.TemporaryDirectory(prefix="hookshot-漢-") as temporary:
        child = Path(temporary) / "child.py"
        child.write_text('import sys\nprint("child-output:\\ufffd" + sys.argv[1], flush=True)\n', encoding="utf-8")
        legacy_command = [sys.executable, "-I", "-u", str(child), temporary]

        # The old command is expected to fail where the Windows locale cannot encode U+FFFD.
        try:
            "\ufffd".encode(locale.getpreferredencoding(False))
            legacy_unencodable = False
        except UnicodeEncodeError:
            legacy_unencodable = True
        negative_control = "not applicable to this locale/platform"
        if os.name == "nt" and legacy_unencodable:
            code, output = run_capture(legacy_command)
            assert code != 0 and "UnicodeEncodeError" in output, (code, output)
            negative_control = "original isolated command reproduced UnicodeEncodeError"

        code, output = run_capture(make_game.PY + [str(child), temporary])
        assert code == 0, output
        assert "child-output:" in output and temporary in output and "\ufffd" in output, output
    print("PASS: isolated UTF-8 child output; legacy negative control: " + negative_control)


if __name__ == "__main__":
    main()
