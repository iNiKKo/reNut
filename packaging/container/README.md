# Low-glibc build container

Builds reNut against an old glibc so the AppImage runs on distros other than
bleeding-edge ones. The rexglue SDK is compiled **into the image**, so building
reNut against it is cheap and nobody has to build the SDK themselves.

## Why this exists

An AppImage bundles libraries but not libc, so the binary inherits whatever
glibc the build host has. Built on CachyOS (glibc 2.43), the result runs on
almost nothing else.

There were two separate floors, and they move independently:

| Floor | Cause | Fix |
| --- | --- | --- |
| `GLIBC_2.43` | build host's libc | this container |
| `GLIBCXX_3.4.35` | build host's libstdc++ headers | bundled by `make-appimage.sh` |

The second one is easy to miss and was in practice the stricter of the two:
`librexruntimerd.so` called the C++20 atomic-wait symbols behind
`std::atomic::wait`/`latch`/`barrier`, which only a GCC 15-or-newer libstdc++
exports. Ubuntu 24.04 and Debian 13 have new enough glibc but too old a
libstdc++, so they failed regardless of the glibc number.
`packaging/make-appimage.sh` now stages `libstdc++.so.6` and `libgcc_s.so.1`
next to the binary unconditionally, which removes that floor everywhere —
including on host builds that never touch this container.

None of the glibc symbols above 2.28 are features the source actually asks for.
`pthread_rwlock_*@2.34` is the libpthread/libc merge, `__isoc23_strtoul@2.38` is
the C23 strtol family, `sqrtf@2.43` is the errno-setting variant, and
`strlcat@2.38` is feature-detected by SDL. They are all chosen by the build
host, which is why building against an older glibc fixes them with no source
changes — and why the `polyfill-glibc` symbol rewriting in `make-appimage.sh`
was only ever treating symptoms.

## Usage

```sh
packaging/container/build.sh image      # once: toolchain + SDK (slow)
packaging/container/build.sh all        # per change: build + package (fast)
```

Output lands in `dist/reNut-x86_64.AppImage`. The container builds into
`out/container/` rather than `out/`, so it will not fight with a host build over
the same CMake cache.

Packaging runs *inside* the container deliberately. `make-appimage.sh` bundles
whichever libstdc++ `$CXX` points at, so running that step on the host would
re-import the host's newer libstdc++ and undo the exercise.

## Sharing the image

Contributors should never build the SDK. Publish the image once and have them
pull it:

```sh
docker tag renut-build:jammy ghcr.io/<owner>/renut-build:jammy
docker push ghcr.io/<owner>/renut-build:jammy
```

```sh
RENUT_IMAGE=ghcr.io/<owner>/renut-build:jammy packaging/container/build.sh all
```

Or, without a registry: `docker save renut-build:jammy | zstd > renut-build.tar.zst`
and `zstd -d < renut-build.tar.zst | docker load`.

Rebuild the image only when `SDK_REF` changes — that is the one argument that
invalidates the expensive layer.

## Choosing the floor

Default is Ubuntu 22.04, giving **glibc 2.35**: Ubuntu 22.04+, Debian 12+,
Fedora 36+, and current SteamOS. To go lower:

```sh
BASE=ubuntu:20.04 CODENAME=focal PYTHON_VENV=python3.8-venv \
  RENUT_IMAGE=renut-build:focal packaging/container/build.sh image
```

That targets glibc 2.31 and covers essentially every distro still receiving
updates. The tradeoff is that the further back you go, the likelier
`apt.llvm.org` or the `ubuntu-toolchain-r` PPA has stopped publishing for that
release — check that before assuming a failure is ours.

One confusing detail if you go looking: the bundled `libstdc++.so.6` reports
`GLIBCXX_3.4.35`, which is *not* evidence that a newer host libstdc++ leaked in.
Installing `g++-13` also pulls the toolchain PPA's libstdc++6 runtime, which is
built from a newer GCC but still against the base image's glibc. Compiled code
needs 3.4.32 (GCC 13's headers) and the bundled runtime provides 3.4.35, so it
is satisfied. The reliable provenance check is the *glibc* requirement of the
bundled library — 2.34 from the container, versus 2.38 from a CachyOS host.

Verify what you actually produced:

```sh
objdump -T out/container/build/linux-amd64-relwithdebinfo/renut \
  | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
```

## Unrelated floor worth knowing

The SDK presets set `-march=x86-64-v2`, which is a *CPU* requirement (roughly
Nehalem and newer), not a distro one. Nothing here changes it.
