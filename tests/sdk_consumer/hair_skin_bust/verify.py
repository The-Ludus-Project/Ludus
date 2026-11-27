"""Qualify hair and skin in a standalone game built against the installed Ludus SDK (#1400).

Reproducible macOS arm64 / Metal driver. The Editor is the source of truth: it imports the character
bust, assigns the Hair and Skin materials (Make Hair Card / Make Skin), SAVES one World, and EXPORTS
its cooked scene.luscn plus a manifest of the World/scene/graph hashes and the saved camera. This
driver:

  1. runs the Editor export test ONCE: it saves the World, exports one scene.luscn and its manifest,
     and renders that exported composition through both packaged profiles with the Editor's offscreen
     raster capture at the saved camera and a fixed 1600x1080 reverse-Z view;
  2. builds the Qt-free standalone app against a relocated installed SDK, which cooks both the
     forward-lit and clustered-deferred render graphs to .lurgrt;
  3. stages the Editor-exported scene.luscn and both graphs beside the app in a clean extraction with
     no engine-source or Editor paths, and launches each profile (forward-lit draws the hair,
     clustered-deferred the skin), so both hosts consume the SAME cooked scene;
  4. checks the World, cooked-scene and capture-view identities, then converts each raw RGBA capture
     to PNG and compares it with the matched Editor capture;
  5. records the commit, SDK version, target, profile, and the World, cooked-scene and graph hashes;
  6. asserts the extracted app depends on NO Homebrew Qt or checkout paths, and tests that removing a
     required packaged input (a bundled runtime library, and the cooked scene) fails with a diagnostic;
  7. runs the incompatible-profile checks.

The native build, export, launch and visual comparison require the pinned Clang 23 toolchain, Slang,
a macOS host and a Metal GPU. Without them, run with --plan to print the exact ordered command
sequence so Codex can run it verbatim on the Metal lane.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
EXTENT = (1600, 1080)
# Each raster profile shades the one feature it owns; see the fixture README and renderer-systems.md.
PROFILES = {"forward-lit": "hair", "clustered-deferred": "skin"}
PROFILES_BY_FEATURE = {feature: profile for profile, feature in PROFILES.items()}
PACKAGE_FILE = {"forward-lit": "forward_lit.lurgrt", "clustered-deferred": "clustered_deferred.lurgrt"}
# The Catch2 test the Editor runs to export the cooked scene and the matched capture.
EXPORT_TEST = "The character bust World exports its cooked scene and matched captures"
# Distinct exit codes the standalone app returns; they must match capture.cpp.
EXIT_INCOMPATIBLE_PROFILE = 3
EXIT_MISSING_DEPENDENCY = 4


def run(command, cwd, env=None, log=None, check=True, timeout=1800, capture=False):
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


EXPORT_FILES = ("bust.world.json", "scene.luscn", "scene.manifest.json") + tuple(
    f"editor-{feature}.png" for feature in PROFILES.values())


def export_editor_scene(editor, directory, env, log):
    """Run the Editor export test once. It saves one bust World, exports its cooked scene.luscn and
    manifest, and renders that one exported composition through both profiles, so every capture and the
    SDK app consume the same saved revision."""
    export_env = dict(env)
    export_env.update({"LUDUS_BUST_EXPORT_DIRECTORY": str(directory), "QT_QPA_PLATFORM": "offscreen"})
    run([editor, EXPORT_TEST], cwd=directory, env=export_env, log=log)
    for name in EXPORT_FILES:
        if not (directory / name).is_file():
            raise RuntimeError(f"The Editor export did not produce {directory / name}")
    return json.loads((directory / "scene.manifest.json").read_text())


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_export_identity(directory, manifest):
    """The staged packet and the saved World must be the exact revision the manifest names."""
    if sha256(directory / "scene.luscn") != manifest.get("sceneSha256"):
        raise RuntimeError("scene.luscn does not match the export manifest's sceneSha256")
    if sha256(directory / "bust.world.json") != manifest.get("savedWorldSha256"):
        raise RuntimeError("bust.world.json does not match the export manifest's savedWorldSha256")
    captures = manifest.get("captures", {})
    for feature in PROFILES.values():
        if captures.get(feature, {}).get("image") != f"editor-{feature}.png":
            raise RuntimeError(f"The export manifest records no Editor {feature} capture")


def read_capture_text(path):
    """Parse the app's key=value capture.txt."""
    return dict(line.split("=", 1) for line in Path(path).read_text().splitlines() if "=" in line)


