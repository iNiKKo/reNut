# Native renderer: rexglue-sdk setup

The native renderer (`rexgpu-nativevk` plugin) builds against a checkout of
[rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) with one small patch applied,
plus this repo's own build wiring. rexglue-sdk itself needs almost no changes — see
`cmake/patches/rexglue_sdk_native_renderer.patch`'s own diff for exactly what's touched (a
single virtual hook on `VulkanCommandProcessor::IssueDraw` for plugins to substitute a native
pipeline, plus the Vulkan device-feature enablement bindless native pipelines need). Everything
else the native renderer actually needs — the pipeline cache, shader debug panel, Phase 1
draw path — lives entirely in this repo's own `src/graphics/nativevk/`-mirrored files inside the
SDK, added fresh rather than editing existing rexglue-sdk source.

## One-time setup

```sh
# 1. Clone rexglue-sdk at the commit this patch was built against.
git clone https://github.com/rexglue/rexglue-sdk.git /path/to/rexglue-sdk
cd /path/to/rexglue-sdk
git checkout 3eb9b511b4140d2769e27be63eae57d41bfa2afa

# 2. Apply the native-renderer patch (from this repo).
git apply /path/to/reNut/cmake/patches/rexglue_sdk_native_renderer.patch

# 3. Point the reNut build at it.
cmake --preset linux-amd64-relwithdebinfo -DREXSDK_DIR=/path/to/rexglue-sdk
```

## After that

Normal build (`cmake --build --preset ...` or `ninja` from the build dir) picks up
`rexgpu-nativevk` (the native renderer plugin) and `rexgpu-xenos` (the stock, unmodified-behavior
plugin — useful as a clean A/B reference: set `gpu_plugin = "xenos"` in `renut.toml` to compare
against `gpu_plugin = "nativevk"`).

If the patch stops applying cleanly after a rexglue-sdk update, regenerate it from a working
tree with the fix applied: `git diff --binary > cmake/patches/rexglue_sdk_native_renderer.patch`
from inside the rexglue-sdk checkout, and update the commit hash above.
