<h1 align="center">
  <br>
  <a>
  <img src="assets/hookshot.png" alt="Hookshot" width="200">
</a>

  <br>
  <b>Hookshot</b>
  <br>
</h1>

<h4 align="center">The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) running natively on Windows & Linux, with no emulator.
</h4>

<p align="center">
  <a href="#project-status">Status</a> |
  <a href="#how-to-build">How to build</a> |
  <a href="#in-game-menu">In-game menu</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a>
</p>

Hookshot is a static recompilation. The game's code is translated to C and compiled into a Windows program, the
console's system software is reimplemented, and the Switch 2 graphics are rebuilt on Vulkan. All of this happens
**on your PC, from your own copy of the game**.
<br>

Discord: https://discord.gg/EajnD2AJD9 Please use the hookshot-discussion channel, as the main point of the discord is for webports, not my work
<br>

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy. Everything the build creates (`hookshot.exe`, the generated code, the shader caches and your
> saves) comes from your game and stays on your PC. Never share those files.

## Project status

Experimental. It has only been tested on **one PC** (Ryzen 5 7600X, RTX 4060 Ti 16 GB), mostly in the area at the start
of the game.

| What | Status |
|---|---|
| Kokiri Forest: reaching gameplay and walking around | ✅ Works |
| Sound, controller, keyboard and saves | ✅ Works |
| In-game menu, frame generation and upscaling | ✅ Works |
| Black flashes while moving | ✅ Fixed in testing, more play needed |
| Performance without frame generation | ⚠️ About 50 fps walking, not yet a steady 60 |
| Models with missing pieces (doors, fences, parts of trees) | ✅ Fixed where tested, more play needed |
| Stutter when entering new areas | ⚠️ Still happens, mostly the first time |
| Ultrawide | ⚠️ The 3D keeps its shape, but the HUD is stretched and the picture is softer |
| Hyrule Field and the rest of the game | ❓ Not tested yet |
| AMD and Intel cards | ❓ Never tested |

## How to build

The game does not come ready-made: it is built on your PC from your copy.

**With Hookshot Builder, the easiest:**

1. Download the HookshotBuilder zip from the Releases page, extract it, and run `HookshotBuilder.exe`. Keep the `tools`
   folder beside it.
2. Choose your game's `.nup`, or your `romfs` and `exefs` folders. The builder checks that it is the right version.
3. Choose a language and a folder for the finished game, and start the build.
4. When it finishes, open `hookshot.exe` in that folder.

**From the command line:**

```powershell
python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Hookshot" --language en-US
```

Or with folders:

```powershell
python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Hookshot" --language en-US
```

Add `--check` with your `.nup` or `--exefs` to only check the game's version. It builds nothing.

A build takes about 40 minutes, and you can use your PC in the meantime.

### On Linux

1. Download `Hookshot-<version>-x86_64.AppImage` from the Releases page (or the Linux AppImage workflow's artifact),
   make it executable (`chmod +x`) and start it. It is Hookshot Builder with everything it needs inside: the compiler,
   the libraries and the Vulkan renderer. Nothing else has to be installed.
2. Choose your `.nup` (or your `romfs` and `exefs` folders), a language and a folder for the game, and build.
3. When it finishes, press Play. The game is also added to your applications menu. The same AppImage starts that
   game without opening the builder: `./Hookshot-<version>-x86_64.AppImage --play`, or `--play ~/Games/Hookshot`
   for a different folder. The game itself is `hookshot` in the folder you chose: it runs without the AppImage.

From the command line: `./Hookshot-<version>-x86_64.AppImage --cli --nup ~/your-game.nup --out ~/Games/Hookshot`
(the same options as `make_game.py`).

You need a 64-bit x86 PC with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora, Arch, SteamOS ...), a Vulkan 1.3
driver with mesh shaders (Mesa or NVIDIA), and the same memory and disk space as on Windows. The build's own files go
to `~/.cache/hookshot-builder`. Saves go in `$XDG_DATA_HOME/hookshot` (`~/.local/share/hookshot`). A `user` folder
beside the game from an earlier build is moved there once. `HOOKSHOT_JOBS` sets how many files compile at once.
Switches to test (`RX_VK_NOOPT=1` and the others) go in `hookshot_env.txt` beside the game, one `NAME=VALUE` a line,
as on Windows.