def check_capture_view(manifest, capture):
    """The app must have drawn at the Editor capture's extent and planes."""
    view = manifest.get("captureView", {})
    expected = {"width": view.get("width"), "height": view.get("height"),
                "near_micrometres": view.get("nearMicrometres"), "far_micrometres": view.get("farMicrometres")}
    actual = {key: int(capture.get(key, "-1")) for key in expected}
    if actual != expected:
        raise RuntimeError(f"The SDK capture view {actual} differs from the Editor capture view {expected}")


def build_commands(tools, sdk, consumer, build, package):
    """The exact, ordered configure/build/install commands, using only the relocated SDK."""
    toolsbin = Path(tools) / "out/host-tools/venv/bin"
    configure = [toolsbin / "cmake", "-S", build.parent / "consumer", "-B", build, "-G", "Ninja",
                 f"-DCMAKE_PREFIX_PATH={sdk}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                 f"-DCMAKE_CXX_COMPILER={Path(tools) / 'out/host-tools/bin/clang++'}",
                 f"-DCMAKE_MAKE_PROGRAM={toolsbin / 'ninja'}",
                 f"-DLUDUS_SLANG_COMPILER={Path(tools) / 'out/shader-tools/slang/bin/slangc'}"]
    # The pinned spirv-val is a Linux amd64 package; macOS shader tools have none, and the graph cook
    # validates SPIR-V only when one is configured (as scripts/python/runtime_profile.py selects).
    validator = Path(tools) / "out/shader-tools/spirv-tools/usr/bin/spirv-val"
    if validator.is_file():
        configure.append(f"-DLUDUS_SPIRV_VALIDATOR={validator}")
    return [
        ["cp", "-R", str(consumer), str(build.parent / "consumer")],
        configure,
        [toolsbin / "cmake", "--build", build],
        [toolsbin / "cmake", "--install", build, "--prefix", package],
    ]


def rgba_to_png(rgba_path, png_path, extent):
    """Convert the app's raw top-left RGBA8 capture to PNG with Pillow (the app links no PNG encoder)."""
    from PIL import Image
    data = rgba_path.read_bytes()
    expected = extent[0] * extent[1] * 4
    if len(data) != expected:
        raise RuntimeError(f"{rgba_path} is {len(data)} bytes, expected {expected} for {extent}")
    Image.frombytes("RGBA", extent, data).convert("RGB").save(png_path)


def bundled_runtime_libraries(executable):
    """Return the app's dynamic dependencies resolved inside the package (macOS otool / Linux ldd)."""
    if sys.platform == "darwin":
        out = subprocess.check_output(["otool", "-L", str(executable)], text=True)
        deps = [line.split()[0] for line in out.splitlines()[1:] if line.strip()]
    else:
        env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
        out = subprocess.check_output(["ldd", str(executable)], text=True, env=env)
        deps = [line.split(" => ", 1)[1].split(" (", 1)[0].strip()
                for line in out.splitlines() if " => " in line]
    return out, deps


def assert_no_homebrew_qt(executable, tools, evidence):
    """The extracted app must depend on no Qt, no Homebrew prefix and no engine-checkout path."""
    out, deps = bundled_runtime_libraries(executable)
    (evidence / "dependencies.txt").write_text(out)
    tools = str(Path(tools).resolve())
    offenders = [d for d in deps if ("Qt" in d or "qt6" in d.lower() or "/opt/homebrew" in d
                                     or "/usr/local/Cellar" in d or d.startswith(tools))]
    if offenders:
        raise RuntimeError(f"The extracted app must not depend on Qt, Homebrew or checkout paths; found {offenders}")
    return deps


def test_runtime_dependency_removal(package, executable, env, evidence):
    """Remove a required packaged input (a bundled runtime library) and confirm the launch fails with an
    actionable message rather than crashing silently, then restore it."""
    lib_dir = package / "lib"
    bundled = sorted(lib_dir.glob("*.dylib")) + sorted(lib_dir.glob("*.so*")) if lib_dir.is_dir() else []
    if not bundled:
        (evidence / "dependency-removal.json").write_text(
            json.dumps({"skipped": "no bundled runtime dependency in lib/"}, indent=2) + "\n")
        return
    victim = bundled[0]
    moved = victim.with_suffix(victim.suffix + ".removed")
    victim.rename(moved)
    try:
        process = run([executable, "--profile", "forward-lit"], cwd=package, env=env, check=False, capture=True)
    finally:
        moved.rename(victim)
    message = (process.stdout + process.stderr).strip()
    if process.returncode == 0:
        raise RuntimeError(f"Removing {victim.name} should make the launch fail, but it succeeded")
    (evidence / "dependency-removal.json").write_text(json.dumps(
        {"removed": victim.name, "exit": process.returncode, "message": message[-2000:]}, indent=2) + "\n")


