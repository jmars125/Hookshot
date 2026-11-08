"""Exercise the production Linux build method without importing Tk or starting a build."""
import ast
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "builder" / "linux" / "hookshot_builder.py"
spec = importlib.util.spec_from_file_location("make_game_jobs", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

# Compile the actual method, with its existing validation, command and side effects.
# The class and module are not executed: no Tk import or window is needed.
builder = next(node for node in ast.parse(SOURCE.read_text(encoding="utf-8")).body
               if isinstance(node, ast.ClassDef) and node.name == "Builder")
build = next(node for node in builder.body if isinstance(node, ast.FunctionDef) and node.name == "build")
BUILD = compile(ast.Module(body=[build], type_ignores=[]), str(SOURCE), "exec")


def value(text):
    return SimpleNamespace(get=lambda: text)


class BuildHarness:
    def __init__(self, folder, environ, kind="nup", fallback=2):
        self.environ = dict(environ)
        self.default_calls, self.events, self.processes = [], [], []
        self.folder = folder
        nup = folder / "synthetic.nup"
        nup.touch()
        romfs, exefs = folder / "romfs", folder / "exefs"
        romfs.mkdir(exist_ok=True)
        exefs.mkdir(exist_ok=True)
        self.gui = SimpleNamespace(
            out=value(str(folder / "out")), kind=value(kind), nup=value(str(nup)),
            romfs=value(str(romfs)), exefs=value(str(exefs)), language=value("English (US)"),
            cfg={"previous": "kept"}, show_progress=lambda: self.events.append(("progress",)), read=lambda proc: None)

        def default_jobs(gb_per_job):
            self.default_calls.append(gb_per_job)
            return fallback

        def start_process(command, **kwargs):
            self.events.append(("process",))
            self.processes.append((command, kwargs))
            return SimpleNamespace()

        def thread(**kwargs):
            self.events.append(("thread", kwargs))
            return SimpleNamespace(start=lambda: self.events.append(("start",)))

        namespace = {
            "os": SimpleNamespace(path=os.path, environ=self.environ), "ROOT": str(ROOT),
            "sys": sys, "make_game": make_game, "toolchain": SimpleNamespace(default_jobs=default_jobs),
            "messagebox": SimpleNamespace(showerror=lambda title, text: self.events.append(("error", title, text))),
            "save_config": lambda cfg: self.events.append(("save", dict(cfg))),
            "time": SimpleNamespace(time=lambda: 123.0),
            "subprocess": SimpleNamespace(Popen=start_process, PIPE=subprocess.PIPE, STDOUT=subprocess.STDOUT),
            "threading": SimpleNamespace(Thread=thread),
        }
        exec(BUILD, namespace)
        self.build = namespace["build"]

    def run(self):
        self.build(self.gui)


class BuilderBuildJobsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-builder-jobs-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)

    def assert_started(self, harness, jobs):
        self.assertEqual([event[0] for event in harness.events], ["save", "progress", "process", "thread", "start"])
        gui = harness.gui
        source = ["--nup", gui.nup.get()] if gui.kind.get() == "nup" else ["--romfs", gui.romfs.get(), "--exefs", gui.exefs.get()]
        expected = [sys.executable, "-I", str(ROOT / "nx2" / "make_game.py")] + source + [
            "--out", gui.out.get(), "--language", "en-US", "--jobs", str(jobs)]
        self.assertEqual(len(harness.processes), 1)
        command, options = harness.processes[0]
        self.assertEqual(command, expected)
        self.assertEqual(options, dict(cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                       encoding="utf-8", errors="replace", start_new_session=True))
        self.assertEqual(harness.events[0][1], dict(previous="kept", kind=gui.kind.get(), nup=gui.nup.get(),
                                                  romfs=gui.romfs.get(), exefs=gui.exefs.get(), out=gui.out.get(), language="en-US"))
        self.assertEqual(harness.events[3][1], dict(target=gui.read, args=(gui.proc,), daemon=True))

    def test_positive_hookshot_jobs_wins_over_legacy_fallback(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "4", "RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 4)
        self.assertEqual(harness.default_calls, [])
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "4", "RX_JOBS": "2"})

    def test_zero_delegates_to_existing_fallback_for_extracted_folders(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "0", "RX_JOBS": "2"}, kind="folders")
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.default_calls, [3.5])
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "0", "RX_JOBS": "2"})

    def test_unset_delegates_to_existing_fallback(self):
        harness = BuildHarness(self.folder, {"RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.default_calls, [3.5])
        self.assertEqual(harness.environ, {"RX_JOBS": "2"})

    def test_empty_delegates_without_changing_inherited_environment(self):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": "", "RX_JOBS": "2"})
        harness.run()
        self.assert_started(harness, 2)
        self.assertEqual(harness.default_calls, [3.5])
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": "", "RX_JOBS": "2"})

    def test_no_override_keeps_the_default_memory_selection(self):
        harness = BuildHarness(self.folder, {}, fallback=3)
        harness.run()
        self.assert_started(harness, 3)
        self.assertEqual(harness.default_calls, [3.5])

    def assert_invalid(self, raw):
        harness = BuildHarness(self.folder, {"HOOKSHOT_JOBS": raw, "RX_JOBS": "2"})
        harness.run()
        self.assertEqual(harness.default_calls, [])
        self.assertEqual(harness.gui.cfg, {"previous": "kept"})
        self.assertEqual(harness.processes, [])
        self.assertFalse(hasattr(harness.gui, "started"))
        self.assertFalse(hasattr(harness.gui, "proc"))
        self.assertEqual(len(harness.events), 1)
        kind, title, message = harness.events[0]
        self.assertEqual((kind, title), ("error", "Hookshot"))
        self.assertIn("HOOKSHOT_JOBS", message)
        self.assertIn("0 for automatic", message)
        self.assertEqual(harness.environ, {"HOOKSHOT_JOBS": raw, "RX_JOBS": "2"})

    def test_negative_override_stops_before_build_side_effects(self):
        self.assert_invalid("-1")

    def test_nonnumeric_override_stops_before_build_side_effects(self):
        self.assert_invalid("n")


if __name__ == "__main__":
    unittest.main()
