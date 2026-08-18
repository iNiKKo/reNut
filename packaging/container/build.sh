#!/usr/bin/env bash
#
# Builds reNut against an old glibc inside Docker and packages the AppImage.
#
# The SDK is already compiled inside the image (see Dockerfile), so nobody has
# to build it -- the first 'image' run pays for it once, or you skip even that
# by loading an image someone else published.
#
# Usage:
#   packaging/container/build.sh image      # build the toolchain+SDK image
#   packaging/container/build.sh build      # compile reNut inside it
#   packaging/container/build.sh appimage   # package dist/reNut-x86_64.AppImage
#   packaging/container/build.sh all        # build + appimage
#   packaging/container/build.sh shell      # interactive shell in the image
#
# Env:
#   RENUT_IMAGE   image tag                       (default renut-build:jammy)
#   BASE          base image                      (default ubuntu:22.04)
#   CODENAME      matching distro codename        (default jammy)
#   PYTHON_VENV   matching venv package           (default python3.10-venv)
#   SDK_REF       rexglue-sdk git tag/branch/SHA  (default pinned below)
#
# The container writes to out/container/ rather than out/, so it never fights
# with a host build over the same CMake cache -- the two use different compilers
# and different sysroots, and a shared cache would produce confusing failures.
# dist/ is shared, because that is the artifact you actually ship.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${RENUT_IMAGE:-renut-build:jammy}"
BASE="${BASE:-ubuntu:22.04}"
CODENAME="${CODENAME:-jammy}"
PYTHON_VENV="${PYTHON_VENV:-python3.10-venv}"
SDK_REF="${SDK_REF:-nightly-20260809-f5c85215}"

PRESET=linux-amd64-relwithdebinfo
CONTAINER_OUT="$REPO_ROOT/out/container"

# The runtime shared libraries, which the build does not place next to the
# executable itself. renut has RUNPATH=$ORIGIN and make-appimage.sh reads them
# out of the build directory, so they are copied there after each build.
LIBS=(librexruntimerd.so librexgpu-xenosrd.so libTracyClientrd.so)

die() { printf 'error: %s\n' "$1" >&2; exit 1; }

DOCKER="${DOCKER:-docker}"

command -v "$DOCKER" >/dev/null 2>&1 \
  || die "docker is not installed (Arch/CachyOS: sudo pacman -S docker && sudo systemctl enable --now docker)"

$DOCKER info >/dev/null 2>&1 || die "cannot reach the docker daemon.
  Either add yourself to the docker group (note that this grants root-equivalent
  access to the machine):
      sudo usermod -aG docker \$USER   # then log out and back in, or: newgrp docker
  or run this script under sudo, which is handled:
      sudo -E $0 ${1:-all}"

# Under sudo, id -u is 0, which would leave root-owned artifacts in the source
# tree. SUDO_UID/SUDO_GID name the invoking user, so prefer them when present.
RUN_UID="${SUDO_UID:-$(id -u)}"
RUN_GID="${SUDO_GID:-$(id -g)}"

have_image() { $DOCKER image inspect "$IMAGE" >/dev/null 2>&1; }

# 'all' deliberately does not rebuild the image -- that is the slow step and it
# rarely needs redoing. The cost is that a Dockerfile edit silently does nothing
# until someone reruns 'image', so say so rather than let it pass unnoticed.
warn_if_image_stale() {
  local created created_epoch dockerfile_epoch
  created=$($DOCKER image inspect -f '{{.Created}}' "$IMAGE" 2>/dev/null) || return 0
  created_epoch=$(date -d "$created" +%s 2>/dev/null) || return 0
  dockerfile_epoch=$(stat -c %Y "$REPO_ROOT/packaging/container/Dockerfile" 2>/dev/null) || return 0
  if (( dockerfile_epoch > created_epoch )); then
    printf 'warning: Dockerfile has changed since %s was built.\n' "$IMAGE" >&2
    printf "         run '%s image' to pick up those changes.\n" "$0" >&2
  fi
}

build_image() {
  echo "==> building $IMAGE (base=$BASE, sdk=$SDK_REF)"
  echo "    the SDK compiles in here; expect this to take a while the first time"
  $DOCKER build \
    --build-arg "BASE=$BASE" \
    --build-arg "CODENAME=$CODENAME" \
    --build-arg "PYTHON_VENV=$PYTHON_VENV" \
    --build-arg "SDK_REF=$SDK_REF" \
    -t "$IMAGE" \
    "$REPO_ROOT/packaging/container"
}

# Runs as the invoking user so build artifacts are not left root-owned on the
# host. That leaves HOME pointing at a directory this user cannot write, which
# CMake and git both dislike, so redirect it.
run_in_container() {
  mkdir -p "$CONTAINER_OUT"
  # Under sudo the mkdir above runs as root, but the container runs as the
  # invoking user and would then have nowhere to write. Hand the directory over.
  # Recursive so a directory left behind by an earlier run repairs itself.
  if [[ -n "${SUDO_UID:-}" ]]; then
    chown -R "$RUN_UID:$RUN_GID" "$CONTAINER_OUT"
  fi
  $DOCKER run --rm -i \
    --user "$RUN_UID:$RUN_GID" \
    -e HOME=/tmp \
    -v "$REPO_ROOT":/src/reNut \
    -v "$CONTAINER_OUT":/src/reNut/out \
    -w /src/reNut \
    "$@"
}

cmd_build() {
  have_image || die "image $IMAGE not found -- run '$0 image' first"
  warn_if_image_stale
  echo "==> configuring and building reNut (glibc floor set by $BASE)"
  run_in_container "$IMAGE" bash -euc "
    cmake --preset $PRESET -DCMAKE_PREFIX_PATH=/opt/rexglue-sdk
    cmake --build --preset $PRESET --parallel
    for lib in ${LIBS[*]}; do
      cp /opt/rexglue-sdk/lib/\$lib out/build/$PRESET/\$lib
    done
  "
}

cmd_appimage() {
  have_image || die "image $IMAGE not found -- run '$0 image' first"
  warn_if_image_stale
  [[ -x "$CONTAINER_OUT/build/$PRESET/renut" ]] \
    || die "no container-built renut -- run '$0 build' first"
  echo "==> packaging AppImage"
  # Packaged inside the container on purpose: make-appimage.sh bundles the
  # libstdc++ that ${CXX} points at, and that has to be the image's GCC 13 one.
  # Running this step on the host would re-import the host's glibc 2.38
  # libstdc++ and undo the whole exercise.
  run_in_container "$IMAGE" \
    packaging/make-appimage.sh "/src/reNut/out/build/$PRESET"
}

case "${1:-all}" in
  image)    build_image ;;
  build)    cmd_build ;;
  appimage) cmd_appimage ;;
  all)      cmd_build; cmd_appimage ;;
  shell)    have_image || die "image $IMAGE not found -- run '$0 image' first"
            run_in_container -t "$IMAGE" bash ;;
  *)        die "unknown command '${1}' (image|build|appimage|all|shell)" ;;
esac