def negative_checks(package, executable, env, evidence):
    """Assert the app names an incompatible profile and a missing cooked scene actionably."""
    results = []
    for feature, profile in (("hair", "clustered-deferred"), ("skin", "forward-lit")):
        process = run([executable, "--check-profile", feature, profile], cwd=package, env=env, check=False,
                      capture=True)
        message = (process.stdout + process.stderr).strip()
        if process.returncode != EXIT_INCOMPATIBLE_PROFILE:
            raise RuntimeError(f"--check-profile {feature} {profile}: expected exit {EXIT_INCOMPATIBLE_PROFILE}, "
                               f"got {process.returncode}: {message}")
        if "Incompatible renderer profile" not in message or PROFILES_BY_FEATURE[feature] not in message:
            raise RuntimeError(f"--check-profile {feature} {profile} did not name the compatible profile: {message}")
        results.append({"feature": feature, "profile": profile, "exit": process.returncode, "diagnostic": message})

    scene = package / "scene.luscn"
    hidden = scene.with_suffix(".luscn.hidden")
    scene.rename(hidden)
    try:
        process = run([executable, "--profile", "clustered-deferred"], cwd=package, env=env, check=False, capture=True)
    finally:
        hidden.rename(scene)
    message = (process.stdout + process.stderr).strip()
    if process.returncode != EXIT_MISSING_DEPENDENCY or "scene.luscn" not in message:
        raise RuntimeError(f"A missing cooked scene must exit {EXIT_MISSING_DEPENDENCY} naming the file: "
                           f"exit {process.returncode}: {message}")
    results.append({"check": "missing-dependency", "exit": process.returncode, "diagnostic": message})
    (evidence / "negative-checks.json").write_text(json.dumps(results, indent=2) + "\n")


def provenance(tools, sdk, scene_path, exported, graph_hashes):
    commit = subprocess.check_output(["git", "-C", str(tools), "rev-parse", "HEAD"], text=True).strip()
    manifest = json.loads((Path(sdk) / "share/Ludus/LudusSdkManifest.json").read_text())
    return {
        "issue": 1400,
        "commit": commit,
        "sdk_version": manifest.get("version", "unknown"),
        "sdk_source_revision": manifest.get("source_revision", "unknown"),
        "target_triple": manifest.get("target_triple", "unknown"),
        "build_flavor": manifest.get("build_flavor", "unknown"),
        "renderer_profiles": PROFILES,
        "world_sha256": exported.get("worldSha256"),
        "saved_world_sha256": exported.get("savedWorldSha256"),
        "scene_sha256": exported.get("sceneSha256"),
        "staged_scene_sha256": sha256(scene_path),
        "graph_sha256": graph_hashes,
        "editor_captures": exported.get("captures"),
        "capture_view": exported.get("captureView"),
        "camera": exported.get("camera"),
        "material_slots": exported.get("materialSlots"),
        "extent": EXTENT,
        "encoding": "normalized 8-bit display RGB",
    }


def plan(editor, tools, sdk, consumer, build, package):
    print("# Reproducible hair/skin standalone qualification (#1400) — macOS arm64 / Metal")
    print("# 1. Export ONE Editor-saved bust World once: scene.luscn, manifest and both profile captures.")
    print(f"  LUDUS_BUST_EXPORT_DIRECTORY=<ws>/export QT_QPA_PLATFORM=offscreen {editor} \"{EXPORT_TEST}\"")
    print("# 2. Build the Qt-free app against the relocated SDK (cooks both .lurgrt graphs):")
    for command in build_commands(tools, sdk, consumer, build, package):
        print("  " + " ".join(str(part) for part in command))
    print("# 3. Stage the Editor-exported scene.luscn + both graphs beside the app and launch each profile:")
    for profile in PROFILES:
        print(f"  (cd {package} && ./bin/hair_skin_capture --profile {profile})  # uses {PACKAGE_FILE[profile]}")
    print("# 4. Checks: no Homebrew-Qt/checkout dependency, a removed bundled runtime dep fails the launch,")
    print(f"     --check-profile hair clustered-deferred -> exit {EXIT_INCOMPATIBLE_PROFILE},")
    print(f"     a missing scene.luscn -> exit {EXIT_MISSING_DEPENDENCY}.")
    print("# 5. Convert each raw RGBA to PNG and compare with the matched Editor capture (scripts/compare-render).")