`RX_VK_MOVIE_UPSCALE=linear` replaces the movie's neural upscale with the existing linear path. The default `auto` behavior
keeps the networks and falls back only when movie GPU time exceeds the existing 40 ms trigger. This switch does not change
the gameplay upscaler and can make movie images look softer. It works on Windows and Linux; `RX_VK_NETWORK_KEEP=all` takes
precedence and keeps the movie networks enabled.

Building it yourself without the AppImage: `clang` and `lld` (LLVM 23 or newer reads the play profile; an older one
builds without it), Python 3 with `lz4`, `numpy`, `capstone`, `unicorn` and `zstandard`, FFmpeg's development files
(with dav1d, for the movies) and SDL2. Then `python -I nx2/make_game.py ...` as above, with `RX_CPM` pointing at a folder
with Vulkan-Headers, SPIRV-Headers and VulkanMemoryAllocator in CPM's layout (`scripts/linux/deps.sh` makes one).
`scripts/linux/deps.sh` and `scripts/linux/appimage.sh` make the AppImage.

### Updating to a newer release

A new release is not a patch to your game folder: the game is built again from your copy, with the new code. Your
saves and settings are kept.

1. Close the game.
2. To be safe, copy your saves somewhere first: the `user` folder beside `hookshot.exe` on Windows, or
   `~/.local/share/hookshot` on Linux.
3. Get the new release:
   - **Hookshot Builder:** download the new HookshotBuilder zip and extract it to a new folder (with its `tools`
     folder). The old builder folder can then be deleted.
   - **Linux:** download the new AppImage and make it executable. The old one can be deleted, the game is not removed with it.
   - **Command line:** `git pull` in your clone, and take the `tools` folder's contents (see
     [What you need](#what-you-need)) from the new HookshotBuilder zip, as they can change between releases.
4. Build again with the same `.nup` (or `romfs` and `exefs` folders) and **the same game folder** as before. With the
   same `.nup`, the game is not read again, but the translation and the compile still run, so it takes about as long
   as the first build.
5. Start the game as before.

What a new build keeps and what it replaces in the game folder:

| Kept | Replaced |
|---|---|
| Your saves (`user`, or `~/.local/share/hookshot` on Linux) | `hookshot.exe` (`hookshot` on Linux) and its files |
| `hookshot_options.ini`, the in-game menu's choices | `image.bin` and the shader list |
| `hookshot_env.txt` | `language.txt`, written again with the language you choose |

### What you need

- Nothing to install for the build itself: Hookshot Builder brings its own compiler ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw),
  as TotK Recomp does), Python and libraries. Visual Studio and the Windows SDK are no longer needed.
- 64-bit Windows 10 or 11.
- A graphics card with Vulkan 1.3 and mesh shaders, with an up-to-date driver. Only NVIDIA has been tested.
- 16 GB of RAM or more. A run uses about 10 GB.
- About 60 GB free while building.
- **Your copy of the game at the supported version:** program `0400486024DEC000`, update v131072, build `e0f3ccfa`.
  Other versions do not work.

To build from the command line you also need:

