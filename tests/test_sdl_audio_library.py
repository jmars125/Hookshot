"""Linux audio loads the SDL beside the executable before the system one."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "host" / "posix" / "win32_posix.c"


class SdlAudioLibraryTests(unittest.TestCase):
    def test_audio_tries_the_bundled_sdl_first(self):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("MMRESULT waveOutOpen")
        fn = text[start:text.index("MMRESULT waveOutPrepareHeader", start)]
        self.assertLess(fn.index("exe_path"), fn.index('LoadLibraryA("SDL2.dll")'))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
