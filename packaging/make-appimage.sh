#!/usr/bin/env bash
#
# Builds dist/reNut-x86_64.AppImage from an existing reNut build.
#
# Prerequisites: a completed build (linux-amd64-release is preferred over
# relwithdebinfo, though this script strips either), ImageMagick for the icon,
# and network access on first run to fetch appimagetool.
#
# Usage:
#   packaging/make-appimage.sh [build-dir]
#   RENUT_LOWER_GLIBC=1 packaging/make-appimage.sh    # see "glibc floor" below
#
# Why this needs no LD_LIBRARY_PATH or library-rewriting:
#   renut is linked with RUNPATH=$ORIGIN, so the three rexglue .so files are
#   found simply by sitting next to the binary -- which is also where the GPU
#   plugin is dlopen'd from. libstdc++/libgcc ride along the same way (see
#   "C++ runtime floor" below); everything else it links (libc, X11, xcb) is
#   deliberately left to the host.
#
# Why it works on a read-only mount:
#   renut writes its config to $XDG_CONFIG_HOME/renut and its logs to
#   $XDG_STATE_HOME/renut (see src/renut_engine/linuxfixes/xdg_paths.h). Before
#   that change it wrote both next to the executable, which cannot work inside
#   an AppImage.
#
# There are two portability floors here, and they move independently.
#
# C++ runtime floor (handled unconditionally, below):
#   clang++ compiles against whichever libstdc++ headers the build host's GCC
#   installs, so on a bleeding-edge host librexruntimerd.so ends up calling
#   GLIBCXX_3.4.35 symbols -- the C++20 atomic-wait machinery behind
#   std::atomic::wait/latch/barrier, which only a GCC 15-or-newer libstdc++
#   exports. That is a *stricter* limit than the glibc one, and it is easy to
#   miss: Ubuntu 24.04 and Debian 13 both have new enough glibc but too old a
#   libstdc++. So libstdc++.so.6 and libgcc_s.so.1 are staged next to renut and
#   resolved through the same RUNPATH=$ORIGIN. Bundling a *newer* libstdc++ is
#   safe -- Mesa/GL drivers dlopen'd later need older versions and a newer one
#   satisfies them. The reverse would not hold, so never bundle an older one.
#
# glibc floor:
#   The resulting AppImage inherits the build host's glibc requirement -- an
#   AppImage bundles libraries, not libc. Building on a bleeding-edge distro
#   therefore produces an AppImage that only runs on bleeding-edge distros.
#   Setting RENUT_LOWER_GLIBC=1 runs polyfill-glibc to remap five float math
#   symbols (sqrtf/acosf/asinf/atan2f/log10f) from their GLIBC_2.43 versions to
#   the ABI-identical GLIBC_2.2.5 ones, which lowers the floor to 2.38 with no
#   rebuild. Going below 2.38 currently does not work: polyfill-glibc crashes on
#   librexruntimerd.so when targeting 2.34+, and targeting 2.32 yields a renut
#   that segfaults in its own static initialisers. None of the symbols above
#   2.28 are features the source actually asks for -- they are all picked by the
#   build host -- so the real fix for anything lower is to build against an older
#   glibc instead of rewriting symbols afterwards. See packaging/container/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/build/linux-amd64-relwithdebinfo}"
WORK_DIR="$REPO_ROOT/out/appimage"
APP_DIR="$WORK_DIR/renut.AppDir"
DIST_DIR="$REPO_ROOT/dist"
OUTPUT="$DIST_DIR/reNut-x86_64.AppImage"

LIBS=(librexruntimerd.so librexgpu-xenosrd.so libTracyClientrd.so)

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

[[ -x "$BUILD_DIR/renut" ]] || die "no renut binary in $BUILD_DIR (build first)"
for lib in "${LIBS[@]}"; do
  [[ -f "$BUILD_DIR/$lib" ]] || die "missing $lib in $BUILD_DIR"
done
command -v magick >/dev/null 2>&1 || command -v convert >/dev/null 2>&1 \
  || die "ImageMagick is required to convert icon/app.ico"

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/usr/bin" "$DIST_DIR"

echo "==> staging binaries"
cp "$BUILD_DIR/renut" "$APP_DIR/usr/bin/"
for lib in "${LIBS[@]}"; do cp "$BUILD_DIR/$lib" "$APP_DIR/usr/bin/"; done

echo "==> stripping"
strip --strip-unneeded "$APP_DIR/usr/bin/renut" "$APP_DIR/usr/bin/"*.so 2>/dev/null || true

