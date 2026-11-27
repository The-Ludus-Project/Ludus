"""Qualify a project-owned BRDF provider in a standalone app built against the installed Ludus SDK (#1310).

Reproducible macOS arm64 / Metal driver. The Editor is the source of truth: its hidden export test imports the
two-board scene and the project's Oren-Nayar provider, binds the Project board to `project/oren-nayar` with Assign
Project BRDF, adds two World directional lights, SAVES the World and EXPORTS its cooked scene.luscn, manifest and
capture view. From the reopened saved World it captures, under each light set, the paired raw CPU-oracle and
GPU-reference images and the compute ray-tracing graph image, and records their region means and the analytic
direct response. This driver:

  1. runs that Editor export;
  2. builds the Qt-free standalone app against a relocated installed SDK, which cooks the installed compute
     ray-tracing template unchanged;
  3. stages the Editor-exported scene.luscn and view.bin beside the app in a clean package, checks that the app
     depends on no Qt, Homebrew or checkout path, and that it finds the project material;
  4. launches the app from the package once per light set and compares each board's mean with the analytic
     response and with the Editor's CPU, GPU and graph captures;
  5. records the commit, SDK identity, World, scene and graph hashes, and checks that a missing staged input
     fails with a diagnostic that names it.

See docs/development/project-sdk-workflow.md#qualify-a-project-brdf-provider.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
EXPORT_TEST = "The project Oren-Nayar provider World exports its cooked scene and matched captures"
APP = "bin/project_brdf_capture"
PACKAGE_GRAPH = "compute_ray_tracing.lurgrt"
# Distinct exit codes the standalone app returns; they must match capture.cpp.
EXIT_MISSING_DEPENDENCY = 4
# LURGCAP layout (modules/content/render_graph_format/include/ludus/content/render_graph_format/capture.h).
CAPTURE_MAGIC = b"LURGCAP\0"
CAPTURE_HEADER_SIZE_OFFSET = 12
CAPTURE_PIXEL_SIZE_OFFSET = 16
CAPTURE_WIDTH_OFFSET = 24
CAPTURE_HEIGHT_OFFSET = 28
CAPTURE_FAILED_PIXELS_OFFSET = 40
LABELS = {"control": 1, "project": 2}
CAPTURES = ("analytic", "editorCpu", "editorGpu", "editorGraph")
# The consumer's build type follows the SDK preset, so the installed configuration and policy match.
BUILD_TYPES = {"debug": "Debug", "development": "RelWithDebInfo", "profile": "RelWithDebInfo", "release": "Release"}


def run(command, cwd, env=None, log=None, check=True, timeout=3600, capture=False):
    """Run a command, streaming to a log file when given. Return the completed process."""
    if capture:
        return subprocess.run([str(part) for part in command], cwd=str(cwd), env=env, text=True,
                              capture_output=True, timeout=timeout)
    stdout = log.open("a") if isinstance(log, Path) else None
    try:
        process = subprocess.run([str(part) for part in command], cwd=str(cwd), env=env,
                                 stdout=stdout or subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=timeout)
    finally:
        if stdout is not None:
            stdout.close()
    if check and process.returncode:
        detail = log if log else (process.stdout or "")
        raise RuntimeError(f"Command failed ({process.returncode}): {' '.join(str(p) for p in command)}\n{detail}")
    return process


def build_type(preset):
    return BUILD_TYPES[preset.rsplit("-", 1)[-1]]


def native_flags(tools, sdk, preset):
    """The macOS sysroot and C++ runtime flags install-sdk gives an SDK consumer, from the checkout's project setup."""
    if not preset.startswith("macos-") or not Path(sdk).is_dir():
        return []
    sys.path.insert(0, str(Path(tools) / "scripts/python"))
    from ludus_tools.project_setup import _inputs
    _, records, _ = _inputs(Path(tools), Path(tools) / "tests/sdk_consumer", Path(sdk), preset, None)
    return [f"-D{key}={value}" for key, value in records[preset]["cacheVariables"].items()
            if key.startswith("CMAKE_OSX_") or key == "CMAKE_CXX_FLAGS"]


