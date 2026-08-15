#!/usr/bin/env python3
"""Builds a synthetic XenosRecomp ShaderContainer .bin for a shader that was
never captured by shader_dump.cpp's dump_guest_shaders hook (D3D9
CreateVertexShader/CreatePixelShader), because it was loaded directly into
GPU sequencer memory via IM_LOAD/IM_LOAD_IMMEDIATE PM4 packets -- confirmed
real, structural gap, not a capture-timing bug (see PLAN_native_renderer.md).

Real raw microcode for such shaders IS available via the SDK's
renut_dump_ucode_hash cvar (shaders_ucode_hash/<vs|ps>_<hash>.ucode), keyed
by the real ucode_data_hash(). This script wraps that real microcode in a
minimal, hand-built ShaderContainer.

Automated end to end (2026-08-15 generalization): all per-shader vertex-fetch
and texture-fetch metadata (element count, data format, byte offset,
instruction address, sampler fetch-constant/register mapping, interpolator
count, color-output mask) is obtained by running tools/ucode_analyze -- a
standalone binary linking the SDK's own real Shader::AnalyzeUcode() -- against
the real .ucode file, NOT hardcoded per-shader tables. See
tools/ucode_analyze.cpp / tools/build_ucode_analyze.sh.

The one piece AnalyzeUcode cannot supply is D3D9 vertex-element Usage/
UsageIndex (POSITION vs TEXCOORD0 vs COLOR etc.) -- Xenos vfetch instructions
carry no D3D9 semantic name at all, confirmed by direct SDK source
inspection (no DeclUsage/D3DDECLUSAGE inference exists anywhere in this SDK
fork). This is now solved with a second REAL capture, not a guess: the
shader_dump.cpp SetVertexShader/SetVertexDeclaration hooks (new this
session) correlate the guest's own real D3DVERTEXELEMENT9 Usage/UsageIndex
bytes to the exact ucode_data_hash of whichever vertex shader was bound at
the time, writing shaders_vertdecl/vs_<hash>.vertdecl.txt keyed by real
per-element byte offset (matches vertex_bindings()'s offset_words*4 exactly,
no positional assumption). If that file isn't present yet (declaration never
observed bound to this shader in a real play session), this script falls
back to the same documented POSITION-first/TEXCOORD-follows heuristic used
for the one shader pair proven by hand earlier this session -- flagged
clearly in its output, not silently treated as equally trustworthy.

Usage: build_synthetic_container.py <ucode_dir> <vs|ps> <hash> <output.bin>
         [--vertdecl-dir DIR] [--ucode-analyze PATH]
  <ucode_dir>: directory containing <vs|ps>_<hash>.ucode files
  <vs|ps>: which shader type to build (vertex or pixel container layout differ)
  <hash>: the target shader's real ucode_data_hash, hex, no 0x prefix
  <output.bin>: where to write the synthetic container
  --vertdecl-dir: directory containing vs_<hash>.vertdecl.txt files
                  (default: <ucode_dir>/../shaders_vertdecl)
  --ucode-analyze: path to the built ucode_analyze binary
                    (default: alongside this script's repo, /tmp/ucode_analyze,
                    or built on demand via build_ucode_analyze.sh)
"""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

# Real XenosRecomp DeclUsage enum values (XenosRecomp/shader.h).
DECL_USAGE_POSITION = 0
DECL_USAGE_TEXCOORD = 5

REGISTER_SET_SAMPLER = 3


