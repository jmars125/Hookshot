# Contributing to Hookshot

Hookshot is a static recompilation of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake: the game's AArch64
code is lifted to C, the Horizon OS and NVN2 are implemented natively, and the GPU work is replayed on Vulkan. No
emulator is involved, and the game is built on your PC from your own copy.

This page covers what to send, how to build and test it, and the rules that keep the project out of trouble. The
[development guide](docs/development.md) has the full pipeline and its commands; the [design notes](docs/design.md)
have the architecture and the lessons the code follows.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and
never will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it: the extracted dump, `image.bin`, the lifted C, test builds, runs and screenshots;
- `hookshot.exe`, `hookshot_vk.dll`, `hookshot_programs.bin`, `hookshot_pipelines.bin`, `mkw_vk_pipelines.bin` and
  other build output;
- saves (`user/`), `hookshot_options.ini`, `hookshot_env.txt` and `language.txt`.

Do not open an issue or a PR asking where to get the game, how to decrypt it, or for someone else's files. A
screenshot, a video and `hookshot_log.txt` are fine and often necessary; the log holds no game data.

`.gitignore` covers most of this, but check `git status` before you commit. If you are not sure whether a file is
game data, leave it out.

## Ways to help

| | What |
|---|---|
| **Test on other hardware** | The project has been tested on one Windows PC (Ryzen 5 7600X, RTX 4060 Ti) and one Linux PC so far. Reports from AMD and Intel cards, other NVIDIA drivers, other distributions and SteamOS are useful even when everything works. |
| **Play further** | The tested area is the start of the game. Reaching new areas, cutscenes, menus and items finds problems the developer has not seen. |
| **Report what is broken** | Use the [issue forms](https://github.com/NotRexed/Hookshot/issues/new/choose): game problem, build problem or idea. Include the version, your hardware, where you were and `hookshot_log.txt`. |
| **Send a fix** | Fork, fix, test, open a pull request. Small, focused changes are easiest to review and merge. |
| **Improve the docs** | The README, the development guide and the design notes are part of the project. Corrections and clearer steps are welcome. |

## Before you open an issue or a PR

- Try the [newest release](https://github.com/NotRexed/Hookshot/releases/latest) first. It includes every fix before it.
- Search the open issues. If yours is already there, add your details instead of opening another.
- English, please.
- One topic per issue or pull request. Do not mix a fix with a refactor or a batch of unrelated changes.
- Report the version you actually ran: the release name, or the first line of `hookshot_log.txt`.
- Test without ReShade, shader-cache tools, overlays and frame-generation injectors when you report a graphics or
  performance problem. They change what Hookshot does.

## Development setup

You need your own supported dump: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Other versions do
not work, and the build reads the dump where it lies without changing it. Builds need about 60 GB free and a run uses
about 10 GB of RAM.

### Windows, from the command line

1. Install Python 3 from [python.org](https://www.python.org/downloads/) (not the Microsoft Store build: `python -I`
   does not see packages installed there) and the pipeline's packages:

   ```powershell
   python -m pip install lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard
   ```

2. Get [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build) and set `HOOKSHOT_MINGW` to its
   folder. Without it the build falls back to emsdk's clang at `C:\emsdk` with Visual Studio's C++ tools and the
   Windows SDK (`HOOKSHOT_MSVC=1` forces that route).

3. Copy the pieces a clone does not carry from the `tools` folder of the latest `HookshotBuilder.zip`:

   | From the zip | To | Variable |
   |---|---|---|
   | `tools\cpm` | anywhere | `RX_CPM` points at it (Vulkan headers and FFmpeg; without it the Vulkan renderer is left out) |
   | `tools\wgpu-native` | anywhere | `WGPU_NATIVE` points at it |
   | `tools\third_party\sdl2\SDL2.dll` | `third_party\sdl2\` | |
   | `tools\work` | `work\` | (hot-function list, link order, profile and icon; without it the game builds but runs slower) |

4. Run everything with `python -I` from the repository root.

### Linux

Either use the `Hookshot-<version>-x86_64.AppImage` from the Releases page, or build the environment and the AppImage
yourself:

```sh
scripts/linux/deps.sh       # LLVM, Python packages, FFmpeg with dav1d, SDL2, Vulkan headers
scripts/linux/appimage.sh   # the AppImage, in build-linux/
```

The AppImage needs glibc 2.35 or newer and a Vulkan 1.3 driver with mesh shaders. `HOOKSHOT_JOBS` sets how many files
compile at once. The README's [Linux section](readme.md#on-linux) has the details and the runtime paths.

### Hookshot Builder

The builder is a .NET 8 WPF app. Open `builder/HookshotBuilder.sln` in Visual Studio 2022 with the .NET desktop
workload, or build it from a developer prompt:

```powershell
dotnet build builder/HookshotBuilder.sln
```

You only need Visual Studio for the builder itself. The game build uses llvm-mingw.

## Building

| Command | What it does |
|---|---|
| `python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Hookshot" --language en-US` | Build a complete game folder from a `.nup`. A build takes about 40 minutes. |
| `python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Hookshot"` | The same, from extracted folders. |
| `python -I nx2/make_game.py --check --nup "C:\path\your-game.nup"` | Check the game and its version without building. |
| `python -I nx2/dump.py --dump <folder with exefs/ and romfs/> --set` | Record the dump in `work/dump.txt`. |
| `python -I nx2/lift.py --standalone --write` | Lift the game to C in `work/sa/gen`. |
| `python -I nx2/build.py --standalone --jobs 4` | Compile `work/sa/build/hookshot.exe` and `hookshot_vk.dll`. |
| `powershell -File scripts/run-vk.ps1 -Window -Seconds 120` | Run the standalone build with the Vulkan back end and take screenshots in `work/runs/<name>/shots`. |
| `python -I scripts/prof.py <stacks file>` | Read a profile sampled with `HOS_SAMPLE` and `HOS_SAMPLE_FILE`. |

Use a separate `--work` folder or `OOT_WORK` if you keep more than one dump around. The lifted C and every build are
local artifacts; they are never committed.

## Testing

### Game-free checks

These need no dump and are what CI runs. The exact commands are the `run:` lines in
[`source-regressions.yml`](.github/workflows/source-regressions.yml); for example:

```powershell
python -I -B nx2/test_nup_paths.py
python -I -B nx2/test_version.py
python -I -B tests/test_make_game_cache.py
python -I -B tests/test_failure_reason.py
python -I -B tests/test_dispatch_clock.py
```

Some checks compile a small C or C++ fixture on the fly and need a compiler: `tests/test_audio_deadline.py`,
`tests/test_fps_overlay_order.py`, `tests/test_fps_rate.py`, `tests/test_framegen_gate.py`,
`tests/test_import_hold.py`, `tests/test_movie_upscale_policy.py`, `tests/test_pool_span.py`,
`tests/test_prof_clock.py`, `tests/test_rumble_refresh.py`, `tests/test_submitted_page.py`,
`tests/test_table_track.py`, `tests/test_ub_memo.py`, `tests/test_upscale_label.py` and
`nx2/test_import_windows.py`. Pass `--cxx clang++` (or `g++`) to choose one; `tests/test_import_hold.py` skips when
no compiler is found. CI marks most of these as Linux-only, so on Windows run the ones you touched by hand. A few
other checks, such as `tests/test_desc_clock.py`, only read the source and need nothing extra.

There are also builder and Linux checks that CI does not wire up yet, such as `tests/test_builder_status.py`,
`tests/test_desktop_exec.py`, `tests/test_sdl_audio_library.py` and the Windows-only `tests/test_cpp_preflight.py`.
Run them by hand if you change what they cover.

### Checks that need your dump or a run

These are slow and local, and they need `work/dump.txt` from `nx2/dump.py --set`:

| Command | What it checks |
|---|---|
| `python -I nx2/test_insn.py` | Every instruction form the game uses, Unicorn against the lifted C. |
| `python -I nx2/test_leaf.py --calls` | Whole functions and call trees on the relocated image, native against Unicorn. |
| `python -I nx2/census.py --nx2` | The instruction census per module. |
| `python -I nx2/lower_test.py work/lowfail` | Shader programs that failed to lower. |
| `powershell -File scripts/run-vk.ps1 -Window` | An in-game run: screenshots, the flash detector, frame dumps and the profiler. |

For a graphics fix, look at the screenshots in `work/runs/<name>/shots` and delete them afterwards; they reach
gigabytes and are derived from the game.

### When you add a fix or a check

- Add a game-free test where the behavior can be tested without the dump. The existing tests are small Python files
  under `tests/` or `nx2/` that read source files and run them, or compile a tiny C++ fixture.
- Wire the test into `.github/workflows/source-regressions.yml`: add a `run:` step, and add the test file and every
  source file it reads to both the `push` and `pull_request` `paths` lists. Without the paths, the check does not run
  when the code changes.
- Keep the test fast enough for the 10-minute CI job, and make it pass on both Ubuntu and Windows where it can.
- If the fix is in the renderer or the runtime, say what you saw before and after, on which GPU and driver, and in
  which part of the game.

## Code and data conventions

- Match the file you are editing: there is no formatter configuration, so use the indentation, comment style and
  naming around you. Do not reformat unrelated code in the same change.
- Keep OoT-specific code in new files where possible. `nx2/`, `host/` and `gpu/` are kept close to mkw-recomp so
  upstream fixes can be applied as a patch; keeping the paths and the shared files recognizable is what makes that
  work. Each sync names its new mkw-recomp base commit in the commit message.
- Do not modify `third_party/`. Its parts are unmodified upstream copies with their notices in `PROVENANCE.md`; add a
  new folder and a provenance note rather than editing one.
- Treat the local mkw-recomp and Eglantine checkouts as read-only references: read from them, run them, never edit
  them.
- New switches use the `RX_` prefix in the renderer and `HOS_` in the runtime. They are off, or neutral, unless
  asked for, and they do not cost anything while off. Document them where the surrounding switches are documented.
- The GPU replay rules in [design notes, section 4](docs/design.md#4-the-lessons-from-mkw-as-rules-here) are not
  suggestions: nothing the replay reads may come from live guest memory, waits are honoured per queue, and texture
  and sampler registrations stay in submission order. A change that reads guest memory at replay time is a
  regression, even if it looks right on one frame.
- Python runs with `-I`. Tests stay on the standard library so CI needs no install step; the pipeline's existing
  dependencies (lz4, numpy, capstone, unicorn, zstandard) are the exception, not a reason to add more. Keep line
  endings LF (`.gitattributes`).
- New code has to be GPL-compatible. See below.

## Commits and pull requests

1. Fork the repository and branch from `main`. Name the branch for the change: `fix/...`, `linux/...`, `builder/...`,
   `docs/...`.
2. Keep the pull request to one topic. Bug fixes do not carry refactors, and two unrelated fixes are two PRs.
3. Write the title as a short imperative sentence, as the history does: "Stop scanning storage-buffer tables after
   one pass", "Linux: a crash report gives the whole host stack". The body says what changed, why, how it was tested
   (GPU, driver, game area, fps before and after), and links the issue it fixes. Before/after screenshots help for
   graphics.
4. Run the relevant checks. CI must pass; a change that only works on your machine is not ready.
5. Rebase or merge `main` when it has moved, and resolve conflicts in your branch rather than in the review.
6. Do not include build output, dumps, generated files or a `.gitignore` bypass. Check `git status`.

Maintainer commits record AI assistance with `Assisted-by:` and `Co-Authored-By:` trailers. If you used a tool, add
the trailers that describe it; if you wrote the change yourself, there is nothing to add.

## Licensing

Hookshot is free software under the [GNU General Public License, version 3 or later](LICENSE). By submitting a change
you agree to license it under those terms. Only send code you can license that way. Do not paste code from a project
with an incompatible license, and do not paste decompiled game code or anything generated from the dump into a PR.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, stb, zbic and the a64-kit tools)
keep their own notices. Preserve them, and do not relicense their code.

## Where to talk

- **Issues:** [use the forms](https://github.com/NotRexed/Hookshot/issues/new/choose).
- **Discord:** https://discord.gg/EajnD2AJD9. Use the `hookshot-discussion` channel; the server is mainly for
  web ports, and that channel is where Hookshot talk belongs.
- **Newest release:** https://github.com/NotRexed/Hookshot/releases/latest.

Thanks for testing, reporting and sending fixes. Every report from a different GPU and every area played further than
before moves the port forward.