def shader_tool_flags(tools):
    """The checkout's pinned shader tools that exist: Slang, and SPIRV-Cross for Metal or spirv-val for SPIR-V."""
    candidates = {
        "LUDUS_SLANG_COMPILER": "out/shader-tools/slang/bin/slangc",
        "LUDUS_SPIRV_CROSS": "out/shader-tools/spirv-cross/bin/spirv-cross",
        "LUDUS_SPIRV_VALIDATOR": "out/shader-tools/spirv-tools/usr/bin/spirv-val",
    }
    return [f"-D{name}={Path(tools) / path}" for name, path in candidates.items() if (Path(tools) / path).exists()]


def build_commands(tools, sdk, consumer, build, package, preset):
    """The exact, ordered configure/build/install commands, using only the relocated SDK."""
    toolsbin = Path(tools) / "out/host-tools/venv/bin"
    return [
        ["cp", "-R", str(consumer), str(build.parent / "consumer")],
        [toolsbin / "cmake", "-S", build.parent / "consumer", "-B", build, "-G", "Ninja",
         f"-DCMAKE_PREFIX_PATH={sdk}", f"-DCMAKE_BUILD_TYPE={build_type(preset)}",
         f"-DCMAKE_CXX_COMPILER={Path(tools) / 'out/host-tools/bin/clang++'}",
         f"-DCMAKE_MAKE_PROGRAM={toolsbin / 'ninja'}",
         *shader_tool_flags(tools),
         *native_flags(tools, sdk, preset)],
        [toolsbin / "cmake", "--build", build],
        [toolsbin / "cmake", "--install", build, "--prefix", package],
    ]


def read_capture(path):
    """Return (width, height, failed pixels, [(r, g, b)]) of a LURGCAP file."""
    data = Path(path).read_bytes()
    if data[:8] != CAPTURE_MAGIC:
        raise RuntimeError(f"{path} is not a LURGCAP capture")
    header, = struct.unpack_from("<I", data, CAPTURE_HEADER_SIZE_OFFSET)
    pixel_bytes, = struct.unpack_from("<I", data, CAPTURE_PIXEL_SIZE_OFFSET)
    width, = struct.unpack_from("<I", data, CAPTURE_WIDTH_OFFSET)
    height, = struct.unpack_from("<I", data, CAPTURE_HEIGHT_OFFSET)
    failed, = struct.unpack_from("<I", data, CAPTURE_FAILED_PIXELS_OFFSET)
    pixels = [struct.unpack_from("<3f", data, header + index * pixel_bytes) for index in range(width * height)]
    return width, height, failed, pixels


def region_means(pixels, labels):
    """Mean RGB of each labelled board region."""
    means = {}
    for name, label in LABELS.items():
        selected = [pixel for pixel, value in zip(pixels, labels) if value == label]
        if not selected:
            raise RuntimeError(f"The {name} board covers no labelled pixel")
        means[name] = [sum(channel) / len(selected) for channel in zip(*selected)]
    return means


def relative_distance(actual, expected):
    norm = sum(value * value for value in expected) ** 0.5
    return sum((a - e) ** 2 for a, e in zip(actual, expected)) ** 0.5 / norm


def compare(sdk, expectations, set_name):
    """Compare the standalone app's board means with the analytic and Editor captures of one light set."""
    tolerance = expectations["meanTolerance"]
    recorded = expectations["sets"][set_name]
    results = {"sdk": sdk, "comparisons": {}}
    failures = []
    for capture in CAPTURES:
        for board in LABELS:
            distance = relative_distance(sdk[board], recorded[capture][board])
            results["comparisons"][f"{capture}/{board}"] = distance
            if distance > tolerance:
                failures.append(f"{set_name} {board}: SDK {sdk[board]} vs {capture} {recorded[capture][board]} "
                                f"differs by {distance:.4f} (tolerance {tolerance})")
    return results, failures


def bundled_runtime_libraries(executable):
    if sys.platform == "darwin":
        out = subprocess.check_output(["otool", "-L", str(executable)], text=True)
        deps = [line.split()[0] for line in out.splitlines()[1:] if line.strip()]
    else:
        out = subprocess.check_output(["ldd", str(executable)], text=True, env={"PATH": "/usr/bin:/bin"})
        deps = [line.split(" => ", 1)[1].split(" (", 1)[0].strip() for line in out.splitlines() if " => " in line]
    return out, deps


