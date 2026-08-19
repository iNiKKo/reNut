# Build image for a low-glibc-floor reNut, with the rexglue SDK baked in.
#
# The problem this solves: an AppImage bundles libraries but not libc, so the
# binary inherits whatever glibc the build host has. Building on CachyOS
# (glibc 2.43) yields an AppImage that runs only on CachyOS-era distros. None of
# the >2.28 symbols are features the source asks for -- pthread_rwlock_*@2.34 is
# the libpthread/libc merge, __isoc23_strtoul@2.38 is the C23 strtol family,
# sqrtf@2.43 is the errno-setting variant, strlcat@2.38 is feature-detected by
# SDL -- so they all resolve downward on their own once the build simply happens
# against an older glibc. No source changes are required.
#
# The catch is that an old distro also means an old libstdc++, and this is a
# C++23 codebase. Hence the split: glibc comes from the old base image, while
# the C++ toolchain is layered on top at a modern version. clang++ takes its
# libstdc++ headers from the highest GCC installation it finds, so g++-13
# supplies <expected>/<print>/etc. while libc stays back at the base version.
# packaging/make-appimage.sh then bundles *this image's* libstdc++, so the
# player's machine never needs a matching one.
#
# The SDK is compiled in stage two and copied into the final image, so building
# reNut against it costs nothing -- pull or load the image and the SDK is
# already there. Only bumping SDK_REF requires paying for that stage again.
#
# Default base is Ubuntu 22.04 (glibc 2.35). Override to go lower, e.g.
#   --build-arg BASE=ubuntu:20.04 --build-arg CODENAME=focal \
#   --build-arg PYTHON_VENV=python3.8-venv        (glibc 2.31)
# The further back you go, the likelier apt.llvm.org or the toolchain PPA has
# stopped publishing for that release -- check before assuming a failure is ours.

# ---------------------------------------------------------------- toolchain --
ARG BASE=ubuntu:22.04
FROM ${BASE} AS toolchain

ARG LLVM_VERSION=20
ARG GCC_VERSION=13
# Distro codename, used for the apt.llvm.org, Kitware and LunarG repo URLs.
ARG CODENAME=jammy
# The SDK build wants a venv module: python3.10 on jammy, python3.8 on focal.
ARG PYTHON_VENV=python3.10-venv

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      wget gnupg lsb-release ca-certificates software-properties-common \
 && rm -rf /var/lib/apt/lists/*

# CMake, from Kitware. The distro package is too old -- reNut's CMakeLists.txt
# requires 3.25 and jammy ships 3.22.
RUN wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
      | gpg --dearmor -o /etc/apt/trusted.gpg.d/kitware.gpg \
 && echo "deb https://apt.kitware.com/ubuntu/ ${CODENAME} main" \
      > /etc/apt/sources.list.d/kitware.list

# clang, via the same llvm.sh flow as .github/workflows/_build-platform.yaml.
RUN wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && chmod +x /tmp/llvm.sh \
 && /tmp/llvm.sh ${LLVM_VERSION} \
 && apt-get install -y --no-install-recommends \
      clang-${LLVM_VERSION} lld-${LLVM_VERSION} \
 && update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-${LLVM_VERSION}   200 \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} 200 \
 && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# A C++23-capable libstdc++ on top of the old glibc.
RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update \
 && apt-get install -y --no-install-recommends g++-${GCC_VERSION} \
 && rm -rf /var/lib/apt/lists/*

# Vulkan SDK, from LunarG.
RUN wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc \
      | tee /etc/apt/trusted.gpg.d/lunarg.asc >/dev/null \
 && wget -qO /etc/apt/sources.list.d/lunarg-vulkan-${CODENAME}.list \
      http://packages.lunarg.com/vulkan/lunarg-vulkan-${CODENAME}.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends vulkan-sdk \
 && rm -rf /var/lib/apt/lists/*

# Build dependencies, mirroring the CI workflow's list, plus imagemagick and
# file for packaging/make-appimage.sh.
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build build-essential git curl unzip zip autoconf ${PYTHON_VENV} \
      libgtk-3-dev libx11-xcb-dev libxss-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev \
      imagemagick file desktop-file-utils \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++

# -------------------------------------------------------------------- sdk ----
# Expensive and cached: this layer only reruns when SDK_REF changes.
FROM toolchain AS sdk

ARG SDK_REPO=https://github.com/rexglue/rexglue-sdk.git
ARG SDK_REF=nightly-20260809-f5c85215

RUN git clone --recursive --depth 1 --branch ${SDK_REF} ${SDK_REPO} /tmp/rexglue-sdk \
 && cd /tmp/rexglue-sdk \
 && cmake --preset linux-amd64 \
 && cmake --build --preset linux-amd64-relwithdebinfo --target install --parallel \
 && mkdir -p /opt \
 && mv out/install/linux-amd64 /opt/rexglue-sdk \
 && rm -rf /tmp/rexglue-sdk

# ----------------------------------------------------------- extract-xiso ----
# Bundled into the AppImage so the first-run flow in packaging/AppRun can pull
# game data out of a disc image without the user installing anything.
#
# This fork rather than XboxDev/extract-xiso: upstream targets original Xbox
# XISO images and documents no XGD2/XGD3 support, where 360 discs put the
# XDVDFS header at a different offset. This one carries somsky's XGD3 patches.
FROM toolchain AS extract-xiso

ARG XISO_REPO=https://github.com/rikyperdana/extract-xiso.git
ARG XISO_REF=master

# The repo ships a prebuilt binary and that is what we use. Building from source
# here fails anyway -- the bundled libftp is compiled without -fPIE while
# Ubuntu's gcc defaults to PIE, so the link dies on an R_X86_64_32 relocation.
RUN git clone --depth 1 --branch ${XISO_REF} ${XISO_REPO} /tmp/extract-xiso \
 && install -Dm755 /tmp/extract-xiso/extract-xiso /opt/extract-xiso/extract-xiso \
 && rm -rf /tmp/extract-xiso

# A prebuilt binary carries whatever glibc it was linked against, so check it
# against this image rather than trusting it. Today it needs only 2.15, well
# under any base we would pick, but a future update to the fork could silently
# raise the AppImage's floor and undo the point of this whole image.
#
# Kept as its own RUN, and written with if/exit rather than a && || chain, so a
# failure stops the build here instead of surfacing as a confusing COPY error in
# the final stage.
RUN need=$(objdump -T /opt/extract-xiso/extract-xiso \
             | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -n 1); \
    have=$(ldd --version | head -n 1 | grep -oE '[0-9]+\.[0-9]+$'); \
    if [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | tail -n 1)" != "$have" ]; then \
        echo "error: extract-xiso needs glibc $need, image provides $have" >&2; \
        exit 1; \
    fi; \
    echo "extract-xiso ok: needs glibc $need, image provides $have"

# ------------------------------------------------------------------ final ----
FROM toolchain AS final

# The built SDK only, without the source tree or object files behind it.
COPY --from=sdk /opt/rexglue-sdk /opt/rexglue-sdk
COPY --from=extract-xiso /opt/extract-xiso /opt/extract-xiso

# Where reNut's generated/rexglue.cmake find_package() call will look.
ENV CMAKE_PREFIX_PATH=/opt/rexglue-sdk
# appimagetool is itself an AppImage and there is no FUSE in here.
ENV APPIMAGE_EXTRACT_AND_RUN=1

WORKDIR /src/reNut