if [[ "${RENUT_LOWER_GLIBC:-0}" == "1" ]]; then
  echo "==> lowering glibc floor (2.43 -> 2.38)"
  command -v polyfill-glibc >/dev/null 2>&1 \
    || die "RENUT_LOWER_GLIBC=1 needs polyfill-glibc on PATH (github.com/corsix/polyfill-glibc)"
  renames="$WORK_DIR/renames.txt"
  cat > "$renames" <<'RENAMES'
// glibc 2.43 added new symbol versions for these float math functions. The
// GLIBC_2.2.5 versions are ABI-identical (they additionally set errno, which is
// what every binary built before 2.43 already relied on), so binding to them is
// safe and removes the hard 2.43 requirement.
sqrtf@GLIBC_2.43   libm.so.6::sqrtf@GLIBC_2.2.5
acosf@GLIBC_2.43   libm.so.6::acosf@GLIBC_2.2.5
asinf@GLIBC_2.43   libm.so.6::asinf@GLIBC_2.2.5
atan2f@GLIBC_2.43  libm.so.6::atan2f@GLIBC_2.2.5
log10f@GLIBC_2.43  libm.so.6::log10f@GLIBC_2.2.5
RENAMES
  for f in "$APP_DIR/usr/bin/renut" "$APP_DIR/usr/bin/"*.so; do
    polyfill-glibc --rename-dynamic-symbols="$renames" "$f"
  done
fi

echo "==> bundling C++ runtime"
# Deliberately staged after the strip and polyfill-glibc loops above: those glob
# *.so, which does not match *.so.N, so these arrive verbatim as the toolchain
# shipped them. They need at most GLIBC_2.38 themselves (libstdc++ 2.38, libgcc
# 2.35), so they never raise the floor reported at the end.
for lib in libstdc++.so.6 libgcc_s.so.1; do
  src="$("${CXX:-clang++}" -print-file-name="$lib")"
  [[ -f "$src" ]] || die "could not locate $lib via ${CXX:-clang++} -print-file-name"
  cp -L "$src" "$APP_DIR/usr/bin/$lib"
done

echo "==> bundling extract-xiso"
# Used by AppRun's first-run flow to extract game data from a disc image the
# user owns. Optional: AppRun skips that flow entirely when the binary is
# absent, falling back to renut's built-in path wizard. Prefer the copy the
# build container produces, since a host-built one would carry the host's glibc
# requirement into the AppImage.
xiso=""
if [[ -x /opt/extract-xiso/extract-xiso ]]; then
  xiso=/opt/extract-xiso/extract-xiso
elif command -v extract-xiso >/dev/null 2>&1; then
  xiso="$(command -v extract-xiso)"
fi
if [[ -n "$xiso" ]]; then
  cp "$xiso" "$APP_DIR/usr/bin/extract-xiso"
  strip --strip-unneeded "$APP_DIR/usr/bin/extract-xiso" 2>/dev/null || true
else
  echo "    warning: extract-xiso not found, first-run ISO extraction will be"
  echo "             unavailable in this build (the in-app wizard still works)"
fi

echo "==> icon"
ico="$REPO_ROOT/icon/app.ico"
if command -v magick >/dev/null 2>&1; then
  magick "$ico[0]" -resize 256x256 "$APP_DIR/renut.png"
else
  convert "$ico[0]" -resize 256x256 "$APP_DIR/renut.png"
fi
cp "$APP_DIR/renut.png" "$APP_DIR/.DirIcon"

echo "==> AppRun + desktop entry"
# Kept as a real file rather than a heredoc so it can be shellchecked and diffed
# on its own. It handles the first-run disc extraction as well as the launch.
cp "$REPO_ROOT/packaging/AppRun" "$APP_DIR/AppRun"
chmod +x "$APP_DIR/AppRun"

cat > "$APP_DIR/renut.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=reNut
Comment=Xbox 360 game recompilation
Exec=renut
Icon=renut
Categories=Game;
Terminal=false
DESKTOP

echo "==> fetching appimagetool if needed"
tool="$WORK_DIR/appimagetool"
if [[ ! -x "$tool" ]]; then
  curl -sSL -o "$tool" \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$tool"
fi

echo "==> packaging"
ARCH=x86_64 "$tool" "$APP_DIR" "$OUTPUT"

echo
echo "built: $OUTPUT"
echo "  size:        $(du -h "$OUTPUT" | cut -f1)"
# Every staged file, bundled C++ runtime included -- all of it has to load on the
# target. GLIBC_ cannot match GLIBCXX_ here, since the pattern needs a digit
# straight after the underscore.
echo "  glibc floor: $(objdump -T "$APP_DIR/usr/bin/"* 2>/dev/null \
                        | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
echo "  libstdc++:   bundled, $(objdump -T "$APP_DIR/usr/bin/libstdc++.so.6" 2>/dev/null \
                        | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | tail -1) (host floor removed)"