def run_ucode_analyze(ucode_analyze_path: Path, shader_kind: str, ucode_path: Path) -> dict:
    result = subprocess.run(
        [str(ucode_analyze_path), shader_kind, str(ucode_path)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"build_synthetic_container: ucode_analyze failed for {ucode_path}:\n{result.stderr}",
              file=sys.stderr)
        sys.exit(1)
    return json.loads(result.stdout)


def find_or_build_ucode_analyze(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    candidates = [
        Path("/tmp/ucode_analyze"),
        Path(__file__).parent / "ucode_analyze_bin",
    ]
    for c in candidates:
        if c.exists():
            return c
    # Build it on demand -- real, not a stub: invokes the same script a human
    # would run, so a fresh checkout works without a separate manual step.
    build_script = Path(__file__).parent / "build_ucode_analyze.sh"
    out_path = Path("/tmp/ucode_analyze")
    print(f"build_synthetic_container: building ucode_analyze via {build_script}...", file=sys.stderr)
    result = subprocess.run(["sh", str(build_script), str(out_path)], capture_output=True, text=True)
    if result.returncode != 0 or not out_path.exists():
        print(f"build_synthetic_container: failed to build ucode_analyze:\n{result.stdout}\n{result.stderr}",
              file=sys.stderr)
        sys.exit(1)
    return out_path


def parse_vertdecl_file(path: Path) -> list[dict]:
    """Parses a real shaders_vertdecl/vs_<hash>.vertdecl.txt file (written by
    shader_dump.cpp's dumpVertexDeclUsageForBoundShader), one real captured
    D3D9 element per line: 'offset=<n> type=<hex> usage=<n> usageIndex=<n>'.
    """
    elements = []
    for line in path.read_text().splitlines():
        parts = dict(p.split("=", 1) for p in line.split() if "=" in p)
        if "offset" not in parts:
            continue
        elements.append({
            "offset": int(parts["offset"]),
            "usage": int(parts["usage"]),
            "usage_index": int(parts["usageIndex"]),
        })
    return elements


def assign_usage_from_vertdecl(attributes: list[dict], vertdecl_elements: list[dict]) -> bool:
    """Matches real captured D3D9 elements to AnalyzeUcode's attributes by
    real byte offset (attribute's offset_words*4 == captured element's
    offset) -- both describe the same underlying vertex buffer layout, offset
    is the natural join key, no assumption that array order matches. Returns
    True if every attribute got a real match.
    """
    by_offset = {e["offset"]: e for e in vertdecl_elements}
    matched_all = True
    for attr in attributes:
        real = by_offset.get(attr["offset_words"] * 4)
        if real is None:
            matched_all = False
            continue
        attr["usage"] = real["usage"]
        attr["usage_index"] = real["usage_index"]
        attr["usage_source"] = "captured"
    return matched_all


def assign_usage_heuristic(attributes: list[dict]) -> None:
    """Fallback when no real captured D3D9 declaration is available yet: the
    same POSITION-first/TEXCOORD-follows convention verified by hand for the
    one shader pair proven earlier this session (see this script's own
    header and PLAN_native_renderer.md) -- real for that one shader, an
    inference (not a capture) for any other, and marked as such in the
    output so a caller can tell the difference.
    """
    for i, attr in enumerate(attributes):
        if i == 0 and attr["offset_words"] == 0:
            attr["usage"] = DECL_USAGE_POSITION
            attr["usage_index"] = 0
        else:
            attr["usage"] = DECL_USAGE_TEXCOORD
            attr["usage_index"] = max(0, i - 1)
        attr["usage_source"] = "heuristic"


def build_vertex_container(ucode_bytes: bytes, analysis: dict, vertdecl_elements: list[dict] | None) -> bytes:
    if not analysis["vertex_bindings"]:
        print("build_synthetic_container: no vertex_bindings found by ucode_analyze -- "
              "this shader has no real vfetch instructions, cannot build a vertex container",
              file=sys.stderr)
        sys.exit(1)
    # A shader normally has exactly one real vertex-fetch binding group in
    # this game (confirmed for the one proven shader; a shader that legitimately
    # uses multiple bindings would need every attribute across all of them,
    # which this flattens rather than dropping).
    attributes = []
    for vb in analysis["vertex_bindings"]:
        for a in vb["attributes"]:
            attributes.append({
                "instr_addr": a["instruction_address"],
                "data_format": a["data_format"],
                "offset_words": a["offset_words"],
            })
    attributes.sort(key=lambda a: a["offset_words"])

    matched_all = False
    if vertdecl_elements:
        matched_all = assign_usage_from_vertdecl(attributes, vertdecl_elements)
        if not matched_all:
            print("build_synthetic_container: WARNING vertdecl file present but did not "
                  "cover every real vfetch attribute by offset -- filling gaps with heuristic",
                  file=sys.stderr)
    if not vertdecl_elements or not matched_all:
        # Only heuristic-fill attributes still missing usage (partial real
        # coverage is trusted where it exists).
        missing = [a for a in attributes if "usage" not in a]
        if missing:
            assign_usage_heuristic(missing)
    for a in attributes:
        a.setdefault("usage_source", "captured")

    interpolator_count = bin(analysis["writes_interpolators"]).count("1")

    # VertexElement and Interpolator have DIFFERENT real on-disk bit layouts
    # -- found EMPIRICALLY (2026-08-15) using a debug build of the real
    # XenosRecomp binary (fprintf added at each read site in
    # shader_recompiler.cpp), tested against a real container with single-
    # nibble/field-set test values swept across every bit position. This
    # matters because `be<uint32_t>` (XenosRecomp/pch.h) unconditionally
    # byte-swaps on every read via its `get()`/implicit-conversion operator
    # with no "already native" guard, so naively packing bitfields in their
    # STRUCT-DECLARED order and writing them as a plain big-endian integer
    # does not universally work the same way for every struct -- verify
    # against a real debug build before changing either formula below.
    def pack_vertex_element(address, usage, usage_index):
        return (address & 0xFFF) | ((usage & 0xF) << 12) | ((usage_index & 0xF) << 16)

    def pack_interpolator(usage, usage_index, reg=0):
        return (usage_index & 0xF) | ((usage & 0xF) << 4) | ((reg & 0xF) << 8)

    vertex_elements = [
        pack_vertex_element(a["instr_addr"], a["usage"], a["usage_index"]) for a in attributes
    ]

    non_position = [a for a in attributes if a["usage"] != DECL_USAGE_POSITION]
    if len(non_position) != interpolator_count:
        print(f"build_synthetic_container: WARNING interpolator_count={interpolator_count} "
              f"doesn't match {len(non_position)} non-position attribute(s)", file=sys.stderr)
    interpolators = [
        pack_interpolator(a["usage"], a["usage_index"]) for a in non_position
    ]

    field18 = 0
    vertex_element_count = len(vertex_elements)
    field20 = 0
    interpolator_info = (interpolator_count & 0x1F) << 5

    vertex_shader_header = struct.pack(
        ">IIIIIIII",
        0, len(ucode_bytes), 0, 0, 0, interpolator_info, field18, vertex_element_count,
    ) + struct.pack(">I", field20)
    # REAL BUG FOUND AND FIXED (see git history / memory for full writeup):
    # vertexElementsAndInterpolators is be<uint32_t>[], whose get()
    # unconditionally byte-swaps -- pack as big-endian here so the runtime's
    # own swap produces the intended value.
    vertex_shader_header += struct.pack(f">{len(vertex_elements)}I", *vertex_elements)
    vertex_shader_header += struct.pack(f">{len(interpolators)}I", *interpolators)

    constant_table = struct.pack(">IIIIIII", 28, 0, 0, 0, 0, 0, 0)
    constant_table_container = struct.pack(">I", len(constant_table)) + constant_table

    header_size = 36
    constant_table_offset = header_size
    shader_offset = constant_table_offset + len(constant_table_container)
    virtual_size = shader_offset + len(vertex_shader_header)
    padding = (-virtual_size) % 4
    virtual_size += padding
    physical_size = len(ucode_bytes)

    shader_container = struct.pack(
        ">IIIIIIIII",
        0x102A1101, virtual_size, physical_size, 0, constant_table_offset, 0, shader_offset, 0, 0,
    )

    usage_sources = {a["usage_source"] for a in attributes}
    if "heuristic" in usage_sources:
        print(f"build_synthetic_container: NOTE {sum(1 for a in attributes if a['usage_source']=='heuristic')}"
              f"/{len(attributes)} vertex element(s) used the fallback usage heuristic, not a real capture "
              "-- verify shaders_vertdecl/*.vertdecl.txt coverage if this shader renders incorrectly",
              file=sys.stderr)

    return (shader_container + constant_table_container + vertex_shader_header +
            b"\x00" * padding + ucode_bytes)


def build_pixel_container(ucode_bytes: bytes, analysis: dict) -> bytes:
    interpolator_count = bin(analysis["writes_interpolators"]).count("1")
    # Fixed convention (confirmed via direct SDK source inspection, not
    # per-shader disassembly): interpolator index N is always read from ALU
    # register N by the compiled pixel shader (spirv_translator.cpp's
    # input_output_interpolators_[interpolator_index]) -- so this needs no
    # per-texture src_register lookup at all, unlike the version of this
    # script that hand-transcribed "reg" from real disassembly text earlier
    # this session.
    interpolators = [
        {"usage": DECL_USAGE_TEXCOORD, "usage_index": i, "reg": i}
        for i in range(interpolator_count)
    ]

    def pack_interpolator(usage, usage_index, reg):
        return (usage_index & 0xF) | ((usage & 0xF) << 4) | ((reg & 0xF) << 8)

    interpolator_words = [pack_interpolator(it["usage"], it["usage_index"], it["reg"]) for it in interpolators]

    field18 = 0
    interpolator_info = (len(interpolators) & 0x1F) << 5
    outputs_mask = analysis["writes_color_targets"]

    pixel_shader_header = struct.pack(
        ">IIIIIIII",
        0, len(ucode_bytes), 0, 0, 0, interpolator_info, field18, outputs_mask,
    )
    pixel_shader_header += struct.pack(f">{len(interpolator_words)}I", *interpolator_words)

    samplers = []
    for tb in analysis["texture_bindings"]:
        samplers.append({"fetch_constant": tb["fetch_constant"], "tf_slot": tb["fetch_constant"],
                          "name": f"g_Texture{len(samplers)}"})

    constant_table_struct_size = 28
    constant_info_size = 20

    string_offset = constant_table_struct_size + constant_info_size * len(samplers)
    strings_blob = b""
    constant_infos = []
    for s in samplers:
        name = s["name"].encode("ascii") + b"\x00"
        this_string_offset = string_offset + len(strings_blob)
        constant_infos.append((this_string_offset, s["tf_slot"]))
        strings_blob += name

    constant_info_bytes = b""
    for name_offset, register_index in constant_infos:
        constant_info_bytes += struct.pack(
            ">IHHHHII", name_offset, REGISTER_SET_SAMPLER, register_index, 1, 0, 0, 0,
        )

    constant_info_array_offset = constant_table_struct_size
    constant_table = struct.pack(
        ">IIIIIII", constant_table_struct_size, 0, 0, len(samplers),
        constant_info_array_offset, 0, 0,
    )
    constant_table_data = constant_table + constant_info_bytes + strings_blob
    constant_table_container = struct.pack(">I", len(constant_table_data)) + constant_table_data

    header_size = 36
    constant_table_offset = header_size
    shader_offset = constant_table_offset + len(constant_table_container)
    virtual_size = shader_offset + len(pixel_shader_header)
    padding = (-virtual_size) % 4
    virtual_size += padding
    physical_size = len(ucode_bytes)

    # REAL BUG FOUND AND FIXED: XenosRecomp determines vertex-vs-pixel purely
    # from `flags & 0x1` (bit 0 CLEAR means pixel shader). Real confirmed
    # flags value for a pixel shader: 0x102A1100.
    shader_container = struct.pack(
        ">IIIIIIIII", 0x102A1100, virtual_size, physical_size, 0,
        constant_table_offset, 0, shader_offset, 0, 0,
    )

    return (shader_container + constant_table_container + pixel_shader_header +
            b"\x00" * padding + ucode_bytes)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ucode_dir", type=Path)
    parser.add_argument("shader_kind", choices=["vs", "ps"])
    parser.add_argument("hash", help="hex, no 0x prefix")
    parser.add_argument("output", type=Path)
    parser.add_argument("--vertdecl-dir", type=Path, default=None)
    parser.add_argument("--ucode-analyze", default=None)
    args = parser.parse_args()

    target_hash = int(args.hash, 16)
    ucode_path = args.ucode_dir / f"{args.shader_kind}_{target_hash:016x}.ucode"
    if not ucode_path.exists():
        print(f"build_synthetic_container: '{ucode_path}' not found", file=sys.stderr)
        return 1

    ucode_analyze_path = find_or_build_ucode_analyze(args.ucode_analyze)
    analysis = run_ucode_analyze(ucode_analyze_path, args.shader_kind, ucode_path)

    ucode_bytes = ucode_path.read_bytes()

    if args.shader_kind == "vs":
        vertdecl_dir = args.vertdecl_dir or (args.ucode_dir.parent / "shaders_vertdecl")
        vertdecl_path = vertdecl_dir / f"vs_{target_hash:016x}.vertdecl.txt"
        vertdecl_elements = parse_vertdecl_file(vertdecl_path) if vertdecl_path.exists() else None
        if vertdecl_elements is None:
            print(f"build_synthetic_container: NOTE no real vertex-declaration capture at "
                  f"'{vertdecl_path}' -- falling back to usage heuristic for this shader", file=sys.stderr)
        container = build_vertex_container(ucode_bytes, analysis, vertdecl_elements)
    else:
        container = build_pixel_container(ucode_bytes, analysis)

    args.output.write_bytes(container)
    print(f"build_synthetic_container: wrote {len(container)} bytes to {args.output} "
          f"({len(ucode_bytes)} bytes real microcode)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