def verify(editor, tools, sdk, consumer, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    with tempfile.TemporaryDirectory() as workspace:
        workspace = Path(workspace)
        relocated = workspace / "relocated-sdk"
        build = workspace / "build"
        package = workspace / "package"
        binary = package / "bin/hair_skin_capture"
        shutil.copytree(sdk, relocated)
        run([Path(tools) / "out/host-tools/venv/bin/python", "scripts/python/engine.py",
             "bundle-sdk-dependencies", "macos-clang-development", "--prefix", relocated],
            cwd=tools, env=env, log=evidence / "bundle.log")

        # 1. Export the one saved World once: the cooked scene and both profiles' Editor captures.
        export = workspace / "export"
        export.mkdir()
        exported = export_editor_scene(editor, export, env, evidence / "editor-export.log")
        check_export_identity(export, exported)
        scene_out = export / "scene.luscn"
        for name in EXPORT_FILES:
            shutil.copyfile(export / name, evidence / name)

        # 2. Build the Qt-free app (cooks both graphs).
        for command in build_commands(tools, relocated, consumer, build, package):
            run(command, cwd=workspace, env=env, log=evidence / "build.log")

        # 3. Stage the Editor-exported scene and both cooked graphs into the clean extraction.
        shutil.copyfile(scene_out, package / "scene.luscn")
        graph_hashes = {}
        for profile, name in PACKAGE_FILE.items():
            source = (build / name) if (build / name).is_file() else next(build.rglob(name))
            shutil.copyfile(source, package / name)
            graph_hashes[name] = hashlib.sha256((package / name).read_bytes()).hexdigest()

        (evidence / "provenance.json").write_text(
            json.dumps(provenance(tools, relocated, package / "scene.luscn", exported, graph_hashes), indent=2) + "\n")
        assert_no_homebrew_qt(binary, tools, evidence)

        # 4+5. Launch each profile from the clean extraction and compare with the matched Editor capture.
        for profile, feature in PROFILES.items():
            run([binary, "--profile", profile], cwd=package, env=env, log=evidence / f"{feature}.log")
            rgba_to_png(package / "capture.rgba", evidence / f"sdk-{feature}.png", EXTENT)
            shutil.move(str(package / "capture.rgba"), str(evidence / f"sdk-{feature}.rgba"))
            shutil.move(str(package / "capture.txt"), str(evidence / f"sdk-{feature}.txt"))
            check_capture_view(exported, read_capture_text(evidence / f"sdk-{feature}.txt"))
            run([sys.executable, ROOT / "scripts/compare-render",
                 evidence / f"editor-{feature}.png", evidence / f"sdk-{feature}.png",
                 "--max-rmse", "0.05", "-o", evidence / f"{feature}.html"],
                cwd=evidence, env=env, log=evidence / f"{feature}-compare.log")

        test_runtime_dependency_removal(package, binary, env, evidence)
        negative_checks(package, binary, env, evidence)
        print(f"Hair/skin standalone qualification evidence written to {evidence}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tools", type=Path, default=ROOT, help="Trusted Ludus checkout with built host tools.")
    parser.add_argument("--editor", type=Path, help="Built Editor test executable that exports the saved bust World.")
    parser.add_argument("--sdk", type=Path, help="Installed SDK prefix to build the game against.")
    parser.add_argument("--consumer", type=Path, default=FIXTURE, help="This fixture directory.")
    parser.add_argument("--evidence", type=Path, default=ROOT / "out/hair-skin-bust")
    parser.add_argument("--plan", action="store_true", help="Print the command sequence without executing.")
    arguments = parser.parse_args()

    if arguments.plan:
        plan(arguments.editor or Path("<ludus_editor_tests>"), arguments.tools,
             arguments.sdk or Path("<sdk-prefix>"), arguments.consumer,
             Path("<workspace>/build"), Path("<workspace>/package"))
        return
    for required in ("editor", "sdk"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required} is required unless --plan is given")
    verify(arguments.editor.resolve(), arguments.tools.resolve(), arguments.sdk.resolve(),
           arguments.consumer.resolve(), arguments.evidence.resolve())


if __name__ == "__main__":
    main()