- Python 3 with `lz4`, `numpy`, `capstone`, `unicorn` and `zstandard`:
  `pip install lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard`
  Use Python from python.org: the Microsoft Store's Python installs packages where `python -I` does not look.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build), with `HOOKSHOT_MINGW` set to its folder. Without
  it the build uses the clang from [emsdk](https://github.com/emscripten-core/emsdk) at `C:\emsdk` with Visual Studio's C++ tools
  and the Windows SDK (`HOOKSHOT_MSVC=1` forces that route).
- Visual Studio 2022 with .NET 8, only if you want to change the builder itself.
- The prebuilt tools that go in `third_party/`: `a64emit`, `lld-link`, `zbic` and SDL2.
- What the HookshotBuilder zip carries in its `tools` folder and a clone does not. The builder sets the variables
  below itself; from the command line, set them before you run `make_game.py`:
  - `tools\cpm`: the Vulkan headers and FFmpeg. Set `RX_CPM` to this folder. Without it the build leaves out
    `hookshot_vk.dll`, the Vulkan renderer, and says nothing.
  - `tools\wgpu-native`: set `WGPU_NATIVE` to this folder.
  - `tools\third_party\sdl2\SDL2.dll`: copy it to `third_party\sdl2\`.
  - `tools\work`: copy it, with its `image` folder, to `work\`. It holds the hot-function list, the link order, the
    profile and the icon; without them the game builds but runs slower.

### What this does on your PC

- The build reads your game and never changes it. It writes to the folder you choose and to
  `%LOCALAPPDATA%\HookshotBuilder`.
- The romfs is hard-linked instead of copied where the disk allows it.
- To uninstall, delete the game folder and `%LOCALAPPDATA%\HookshotBuilder`.

### If something goes wrong

| Problem | What to do |
|---|---|
| "Hookshot supports build e0f3ccfa" | Check that you chose the right game with its update installed |
| The game opens and closes | Update your graphics driver |
| Stutter the first time you see something | Normal for now: shaders are being prepared. It gets better when you pass through the same place again |
| The game is in the wrong language | Change the code in `language.txt` beside the game. The file lists them all |

If the problem continues, report it with where you were in the game, your graphics card and a screenshot or video.
Never send game files.

## In-game menu

Press <kbd>Insert</kbd> in game, or on a controller the Home button (or Select and Start together). Everything is off
by default, and your choices are saved in `hookshot_options.ini` beside the game. On a controller the D-pad chooses
and changes, the bottom face button changes too, and the right one closes the menu.

| Option | Choices |
|---|---|
| Quality preset | Custom, Minimum (4GB), Low (6GB), Balanced (8GB), High (12GB), Ultra (16GB+) |
| Frame generation | Off, 2x, 3x, 4x |
| Frame pacing | Off, On |
| Resolution | Auto (the game's own dynamic resolution), 100%, 75%, 67%, 50% |
| Upscaling | Off, Catmull-Rom, Lanczos |
| Sharpness | 0 to 100% |
| Game speed | Per frame, Real time |
| Handheld profile | Off, On |
| Ultrawide | 16:9, Monitor's shape |
| FPS counter | Off, On |

With Game speed on Per frame, the game runs slower whenever it is below 60 fps. Real time keeps it at normal speed.
Upscaling and Sharpness also work when Frame generation is Off.

### Controls

Use a controller, or the keyboard and mouse. On the keyboard, <kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> move
and the arrow keys turn the camera.

To change which gamepad button or key presses a Switch button, open the in-game menu's Controls page. Choose a
button and a column, press <kbd>Enter</kbd> (or the bottom face button), then the new button or key; <kbd>Delete</kbd>
clears it, and Reset to defaults puts them all back. A gamepad button taken from another Switch button swaps with it.
The page saves them in `hookshot_options.ini`, where you can also write them yourself:

```ini
pad_a=0
key_a=69,13
```

`pad_<button>` is a gamepad input by position: 0 bottom face button, 1 right, 2 left, 3 top, 4 Select or Back, 5 Home,
6 Start, 7 and 8 the stick presses, 9 and 10 the shoulder buttons, 11 to 14 the D-pad (up, down, left, right), 15 and
16 the triggers, -1 none. `key_<button>` is one or two
[Windows key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) in decimal (69 is
<kbd>E</kbd>, 13 <kbd>Enter</kbd>), 0 none. The buttons are `a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `ls`
`rs` `left` `up` `right` `down`. A button the file leaves out keeps its default.

## Mods

Put mods in a `mods` folder beside `hookshot.exe` (or point `HOS_MODS` at another folder), in the layout emulators
use: `mods/<name>/romfs/...` holds files that replace or add to the game's own. Deeper layouts such as
`mods/contents/<title id>/<name>/romfs` work too. A folder whose name starts with `-` or `_` is off. When two mods
have the same file, the one whose folder name sorts last wins, so name them `01-...`, `02-...` to set the order.

Only RomFS mods are loaded. `exefs` patches (`.pchtxt`, `.ips`) are left out: Hookshot compiles the game's code ahead
of time, so patching its instructions would need a rebuild. The log (`hookshot_log.txt` when started from its icon)
names each mod and the files read from it.

## How it works

1. `nx2/lift.py` translates the game's ARM64 code to C.
2. `nx2/build.py` compiles that C together with `host/`, which plays the part of the console's system.
3. `host/nvn_native.c` records the game's NVN2 graphics calls, and `gpu/vk/` replays them on Vulkan.
4. Switch 2 Ampere shaders are lowered to Maxwell (`lower_to_maxwell.h`) and translated to SPIR-V by Eden's shader
   recompiler.

More detail is in the [development guide](docs/development.md) and the [design notes](docs/design.md).

## Want to help?

Testing on other graphics cards and reporting the result already helps a lot, and so does playing further into the
game than anyone has yet.

Bigger ways to help:

- **Linux testing.** The Linux build (`host/posix`) has been tested on one PC so far: reports from other distributions
  and graphics cards help.
- **Fixes in your own fork.** Fork the project, fix what you find and send the change back. Keep game files, keys and
  anything generated from your game out of it.

[CONTRIBUTING.md](CONTRIBUTING.md) has the setup, the test commands, the code conventions and what a good pull request
looks like.

## Credits

Hookshot exists thanks to the work of many people:

- **NotRexed**: Hookshot, BlueShell (Mario Kart World), Skybound (Tears of the Kingdom) and the a64-kit tools.
- **EvilBoyce** (Discord: evilmfkr, [fork](https://github.com/evilmfkr/Hookshot)): Ocarina of Time's foliage culling, ambient
  light, water and fog fixes, the freeze after Load, the exposure flashes, and the rendering diagnostics tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders and the AArch64 translator.
- **[seedds/nxnative](https://github.com/seedds/nxnative)**: reference for how task and mesh shaders work.
- **[EvilBoyce / evilmfkr](https://github.com/evilmfkr/Hookshot)** (EvilOOT): the GPU culling that brings back Kokiri's foliage
  (trees, ferns, grass, falling leaves, fireflies), the ambient light probes, the pond's water, the game's own fog, the freeze
  after Load (Horizon's mutex handoff) and the exposure flicker fix.
- **[zortos293/Lichii](https://github.com/zortos293/Lichii)**: reference for Ampere shader lowering, and its compare-exchange retry fix.
- **[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**: the bundled Windows compiler (no Visual Studio needed).
- **[kinnay/zbic](https://github.com/kinnay/zbic)**, **Opus** and **SDL**: decompression, audio and controllers.
- **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**: this README's layout, and its notes
  confirmed the mesh stage's control section.
- **[MythosOfs/Orchid](https://github.com/MythosOfs/Orchid)**: the fog and exposure workaround for Ocarina of Time.
- **Mesa (NAK)**: the reference for Ampere's instruction encoding.
- Everyone who sent fixes: SkippySteve (Linux and the AppImage, and many more), klimovich008, d3zessete, chrispository, vzeol,
  monsterlag, leonidasmark, Segmentree, Zorkats, ShyVortex, hiKareeem, gab9281 and i900gaming, among others.

## License

Hookshot is free software under the [GNU General Public License, version 3 or later](LICENSE) (GPL-3.0-or-later).
It includes code from Eden, suyu and Orchid, which are under the GPL too. Third-party parts under other licenses that
work with the GPL (Sirit, the Vulkan Memory Allocator, SDL, Opus, stb, zbic and the a64-kit tools) keep their own notices
in their folders.

Hookshot is an unofficial fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of
Time are trademarks of Nintendo.
