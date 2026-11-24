"""Lower a LUSL compute shader and compile one entry point to WGSL for a research harness.

The benchmark drivers in tools/luminaire-benchmark and tools/sms-benchmark run their kernels
through wgpu. They lower a LUSL source with ``cmake/shaders/lower_lusl.py``, the step that
``ludus_compile_shader`` runs, then compile the generated Slang module with
``cmake/shaders/compile_shader.py``. LUSL assigns binding slots in declaration order
(docs/modules/tooling/shader-language.md#resources-and-bindings), so a driver binds each buffer
by the slot constant the lowering writes, never by a literal.
"""
from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SLOT_CONSTANT = re.compile(r"inline constexpr foundation::uint32 ([A-Z0-9_]+) = (\d+);")


def resolve_luslc(luslc: Path | None) -> Path:
    """Return ``luslc``, or build the host compiler with ./scripts/bootstrap-luslc when it is None."""
    if luslc is not None:
        return luslc.resolve()
    sys.path.insert(0, str(ROOT / "scripts/python"))
    import host_luslc

    return host_luslc.ensure(ROOT)


def read_slots(header: Path) -> dict[str, int]:
    """Return the binding slots of a generated ``<name>_slots.h``, keyed by constant name."""
    text = header.read_text(encoding="utf-8")
    slots_begin = text.index("::slots")
    locations_begin = text.index("::locations", slots_begin)
    return {name: int(slot) for name, slot in SLOT_CONSTANT.findall(text[slots_begin:locations_begin])}


def compile_compute(luslc: Path, compiler: Path, source_directory: Path, source_name: str, entry: str,
                    output: Path) -> tuple[str, dict[str, int]]:
    """Lower ``source_directory/source_name`` and compile ``entry`` to WGSL in ``output``.

    ``source_directory`` holds the LUSL source, its headers and every Slang file they name with
    ``extern slang``. The generated module is written beside them. Returns the WGSL text and the
    binding slots keyed by constant name (``sceneData`` is ``SCENE_DATA``). Raises
    ``subprocess.CalledProcessError`` when lowering or compilation fails; their diagnostics are
    already on standard error.
    """
    stem = Path(source_name).stem
    generated = source_directory / (source_name + ".slang")
    slots = output / f"{stem}_slots.h"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "cmake/shaders/lower_lusl.py"), "--luslc", str(luslc),
                    "--root", str(source_directory), "--source", source_name, "--name", stem,
                    "--output", str(generated), "--slots", str(slots), "--depfile", str(output / "lusl.d")],
                   check=True)
    subprocess.run([sys.executable, str(ROOT / "cmake/shaders/compile_shader.py"), "--source", str(generated),
                    "--output", str(output), "--name", entry, "--compiler", str(compiler),
                    "--lock", str(ROOT / "config/shader_toolchain.json"), "--graph-stage", "compute",
                    "--graph-target", "wgsl"], check=True)
    return (output / "artifact.wgsl").read_text(encoding="utf-8"), read_slots(slots)
