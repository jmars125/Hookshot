#!/bin/bash
# What the Linux AppImage carries, fetched and built into build-linux/deps (or $DEPS): each part once, kept while its
# version stays the same. Run on the oldest system the AppImage is for (CI: Ubuntu 22.04, glibc 2.35).
#
#   emsdk/upstream/   LLVM's clang and lld (the layout third_party/a64-kit/tools/toolchain.py finds through EMSDK):
#                     the player's PC compiles the game with them. The play profile (work/hookshot_pieces.prof) wants
#                     LLVM 23 or newer.
#   python/           CPython (python-build-standalone) with lz4, numpy, capstone, unicorn and zstandard: the tools
#   ffmpeg/           FFmpeg's libavcodec, libswscale, libswresample, libavutil, static and LGPL, with dav1d for OoT's
#                     AV1 movies (VP8 / VP9 too) - linked into the game
#   sdl2/             libSDL2-2.0.so.0: the window, sound and controllers, placed beside the game
#   cpm/              Vulkan-Headers, SPIRV-Headers and VulkanMemoryAllocator, in the CPM cache layout nx2/build.py reads
#   appimagetool      the AppImage packer
set -euo pipefail
cd "$(dirname "$0")/../.."
DEPS=${DEPS:-$PWD/build-linux/deps}
DL=$DEPS/downloads
mkdir -p "$DEPS" "$DL"
JOBS=$(nproc)

LLVM_VER=23.1.5
PY_TAG=20261026
PY_VER=3.12.16
SDL_VER=2.32.10
FFMPEG_VER=9.0.2
DAV1D_VER=1.5.4
VK_HEADERS=v1.4.345
SPIRV_HEADERS=vulkan-sdk-1.4.341.0
VMA=v3.3.0
APPIMAGETOOL=1.9.1

fetch() {   # url file
    if [ ! -f "$DL/$2" ]; then
        curl -fL --retry 3 --proto '=https' -o "$DL/$2.part" "$1"
        mv -f "$DL/$2.part" "$DL/$2"
    fi
}
done_for() { [ -f "$DEPS/.$1" ] && [ "$(cat "$DEPS/.$1")" = "$2" ]; }
mark() { echo "$2" > "$DEPS/.$1"; }

