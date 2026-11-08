"""Discover authored CLI package layouts; no real dump, compiler or dependency headers."""
import contextlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "build.py"


class CpmDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-cpm-fixture-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.tools = self.base / "portable builder \u03a9" / "tools"
        self.tools.mkdir(parents=True)
        self.bundled = self.tools / "cpm"
        self.legacy = self.tools / "work" / "eg" / ".cache" / "cpm"

    def cache(self, root):
        ffmpeg = root / "ffmpeg" / "authored-version"
        (ffmpeg / "lib").mkdir(parents=True)
        header = ffmpeg / "include" / "libavcodec" / "avcodec.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        vulkan = root / "vulkanheaders" / "authored-version"
        header = vulkan / "include" / "vulkan" / "vulkan.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* authored fixture */\n", encoding="utf-8")
        return {"ffmpeg": str(ffmpeg), "vulkanheaders": str(vulkan)}

    @contextlib.contextmanager
    def build(self, override=""):
        # The real kit reads dump settings on import. Replace only unrelated imports
        # so the real build module discovers packages in this owned layout instead.
        kit = types.SimpleNamespace(ROOT=str(self.tools), HERE=str(self.tools / "nx2"),
                                    WORK=str(self.tools / "work"))
        toolchain = types.SimpleNamespace(windows=lambda: os.name == "nt")

        def no_process(command, *args, **kwargs):
            if command[0] == "pkg-config":
                raise OSError("This fixture has no system FFmpeg")
            raise AssertionError("Unexpected build process: " + repr(command))

        with (mock.patch.dict(sys.modules, {"kit": kit, "rtgen": types.ModuleType("rtgen"),
                                           "toolchain": toolchain}),
              mock.patch.object(sys, "path", list(sys.path)),
              mock.patch.dict(os.environ, {"RX_CPM": str(override), "HOOKSHOT_FFMPEG": ""}),
              mock.patch.object(subprocess, "run", side_effect=no_process),
              mock.patch.object(subprocess, "Popen", side_effect=AssertionError("Unexpected process launch"))):
            spec = importlib.util.spec_from_file_location("cpm_build_fixture", SOURCE)
            build = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(build)
            yield build

    def assert_packages(self, build, expected):
        self.assertEqual(build.cpm_package("ffmpeg"), expected["ffmpeg"],
                         "bundled FFmpeg was not discovered")
        self.assertEqual(build.cpm_package("vulkanheaders"), expected["vulkanheaders"],
                         "bundled Vulkan headers were not discovered")
        if not build.LINUX:
            self.assertEqual(build.FFMPEG, expected["ffmpeg"], "Windows import did not select FFmpeg")

    def test_portable_package_without_rx_cpm(self):
        expected = self.cache(self.bundled)
        with self.build() as build:
            self.assert_packages(build, expected)

    def test_explicit_override_wins(self):
        self.cache(self.bundled)
        self.cache(self.legacy)
        override = self.base / "explicit cache"
        expected = self.cache(override)
        with self.build(override) as build:
            self.assert_packages(build, expected)

    def test_bundle_precedes_legacy_cache(self):
        expected = self.cache(self.bundled)
        self.cache(self.legacy)
        with self.build() as build:
            self.assert_packages(build, expected)

    def test_legacy_fallback_without_bundle(self):
        expected = self.cache(self.legacy)
        with self.build() as build:
            self.assert_packages(build, expected)


if __name__ == "__main__":
    unittest.main()
