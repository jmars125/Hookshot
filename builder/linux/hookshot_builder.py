"""Hookshot Builder for Linux: what builder/HookshotBuilder (WPF) is on Windows, in Tk. It runs nx2/make_game.py and
follows its @@ lines (make_game.py's docstring), then offers to start the game. The AppImage starts it (builder/linux/AppRun),
with the compiler, sysroot and libraries it brings in the environment.

    hookshot_builder.py           the window
    hookshot_builder.py --play    the game built last time, if there is one (else the window)
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("HOOKSHOT_TOOLS") or os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
import make_game  # noqa: E402  (LANGUAGES, BUILD_ID)
import toolchain  # noqa: E402  (default_jobs)

CONFIG = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "hookshot", "builder.ini")
STAGES = {"read": "Reading your game files", "lift": "Translating the game to PC code", "compile": "Compiling",
          "finish": "Putting the game folder together"}


def load_config():
    cfg = {}
    try:
        for line in open(CONFIG, encoding="utf-8"):
            k, eq, v = line.rstrip("\n").partition("=")
            if eq:
                cfg[k] = v
    except OSError:
        pass
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    with open(CONFIG, "w", encoding="utf-8") as f:
        f.writelines("%s=%s\n" % kv for kv in cfg.items())


def game_ready(folder):
    return bool(folder) and os.path.isfile(os.path.join(folder, "hookshot")) and os.path.isfile(os.path.join(folder, "image.bin"))


def play(folder):
    """the game, on its own (it outlives this window), from its folder"""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("HOOKSHOT_", "PYTHON", "APPDIR", "APPIMAGE", "ARGV0", "OWD"))}
    env.pop("LD_LIBRARY_PATH", None)
    subprocess.Popen([os.path.join(folder, "hookshot")], cwd=folder, env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def desktop_exec(path):
    """One Exec argument. A % in the path is a field code, so "100% Games" is rejected and the menu entry does not run."""
    escaped = path.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
    return '"%s"' % escaped


def add_to_menu(folder):
    """an entry in the applications menu that starts the game"""
    icon = os.path.join(folder, "hookshot.png")
    src = os.path.join(ROOT, "assets", "hookshot.png")
    if os.path.exists(src):
        shutil.copyfile(src, icon)
    apps = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "applications")
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, "hookshot.desktop"), "w", encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Hookshot\nComment=The Legend of Zelda: Ocarina of Time, built from your copy\n"
                "Exec=%s\nPath=%s\nIcon=%s\nCategories=Game;\nTerminal=false\n" % (desktop_exec(os.path.join(folder, "hookshot")), folder, icon))


def free_gb(path):
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or "/").free / 2 ** 30
    except OSError:
        return None


class Builder(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Hookshot Builder")
        self.minsize(620, 420)
        self.cfg = load_config()
        self.proc = None
        self.lines = queue.Queue()
        self.started = 0.0
        self.kind = tk.StringVar(value=self.cfg.get("kind", "nup"))
        self.nup = tk.StringVar(value=self.cfg.get("nup", ""))
        self.romfs = tk.StringVar(value=self.cfg.get("romfs", ""))
        self.exefs = tk.StringVar(value=self.cfg.get("exefs", ""))
        self.out = tk.StringVar(value=self.cfg.get("out", os.path.expanduser("~/Games/Hookshot")))
        names = [n for _, n in make_game.LANGUAGES]
        code = self.cfg.get("language", "en-US")
        self.language = tk.StringVar(value=dict(make_game.LANGUAGES).get(code, names[0]))
        self.check_text = tk.StringVar()
        self.space_text = tk.StringVar()
        self.stage_text = tk.StringVar()
        self.time_text = tk.StringVar()
        self.pad = {"padx": 12, "pady": 4}
        self.page = None
        self.show_form()
        self.after(100, self.pump)
        self.protocol("WM_DELETE_WINDOW", self.close)

    # ---- the form ----
    def show_form(self):
        if self.page:
            self.page.destroy()
        f = self.page = ttk.Frame(self)
        f.pack(fill="both", expand=True, pady=8)
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Hookshot", font=("TkDefaultFont", 16, "bold")).grid(row=0, column=0, columnspan=3, sticky="w", **self.pad)
        ttk.Label(f, text="Turns your own copy of Ocarina of Time into a game folder you can start on PC.").grid(
            row=1, column=0, columnspan=3, sticky="w", **self.pad)
        if game_ready(self.cfg.get("out")):
            ttk.Button(f, text="Play the game built in %s" % self.cfg["out"], command=lambda: self.play_and_close(self.cfg["out"])).grid(
                row=2, column=0, columnspan=3, sticky="we", **self.pad)
        ttk.Label(f, text="Game files", font=("TkDefaultFont", 10, "bold")).grid(row=3, column=0, sticky="w", **self.pad)
        kinds = ttk.Frame(f)
        kinds.grid(row=3, column=1, columnspan=2, sticky="w")
        ttk.Radiobutton(kinds, text=".nup file", value="nup", variable=self.kind, command=self.show_form).pack(side="left")
        ttk.Radiobutton(kinds, text="romfs and exefs", value="folders", variable=self.kind, command=self.show_form).pack(side="left", padx=12)
        row = 4
        if self.kind.get() == "nup":
            self.path_row(f, row, ".nup file", self.nup, lambda: self.pick_file(self.nup))
        else:
            self.path_row(f, row, "romfs", self.romfs, lambda: self.pick_dir(self.romfs))
            row += 1
            self.path_row(f, row, "exefs", self.exefs, lambda: self.pick_dir(self.exefs))
        row += 1
        ttk.Label(f, textvariable=self.check_text, wraplength=560).grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        self.path_row(f, row, "Save the game to", self.out, lambda: self.pick_dir(self.out))
        row += 1
        ttk.Label(f, text="Language").grid(row=row, column=0, sticky="w", **self.pad)
        ttk.Combobox(f, textvariable=self.language, values=[n for _, n in make_game.LANGUAGES], state="readonly").grid(
            row=row, column=1, sticky="w", **self.pad)
        row += 1
        ttk.Label(f, text="Takes about 45 minutes and needs 60 GB.").grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        ttk.Label(f, textvariable=self.space_text).grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        ttk.Button(f, text="Build", command=self.build).grid(row=row, column=0, columnspan=3, sticky="we", padx=12, pady=12)
        self.out.trace_add("write", lambda *a: self.update_space())
        self.update_space()

    def path_row(self, f, row, label, var, browse):
        ttk.Label(f, text=label).grid(row=row, column=0, sticky="w", **self.pad)
        ttk.Entry(f, textvariable=var).grid(row=row, column=1, sticky="we", **self.pad)
        ttk.Button(f, text="Browse", command=browse).grid(row=row, column=2, **self.pad)

    def pick_file(self, var):
        p = filedialog.askopenfilename(title="Your game's .nup", filetypes=[("Switch 2 game package", "*.nup"), ("All files", "*")])
        if p:
            var.set(p)
            self.check_version()

    def pick_dir(self, var):
        p = filedialog.askdirectory(title="Choose a folder", initialdir=var.get() or os.path.expanduser("~"))
        if p:
            var.set(p)
            if var is self.exefs:
                self.check_version()

    def update_space(self):
        gb = free_gb(self.out.get())
        self.space_text.set("%s has %.0f GB free." % (self.out.get(), gb) if gb is not None else "")

    def check_version(self):
        args = ["--nup", self.nup.get()] if self.kind.get() == "nup" else ["--exefs", self.exefs.get()]
        self.check_text.set("Checking the game version...")

        def run():
            r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py"), "--check"] + args,
                               capture_output=True, text=True, cwd=ROOT)
            said = []
            for l in r.stdout.splitlines():
                t = make_game.reported(l)
                if t is not None:
                    said.append(t)
            text = said[-1] if said else "The version could not be read."
            self.lines.put(("check", text))
        threading.Thread(target=run, daemon=True).start()

    # ---- the build ----
    def build(self):
        out = os.path.expanduser(self.out.get().strip())
        if self.kind.get() == "nup":
            if not os.path.isfile(self.nup.get()):
                return messagebox.showerror("Hookshot", "Choose your game's .nup file.")
            src = ["--nup", self.nup.get()]
        else:
            if not (os.path.isdir(self.romfs.get()) and os.path.isdir(self.exefs.get())):
                return messagebox.showerror("Hookshot", "Choose your game's romfs and exefs folders.")
            src = ["--romfs", self.romfs.get(), "--exefs", self.exefs.get()]
        if not out:
            return messagebox.showerror("Hookshot", "Choose where to save the game.")
        code = next((c for c, n in make_game.LANGUAGES if n == self.language.get()), "en-US")
        self.cfg.update(kind=self.kind.get(), nup=self.nup.get(), romfs=self.romfs.get(), exefs=self.exefs.get(), out=out, language=code)
        save_config(self.cfg)
        jobs = toolchain.default_jobs(3.5)   # (what fits in memory: a big chunk takes up to ~3.5 GB to compile)
        cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py")] + src + ["--out", out, "--language", code, "--jobs", str(jobs)]
        self.show_progress()
        self.started = time.time()
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                     encoding="utf-8", errors="replace", start_new_session=True)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()

    def read(self, proc):
        for line in proc.stdout:
            self.lines.put(("line", line.rstrip("\n")))
        self.lines.put(("exit", proc.wait()))

    def show_progress(self):
        self.page.destroy()
        f = self.page = ttk.Frame(self)
        f.pack(fill="both", expand=True, pady=8)
        f.columnconfigure(0, weight=1)
        f.rowconfigure(5, weight=1)
        ttk.Label(f, text="Building", font=("TkDefaultFont", 16, "bold")).grid(row=0, column=0, sticky="w", **self.pad)
        ttk.Label(f, text="This takes a while, and your PC stays usable.").grid(row=1, column=0, sticky="w", **self.pad)
        ttk.Label(f, textvariable=self.stage_text).grid(row=2, column=0, sticky="w", **self.pad)
        self.bar = ttk.Progressbar(f, maximum=1.0, mode="determinate")
        self.bar.grid(row=3, column=0, sticky="we", **self.pad)
        ttk.Label(f, textvariable=self.time_text).grid(row=4, column=0, sticky="w", **self.pad)
        self.log = tk.Text(f, height=12, wrap="none", font=("TkFixedFont", 9))
        self.log.grid(row=5, column=0, sticky="nsew", **self.pad)
        self.buttons = ttk.Frame(f)
        self.buttons.grid(row=6, column=0, sticky="e", **self.pad)
        ttk.Button(self.buttons, text="Cancel", command=self.cancel).pack(side="right")
        self.stage_text.set("Starting")
        self.tick()

    def tick(self):
        if self.proc and self.proc.poll() is None:
            s = int(time.time() - self.started)
            self.time_text.set("Elapsed %d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60))
            self.after(1000, self.tick)

    def pump(self):
        try:
            while True:
                kind, v = self.lines.get_nowait()
                if kind == "check":
                    self.check_text.set(v)
                elif kind == "line":
                    self.on_line(v)
                elif kind == "exit":
                    self.on_exit(v)
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def on_line(self, line):
        if line.startswith("@@stage "):
            stage = line.split(" ", 2)[1]
            self.stage_text.set(STAGES.get(stage, line.split(" ", 2)[-1]))
            self.bar.configure(mode="determinate", value=0)
        elif line.startswith("@@progress "):
            try:
                self.bar.configure(value=float(line.split()[1]))
            except ValueError:
                pass
        elif line.startswith("@@done "):
            self.built = line[7:]
        elif line.startswith("@@error "):
            self.error = line[8:]
        if hasattr(self, "log"):
            self.log.insert("end", line + "\n")
            self.log.see("end")

    def on_exit(self, code):
        self.proc = None
        for w in self.buttons.winfo_children():
            w.destroy()
        folder = getattr(self, "built", None)
        if code == 0 and folder and game_ready(folder):
            s = int(time.time() - self.started)
            self.stage_text.set("Your game is ready, built in %d:%02d" % (s // 3600, s // 60 % 60))
            self.bar.configure(value=1.0)
            try:
                add_to_menu(folder)
            except OSError:
                pass
            ttk.Button(self.buttons, text="Play", command=lambda: self.play_and_close(folder)).pack(side="right", padx=4)
            ttk.Button(self.buttons, text="Open folder", command=lambda: subprocess.Popen(["xdg-open", folder])).pack(side="right", padx=4)
        else:
            self.stage_text.set("The build stopped: %s" % getattr(self, "error", "see the lines above"))
            ttk.Button(self.buttons, text="Back", command=self.show_form).pack(side="right", padx=4)

    def cancel(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.error = "Build cancelled. You can start again from the same folder."

    def play_and_close(self, folder):
        play(folder)
        self.destroy()

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno("Hookshot", "Stop the build and close?"):
                return
            os.killpg(self.proc.pid, signal.SIGTERM)
        self.destroy()


def main():
    if "--play" in sys.argv[1:]:
        folder = load_config().get("out")
        if game_ready(folder):
            play(folder)
            return
    Builder().mainloop()


if __name__ == "__main__":
    main()