def assert_clean_dependencies(executable, tools, evidence):
    out, deps = bundled_runtime_libraries(executable)
    (evidence / "dependencies.txt").write_text(out)
    tools = str(Path(tools).resolve())
    offenders = [d for d in deps if ("Qt" in d or "qt6" in d.lower() or "/opt/homebrew" in d
                                     or "/usr/local/Cellar" in d or d.startswith(tools))]
    if offenders:
        raise RuntimeError(f"The packaged app must not depend on Qt, Homebrew or checkout paths; found {offenders}")


def missing_input_check(package, executable, env, evidence):
    """A missing staged capture view fails with its own exit code and names the file."""
    view = package / "view.bin"
    hidden = package / "view.bin.hidden"
    view.rename(hidden)
    try:
        process = run([executable, "--lights", "3", "missing.lurgcap"], cwd=package, env=env, check=False,
                      capture=True)
    finally:
        hidden.rename(view)
    message = (process.stdout + process.stderr).strip()
    if process.returncode != EXIT_MISSING_DEPENDENCY or "view.bin" not in message:
        raise RuntimeError(f"A missing view.bin must exit {EXIT_MISSING_DEPENDENCY} naming the file: "
                           f"exit {process.returncode}: {message}")
    (evidence / "negative-checks.json").write_text(json.dumps(
        [{"check": "missing-view", "exit": process.returncode, "diagnostic": message[-2000:]}], indent=2) + "\n")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def provenance(tools, sdk, export, package):
    commit = subprocess.check_output(["git", "-C", str(tools), "rev-parse", "HEAD"], text=True).strip()
    manifest = json.loads((Path(sdk) / "share/Ludus/LudusSdkManifest.json").read_text())
    exported = json.loads((export / "scene.manifest.json").read_text())
    if exported.get("sceneSha256") != sha256(package / "scene.luscn"):
        raise RuntimeError("The staged scene.luscn differs from the Editor export")
    return {
        "issue": 1310,
        "commit": commit,
        "sdk_version": manifest.get("version", "unknown"),
        "sdk_source_revision": manifest.get("source_revision", "unknown"),
        "target_triple": manifest.get("target_triple", "unknown"),
        "build_flavor": manifest.get("build_flavor", "unknown"),
        "world_sha256": exported.get("worldSha256"),
        "scene_sha256": exported.get("sceneSha256"),
        "graph_sha256": sha256(package / PACKAGE_GRAPH),
        "camera": exported.get("camera"),
        "material_slots": exported.get("materialSlots"),
        "lights": exported.get("lights"),
    }


def plan(editor, tools, sdk, consumer, build, package, preset):
    print("# Project BRDF provider qualification (#1310) — macOS arm64 / Metal")
    print("# 1. Export the saved provider World and the Editor's matched captures:")
    print(f"  LUDUS_PROJECT_BRDF_EXPORT=<ws>/export QT_QPA_PLATFORM=offscreen {editor} \"{EXPORT_TEST}\"")
    print("# 2. Build the Qt-free app against the relocated SDK (cooks compute_ray_tracing.lurgrt):")
    for command in build_commands(tools, sdk, consumer, build, package, preset):
        print("  " + " ".join(str(part) for part in command))
    print("# 3. Stage <ws>/export/scene.luscn and view.bin beside the app, then:")
    print(f"  (cd {package} && ./{APP} --check-materials)")
    print(f"  (cd {package} && ./{APP} --lights <mask> sdk-<set>.lurgcap)  # first, second, both")
    print("# 4. Compare each board's mean with expectations.json: analytic, editorCpu, editorGpu, editorGraph.")


