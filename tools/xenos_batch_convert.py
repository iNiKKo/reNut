#!/usr/bin/env python3
"""Batch-convert a directory of dumped guest shaders to compiled SPIR-V via
XenosRecomp, isolating each shader in its own subprocess so a crash (26/748
real Banjo shaders segfault XenosRecomp's own directory-scan mode, confirmed
2026-08-17 -- see PLAN_native_renderer.md) only drops that one shader instead
of aborting the whole batch.

XenosRecomp's directory-scan mode (see its main.cpp) is what actually invokes
DXC and smol-v-encodes SPIR-V -- single-file mode only emits HLSL text. But
directory mode processes every shader it finds in one process with bare
asserts and no per-shader recovery, so a bad shader takes the whole run down.
This script gets the same directory-mode output (a real multi-shader cache)
by feeding XenosRecomp ONE synthetic single-shader directory per invocation:
copy (well, symlink) exactly one dumped shader into a scratch directory, run
XenosRecomp against that directory, check the exit code, then merge every
successful shader's cache entries into one combined output.

Usage:
    xenos_batch_convert.py <xenos_recomp_binary> <shader_dump_dir>
        <shader_common_header> <output_cpp> [<output_manifest>]

<output_cpp> gets XenosRecomp's own generated shader-cache C++ (the
ShaderCacheEntry table + compressed DXIL/SPIR-V blobs), but built only from
the shaders that converted successfully. <output_manifest>, if given, gets a
plain-text list of "<hash> <status>" lines (ok/crash/error) for visibility
into what got skipped.
"""

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Real ShaderContainer layout (see XenosRecomp/XenosRecomp/shader.h):
#   flags, virtualSize, physicalSize, fieldC, constantTableOffset,
#   definitionTableOffset, shaderOffset, field1C, field20 -- all big-endian
#   uint32. Magic gate: (flags & 0xFFFFFF00) == 0x102A1100.
_MAGIC_MASK = 0xFFFFFF00
_MAGIC_VALUE = 0x102A1100
_CONTAINER_STRUCT = struct.Struct(">IIIIIIIII")  # 9 big-endian uint32 = 36 bytes


def find_containers(data: bytes):
    """Yield (offset, size) for each real ShaderContainer found in data,
    mirroring XenosRecomp main.cpp's own directory-mode scan exactly so we
    isolate the same units it would."""
    i = 0
    n = len(data)
    header_size = _CONTAINER_STRUCT.size
    while i + header_size < n:
        (flags, virtual_size, physical_size, field_c, ctbl_off, dtbl_off,
         shader_off, field1c, field20) = _CONTAINER_STRUCT.unpack_from(data, i)
        data_size = virtual_size + physical_size
        if ((flags & _MAGIC_MASK) == _MAGIC_VALUE and data_size <= (n - i) and
                field1c == 0 and field20 == 0):
            yield i, data_size
            i += data_size
        else:
            i += 4


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__)
        return 1

    xenos_recomp = Path(sys.argv[1])
    dump_dir = Path(sys.argv[2])
    common_header = Path(sys.argv[3])
    output_cpp = Path(sys.argv[4])
    manifest_path = Path(sys.argv[5]) if len(sys.argv) > 5 else None

    if not dump_dir.is_dir():
        print(f"xenos_batch_convert: '{dump_dir}' is not a directory, nothing to do")
        output_cpp.write_text("// no shader dump directory found\n")
        return 0

    bin_files = sorted(dump_dir.glob("*.bin"))
    if not bin_files:
        print(f"xenos_batch_convert: no .bin files in '{dump_dir}', nothing to do")
        output_cpp.write_text("// no shaders found\n")
        return 0

    results = []  # (hash_hex, status)
    good_dir_root = Path(tempfile.mkdtemp(prefix="xenos_batch_ok_"))
    good_shaders_dir = good_dir_root / "shaders"
    good_shaders_dir.mkdir()

    kept = 0
    for bin_file in bin_files:
        data = bin_file.read_bytes()
        containers = list(find_containers(data))
        if not containers:
            continue
        # A dumped .bin is expected to hold exactly one real shader
        # (shader_dump.cpp writes header+ucode for one CreateVertexShader/
        # CreatePixelShader call), but scan generically in case that changes.
        for idx, (offset, size) in enumerate(containers):
            chunk = data[offset:offset + size]
            tag = bin_file.stem if idx == 0 else f"{bin_file.stem}_{idx}"

            with tempfile.TemporaryDirectory(prefix="xenos_batch_try_") as trial_dir:
                trial_path = Path(trial_dir)
                shader_path = trial_path / f"{tag}.bin"
                shader_path.write_bytes(chunk)
                out_path = trial_path / f"{tag}_cache.cpp"

                try:
                    proc = subprocess.run(
                        [str(xenos_recomp), str(trial_path), str(out_path), str(common_header)],
                        capture_output=True, timeout=60)
                except subprocess.TimeoutExpired:
                    results.append((tag, "timeout"))
                    continue

                if proc.returncode != 0:
                    status = "crash" if proc.returncode < 0 else "error"
                    results.append((tag, status))
                    continue

                if not out_path.exists():
                    results.append((tag, "no-output"))
                    continue

                # Success: keep the source shader for the real combined pass
                # below (re-running XenosRecomp once over ALL good shaders
                # together, so the combined cache is one real dedup'd table
                # rather than a hand-merge of N single-shader C++ files).
                shutil.copy2(shader_path, good_shaders_dir / shader_path.name)
                results.append((tag, "ok"))
                kept += 1

    if kept == 0:
        print("xenos_batch_convert: 0/%d shaders converted successfully" % len(results))
        output_cpp.write_text("// all shaders failed to convert\n")
    else:
        # Real, compressed (ZSTD+smol-v) XenosRecomp output -- written
        # directly to output_cpp. Decompressing this into plain SPIR-V words
        # is a SEPARATE build step (tools/xenos_cache_unpack.cpp, chained
        # after this script by renut_xenos_shader_batch() in
        # cmake/rexglue_xenosrecomp.cmake), not done here.
        proc = subprocess.run(
            [str(xenos_recomp), str(good_shaders_dir), str(output_cpp), str(common_header)],
            capture_output=True, timeout=1800)
        if proc.returncode != 0:
            print("xenos_batch_convert: combined pass over %d good shaders FAILED "
                  "unexpectedly (rc=%d) -- stderr:\n%s" % (kept, proc.returncode,
                                                             proc.stderr.decode(errors="replace")))
            return 1

    shutil.rmtree(good_dir_root, ignore_errors=True)

    ok = sum(1 for _, s in results if s == "ok")
    print(f"xenos_batch_convert: {ok}/{len(results)} shaders converted "
          f"({len(results) - ok} skipped: crashes/errors isolated per-shader)")

    if manifest_path is not None:
        with manifest_path.open("w") as f:
            for tag, status in results:
                f.write(f"{tag} {status}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
