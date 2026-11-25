#!/usr/bin/env python3
"""Feasibility-only: emit GLSL ES 3.00 for the probe shader via pinned Slang +
pinned SPIRV-Cross, derive the std140 upload contract independently, and stage a
WebGL 2 browser harness. This is NOT the installed shader helper; it exists only
to satisfy the shader feasibility gate before any production backend code."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent.parent
TOOLS = ROOT / "out/shader-tools"
SOURCE = Path(__file__).resolve().parent
LOCK = json.loads((ROOT / "config/shader_toolchain.json").read_text())
CROSS_LOCK = json.loads((ROOT / "config/spirv_cross_toolchain.json").read_text())
OFFSETS = {"Resolution": 0, "ElapsedTime": 8, "Direction": 16, "Tint": 32}


def run(command: list[str], *, echo: bool = True) -> str:
    if echo:
        print("+ " + " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), check=True, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.stdout


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def verify_tools() -> tuple[Path, Path]:
    slang = TOOLS / "slang/bin/slangc"
    cross = TOOLS / "spirv-cross/bin/spirv-cross"
    archive = TOOLS / "spirv-cross-src/spirv-cross.tar.gz"
    require(run([slang, "-version"], echo=False).strip() == LOCK["slang"]["version"],
            "Slang version differs from pin")
    # Reproducible pin is the SPIRV-Cross source archive (tag/commit/sha256).
    # The CLI is a local Release build of exactly that verified source.
    require(archive.exists() and digest(archive) == CROSS_LOCK["spirv_cross"]["source_sha256"],
            "SPIRV-Cross source archive differs from pin")
    require(cross.exists(), "SPIRV-Cross CLI missing; build the pinned source first")
    return slang, cross


def derive_glsl_es_layout(code: str) -> dict:
    """Independently apply std140 to the emitted GLSL ES uniform block, without
    borrowing SPIR-V/WGSL offsets or trusting the program query."""
    match = re.search(r"uniform\s+(\w+)\s*\{([^}]+)\}", code)
    require(match is not None, "Missing GLSL ES uniform block")
    members = re.findall(r"(?:highp|mediump|lowp)?\s*(vec[234]|float)\s+(\w+)\s*;", match[2])
    require(len(members) == 4, "GLSL ES uniform shape changed; review verifier")
    kinds = {"float": (4, 4), "vec2": (8, 8), "vec3": (12, 16), "vec4": (16, 16)}
    offset = 0
    alignment = 1
    offsets = {}
    types = []
    for kind, name in members:
        size, align = kinds[kind]
        alignment = max(alignment, align)
        offset = (offset + align - 1) // align * align
        offsets[name] = offset
        offset += size
        types.append(kind)
    size = (offset + alignment - 1) // alignment * alignment
    require(types == ["vec2", "float", "vec3", "vec4"], "GLSL ES field types differ from upload contract")
    require(offsets == OFFSETS and size == 48 and alignment == 16, "GLSL ES std140 layout mismatch")
    require(re.search(r"layout\(std140\)\s+uniform\s+" + match[1], code) is not None, "Missing std140 block layout")
    entry = re.search(r"\bvoid\s+main\s*\(", code)
    require(entry is not None, "GLSL ES entry point is not main")
    return {"block": match[1], "offsets": offsets, "size": size, "alignment": alignment, "binding": 0}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/shader-probe-webgl")
    parser.add_argument("--luslc", type=Path, help="luslc that lowers probe.lusl (default: ./scripts/bootstrap-luslc)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    slang, cross = verify_tools()
    luslc = args.luslc
    if luslc is None:
        sys.path.insert(0, str(ROOT / "scripts/python"))
        import host_luslc
        luslc = host_luslc.ensure(ROOT)
    # The SDK helper's lowering step; it keeps both LUDUS_GLSL_ES variants in one module.
    source = output / "probe.lusl.slang"
    lowering = [sys.executable, ROOT / "cmake/shaders/lower_lusl.py", "--luslc", luslc.resolve(),
                "--root", SOURCE, "--source", "probe.lusl", "--name", "probe", "--output", source,
                "--slots", output / "probe_slots.h", "--depfile", output / "lusl.d"]
    run(lowering)
    commands = [[str(part) for part in lowering]]
    for stage, entry in (("vertex", "VertexMain"), ("fragment", "FragmentMain")):
        spirv = output / f"probe.{stage}.spv"
        command = [slang, source, "-target", "spirv", "-profile", "spirv_1_3",
                   "-entry", entry, "-stage", stage, "-o", spirv]
        commands.append([str(part) for part in command])
        run(command)
        essl = output / f"probe.{'vert' if stage == 'vertex' else 'frag'}.essl"
        # SPIRV-Cross: SPIR-V -> GLSL ES 3.00. --es forces ESSL; --version 300.
        cross_command = [cross, "--version", "300", "--es", "--output", essl, spirv]
        commands.append([str(part) for part in cross_command])
        run(cross_command)
        text = essl.read_text()
        require(text.lstrip().startswith("#version 300 es"), f"{stage} is not GLSL ES 3.00")
        require("GL_ARB_shader_draw_parameters" not in text, f"{stage} requires a desktop-only extension")
        require("gl_BaseVertex" not in text and "gl_BaseInstance" not in text,
                f"{stage} still references base vertex/instance")

    layout = derive_glsl_es_layout((output / "probe.frag.essl").read_text())
    (output / "contract.json").write_text(json.dumps({"glsl_es": layout, "offsets": OFFSETS,
        "note": "std140 derived independently from emitted GLSL ES; verified against the real linked program at runtime."}, indent=2) + "\n")
    for name in ("webgl.html", "webgl.js"):
        (output / ("index.html" if name == "webgl.html" else name)).write_text((SOURCE / name).read_text())
    (output / "build-evidence.json").write_text(json.dumps({
        "slang": LOCK["slang"], "spirv_cross": CROSS_LOCK["spirv_cross"],
        "commands": commands, "source_sha256": digest(SOURCE / "probe.lusl"),
        "artifacts": {p.name: digest(p) for p in sorted(output.iterdir())
                      if p.suffix in (".spv", ".essl")},
        "runtime_validation": "Not established by translation; run the WebGL 2 pixel probe."}, indent=2) + "\n")
    print("Staged GLSL ES feasibility harness in " + str(output))


if __name__ == "__main__":
    main()