def verify(editor, tools, sdk, consumer, evidence, preset):
    evidence.mkdir(parents=True, exist_ok=True)
    for log in evidence.glob("*.log"):
        log.unlink()
    env = dict(os.environ)
    with tempfile.TemporaryDirectory() as workspace:
        workspace = Path(workspace)
        export = evidence / "export"
        if export.exists():
            shutil.rmtree(export)
        export_env = dict(env, LUDUS_PROJECT_BRDF_EXPORT=str(export), QT_QPA_PLATFORM="offscreen")
        run([editor, EXPORT_TEST], cwd=workspace, env=export_env, log=evidence / "editor-export.log")
        for name in ("scene.luscn", "scene.manifest.json", "view.bin", "labels.bin", "expectations.json"):
            if not (export / name).is_file():
                raise RuntimeError(f"The Editor export did not produce {name}")
        expectations = json.loads((export / "expectations.json").read_text())
        labels = list((export / "labels.bin").read_bytes())

        relocated = workspace / "relocated-sdk"
        build = workspace / "build"
        package = workspace / "package"
        shutil.copytree(sdk, relocated)
        run([Path(tools) / "out/host-tools/venv/bin/python", "scripts/python/engine.py",
             "bundle-sdk-dependencies", preset, "--prefix", relocated],
            cwd=tools, env=env, log=evidence / "bundle.log")
        for command in build_commands(tools, relocated, consumer, build, package, preset):
            run(command, cwd=workspace, env=env, log=evidence / "build.log")
        for name in ("scene.luscn", "view.bin"):
            shutil.copyfile(export / name, package / name)
        executable = package / APP
        assert_clean_dependencies(executable, tools, evidence)
        (evidence / "provenance.json").write_text(
            json.dumps(provenance(tools, relocated, export, package), indent=2) + "\n")
        check = run([executable, "--check-materials"], cwd=package, env=env, capture=True)
        if check.returncode != 0:
            raise RuntimeError(f"The packaged app did not find the project material: {check.stdout}{check.stderr}")

        indices = expectations["lightIndices"]
        masks = {"first": 1 << indices["first"], "second": 1 << indices["second"],
                 "both": (1 << indices["first"]) | (1 << indices["second"])}
        report = {"tolerance": expectations["meanTolerance"], "sets": {}}
        failures = []
        for set_name, mask in masks.items():
            output = f"sdk-{set_name}.lurgcap"
            run([executable, "--lights", str(mask), output], cwd=package, env=env, log=evidence / "sdk.log")
            width, height, failed, pixels = read_capture(package / output)
            if (width, height) != (expectations["width"], expectations["height"]) or failed != 0:
                raise RuntimeError(f"{output}: {width}x{height} with {failed} failed pixels")
            shutil.copyfile(package / output, evidence / output)
            result, set_failures = compare(region_means(pixels, labels), expectations, set_name)
            report["sets"][set_name] = result
            failures.extend(set_failures)
        # Both lights together are the linear sum of each alone in the standalone app too.
        for board in LABELS:
            summed = [a + b for a, b in zip(report["sets"]["first"]["sdk"][board],
                                            report["sets"]["second"]["sdk"][board])]
            distance = relative_distance(report["sets"]["both"]["sdk"][board], summed)
            report["sets"]["both"]["comparisons"][f"linearSum/{board}"] = distance
            if distance > expectations["meanTolerance"]:
                failures.append(f"both {board}: {report['sets']['both']['sdk'][board]} is not the sum {summed}")
        (evidence / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
        missing_input_check(package, executable, env, evidence)
        if failures:
            raise RuntimeError("Standalone captures differ:\n" + "\n".join(failures))
        print(f"Project BRDF provider qualification evidence written to {evidence}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tools", type=Path, default=ROOT, help="Trusted Ludus checkout with built host tools.")
    parser.add_argument("--editor", type=Path, help="Built ludus_editor_tests executable.")
    parser.add_argument("--sdk", type=Path, help="Installed SDK prefix to build the app against.")
    parser.add_argument("--preset", default="macos-clang-development",
                        help="Native preset that installed the SDK; selects its dependency bundle and build type.")
    parser.add_argument("--consumer", type=Path, default=FIXTURE, help="This fixture directory.")
    parser.add_argument("--evidence", type=Path, default=ROOT / "out/project-brdf-world")
    parser.add_argument("--plan", action="store_true", help="Print the command sequence without executing.")
    arguments = parser.parse_args()
    if arguments.plan:
        plan(arguments.editor or Path("<ludus_editor_tests>"), arguments.tools,
             arguments.sdk or Path("<sdk-prefix>"), arguments.consumer,
             Path("<workspace>/build"), Path("<workspace>/package"), arguments.preset)
        return
    for required in ("editor", "sdk"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required} is required unless --plan is given")
    verify(arguments.editor.resolve(), arguments.tools.resolve(), arguments.sdk.resolve(),
           arguments.consumer.resolve(), arguments.evidence.resolve(), arguments.preset)


if __name__ == "__main__":
    main()