# ---- LLVM: clang, lld and clang's own headers, nothing else ----
if ! done_for llvm "$LLVM_VER"; then
    fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/LLVM-$LLVM_VER-Linux-X64.tar.xz" llvm.tar.xz
    rm -rf "$DEPS/emsdk" "$DEPS/llvm-x"
    mkdir -p "$DEPS/llvm-x"
    tar -xJf "$DL/llvm.tar.xz" -C "$DEPS/llvm-x" --strip-components=1 --wildcards \
        '*/bin/clang' '*/bin/clang++' '*/bin/clang-[0-9]*' '*/bin/lld' '*/bin/ld.lld' '*/bin/llvm-profdata' '*/lib/clang/*'
    U=$DEPS/emsdk/upstream
    mkdir -p "$U/bin" "$U/lib"
    mv "$DEPS/llvm-x/bin/"* "$U/bin/"
    mv "$DEPS/llvm-x/lib/clang" "$U/lib/clang"
    rm -rf "$DEPS/llvm-x"
    # (the runtimes: only the profile and builtins libraries; no sanitizers)
    find "$U"/lib/clang/*/lib -type f ! -name 'libclang_rt.profile*.a' ! -name 'libclang_rt.builtins*.a' -delete
    strip "$U/bin/clang-"[0-9]* "$U/bin/lld" "$U/bin/llvm-profdata" 2>/dev/null || true
    "$U/bin/clang" --version | head -1
    [ -f "$U/lib/clang/${LLVM_VER%%.*}/include/stddef.h" ] || { echo "clang's own headers are missing"; exit 1; }
    mark llvm "$LLVM_VER"
fi

# ---- Python and the tools' modules ----
if ! done_for python "$PY_VER-$PY_TAG"; then
    fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_TAG/cpython-$PY_VER+$PY_TAG-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz" python.tar.gz
    rm -rf "$DEPS/python"
    tar -xzf "$DL/python.tar.gz" -C "$DEPS"
    "$DEPS/python/bin/python3" -m pip install --no-cache-dir --only-binary :all: --disable-pip-version-check -q \
        "lz4==4.4.5" "numpy==2.5.4" "capstone==5.0.9" "unicorn==2.1.4" "zstandard==0.25.0"
    rm -rf "$DEPS/python/lib/python3.12/test" "$DEPS/python/lib/python3.12/idlelib" "$DEPS/python/lib/python3.12/ensurepip"
    find "$DEPS/python" -name __pycache__ -prune -exec rm -rf {} +
    mark python "$PY_VER-$PY_TAG"
fi

# ---- dav1d and FFmpeg (static, position independent: linked into the game's executable) ----
P=$DEPS/ffmpeg
if ! done_for ffmpeg "$FFMPEG_VER-$DAV1D_VER"; then
    rm -rf "$P" "$DEPS/src-dav1d" "$DEPS/src-ffmpeg"
    fetch "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VER/dav1d-$DAV1D_VER.tar.gz" dav1d.tar.gz
    mkdir -p "$DEPS/src-dav1d" && tar -xzf "$DL/dav1d.tar.gz" -C "$DEPS/src-dav1d" --strip-components=1
    (cd "$DEPS/src-dav1d" && meson setup build --prefix="$P" --libdir=lib --buildtype=release --default-library=static \
        -Db_staticpic=true -Denable_tools=false -Denable_tests=false -Denable_examples=false && ninja -C build install)
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" ffmpeg.tar.xz
    mkdir -p "$DEPS/src-ffmpeg" && tar -xJf "$DL/ffmpeg.tar.xz" -C "$DEPS/src-ffmpeg" --strip-components=1
    (cd "$DEPS/src-ffmpeg" && PKG_CONFIG_PATH="$P/lib/pkgconfig" ./configure --prefix="$P" --pkg-config-flags=--static \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-everything --disable-avformat --disable-avfilter --disable-avdevice \
        --enable-avcodec --enable-swscale --enable-swresample --enable-avutil \
        --enable-libdav1d --enable-decoder=vp8,vp9,libdav1d --enable-parser=vp8,vp9,av1 \
        && make -j"$JOBS" && make install)
    rm -rf "$DEPS/src-dav1d" "$DEPS/src-ffmpeg" "$P/share" "$P/lib/pkgconfig"
    mark ffmpeg "$FFMPEG_VER-$DAV1D_VER"
fi

# ---- SDL2 (its X11, Wayland and sound back ends load their libraries at run time) ----
if ! done_for sdl2 "$SDL_VER"; then
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" sdl2.tar.gz
    rm -rf "$DEPS/src-sdl2" "$DEPS/sdl2"
    mkdir -p "$DEPS/src-sdl2" && tar -xzf "$DL/sdl2.tar.gz" -C "$DEPS/src-sdl2" --strip-components=1
    cmake -S "$DEPS/src-sdl2" -B "$DEPS/src-sdl2/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_ALSA=ON
    ninja -C "$DEPS/src-sdl2/build"
    mkdir -p "$DEPS/sdl2"
    cp -L "$DEPS/src-sdl2/build/libSDL2-2.0.so.0" "$DEPS/sdl2/libSDL2-2.0.so.0"
    strip "$DEPS/sdl2/libSDL2-2.0.so.0"
    cp "$DEPS/src-sdl2/LICENSE.txt" "$DEPS/sdl2/LICENSE.txt"
    rm -rf "$DEPS/src-sdl2"
    mark sdl2 "$SDL_VER"
fi

# ---- the Vulkan back end's headers ----
if ! done_for cpm "$VK_HEADERS-$SPIRV_HEADERS-$VMA"; then
    rm -rf "$DEPS/cpm"
    get_headers() {   # repo tag name
        fetch "https://github.com/$1/archive/refs/tags/$2.tar.gz" "$3-$2.tar.gz"
        mkdir -p "$DEPS/cpm/$3/$2"
        tar -xzf "$DL/$3-$2.tar.gz" -C "$DEPS/cpm/$3/$2" --strip-components=1
    }
    get_headers KhronosGroup/Vulkan-Headers "$VK_HEADERS" vulkanheaders
    get_headers KhronosGroup/SPIRV-Headers "$SPIRV_HEADERS" spirv-headers
    get_headers GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA" vulkanmemoryallocator
    mark cpm "$VK_HEADERS-$SPIRV_HEADERS-$VMA"
fi

# ---- the licenses of what the AppImage carries (Python's and its modules' are in python/) ----
if ! done_for licenses "$LLVM_VER-$FFMPEG_VER-$DAV1D_VER-$SDL_VER"; then
    L=$DEPS/licenses
    rm -rf "$L" && mkdir -p "$L"
    curl -fsSL -o "$L/LLVM-LICENSE.TXT" "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VER/llvm/LICENSE.TXT"
    curl -fsSL -o "$L/FFmpeg-COPYING.LGPLv2.1" "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n$FFMPEG_VER/COPYING.LGPLv2.1"
    curl -fsSL -o "$L/FFmpeg-LICENSE.md" "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n$FFMPEG_VER/LICENSE.md"
    curl -fsSL -o "$L/dav1d-COPYING" "https://code.videolan.org/videolan/dav1d/-/raw/$DAV1D_VER/COPYING"
    cp "$DEPS/sdl2/LICENSE.txt" "$L/SDL2-LICENSE.txt"
    mark licenses "$LLVM_VER-$FFMPEG_VER-$DAV1D_VER-$SDL_VER"
fi

# ---- appimagetool ----
if ! done_for appimagetool "$APPIMAGETOOL"; then
    fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL/appimagetool-x86_64.AppImage" appimagetool
    cp "$DL/appimagetool" "$DEPS/appimagetool" && chmod +x "$DEPS/appimagetool"
    mark appimagetool "$APPIMAGETOOL"
fi
echo "deps in $DEPS: $(du -sh --exclude=downloads "$DEPS" | cut -f1)"
