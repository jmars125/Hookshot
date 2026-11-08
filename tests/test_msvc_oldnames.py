"""Windows link defaults: oldnames.lib is no longer a default library (issue 439).

Run with: python -I tests/test_msvc_oldnames.py
"""
import importlib.util
import os
from pathlib import Path
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = ROOT / "third_party" / "a64-kit" / "tools" / "toolchain.py"
spec = importlib.util.spec_from_file_location("toolchain_under_test", TOOLCHAIN)
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)

# The legacy names the prebuilt static FFmpeg libraries still reference (avutil's log.o/file.o,
# avcodec's and avutil's file_open.o).
ALIASES = ("close", "fdopen", "isatty", "tempnam")


class MsvcLinkTests(unittest.TestCase):
    def test_msvc_link_does_not_ask_for_oldnames(self):
        fake_os = types.SimpleNamespace(name="nt", path=os.path, environ=os.environ)
        with mock.patch.object(toolchain, "os", fake_os), \
                mock.patch.dict(os.environ, {"HOOKSHOT_MINGW": ""}), \
                mock.patch.object(toolchain, "lld_link", return_value=r"C:\emSDK\lld-link.exe"):
            link = toolchain.native_link()
        self.assertEqual(link[0], "-fuse-ld=lld")
        self.assertIn("-Wl,/nodefaultlib:oldnames", link)

    def test_mingw_link_keeps_suppressing_msvc_default_libraries(self):
        with mock.patch.object(toolchain, "mingw", return_value=r"C:\llvm-mingw"):
            link = toolchain.native_link()
        for name in ("libcmt", "oldnames", "libvcruntime", "msvcrt"):
            self.assertIn("-Wl,-Xlink=/nodefaultlib:" + name, link)

    def test_the_aliases_ffmpeg_asks_for_are_defined(self):
        source = (ROOT / "host" / "msvc_oldnames.c").read_text(encoding="utf-8")
        for name in ALIASES:
            self.assertIn('"/alternatename:%s=_%s"' % (name, name), source,
                          "host/msvc_oldnames.c no longer aliases " + name)


if __name__ == "__main__":
    unittest.main()
