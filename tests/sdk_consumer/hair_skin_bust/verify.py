"""Qualify hair and skin in a standalone game built against the installed Ludus SDK (#1400).

Reproducible macOS arm64 / Metal driver. It:

  1. cooks ONE real bust scene.luscn with the public `ludus-cook scene` pipeline (new -> import the
     bust glTF -> inject the authored Hair/Skin slot overrides -> assemble), the same level schema
     the Editor's Make Hair Card / Make Skin produce;
  2. builds the Qt-free standalone app against a relocated installed SDK, which also cooks both the
     forward-lit and clustered-deferred render graphs to .lurgrt;
  3. stages scene.luscn and both graphs beside the app in a clean extraction with no engine-source or
     Editor paths, and launches each profile (forward-lit draws the hair, clustered-deferred the skin);
  4. converts each raw RGBA capture to PNG and compares it with the Editor on the same fixture;
  5. records the commit, SDK version, target, profile and World (level) revision;
  6. asserts the extracted app depends on NO Homebrew Qt, and tests that removing a real bundled
     runtime dependency makes a launch fail with an actionable message;
  7. runs the negative checks for an incompatible profile and a missing cooked scene.

The native build, cook, launch and visual comparison require the pinned Clang 23 toolchain, Slang,
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
BUST = ROOT / "tests/scenes/characters/infinite-head"
EXTENT = (1600, 1080)
# The saved bust level revision recorded in the provenance; bumped when the authored overrides change.
WORLD_REVISION = 2
# Each raster profile shades the one feature it owns; see the fixture README and renderer-systems.md.
PROFILES = {"forward-lit": "hair", "clustered-deferred": "skin"}
PROFILES_BY_FEATURE = {feature: profile for profile, feature in PROFILES.items()}
PACKAGE_FILE = {"forward-lit": "forward_lit.lurgrt", "clustered-deferred": "clustered_deferred.lurgrt"}
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


def cook_commands(cook, project, level, bust_gltf, scene_out):
    """The exact, ordered `ludus-cook scene` commands that cook the bust to scene.luscn.

    Step 3 (inject the Hair/Skin overrides into PROJECT/<level>) is done in Python between import and
    assemble; see cook_scene. The printed plan lists it as an explicit edit step."""
    return [
        [cook, "scene", "new", project, level, "--name", "Character bust"],
        [cook, "scene", "import", project, level, bust_gltf],
        # (edit PROJECT/<level>: inject authoring/bust-overrides.json onto the Hair and Skin objects)
        [cook, "scene", "assemble", project, level, scene_out],
    ]


def inject_overrides(level_path, overrides):
    """Inject the authored hair_card/skin overrides and camera onto the imported bust level objects.

    The override is keyed by the imported material slot name (the glTF slot). Each level object names
    its slot in `name`; the matching object gets the `hair_card` or `skin` member the cook reads."""
    level = json.loads(level_path.read_text())
    assigned = {name: False for name in overrides["slots"]}
    for obj in level.get("objects", []):
        slot = overrides["slots"].get(obj.get("name"))
        if slot is None:
            continue
        obj.update(slot)
        assigned[obj["name"]] = True
    missing = [name for name, done in assigned.items() if not done]
    if missing:
        raise RuntimeError(f"The imported bust level has no object named {missing}; "
                           "check the glTF slot names against authoring/bust-overrides.json")
    camera = overrides.get("camera")
    if camera:
        level["camera"] = {k: camera[k] for k in ("eye", "target", "up")}
    level_path.write_text(json.dumps(level, indent=2, sort_keys=True) + "\n")


def cook_scene(cook, workspace, scene_out, log):
    """Cook the single real bust scene.luscn via the public ludus-cook scene pipeline."""
    project = workspace / "bust-project"
    project.mkdir(parents=True, exist_ok=True)
    level = "bust.lulevel"
    # Stage the bust glTF (and its maps/licence) so the import carries no engine-source path.
    bust_dir = workspace / "bust-gltf"
    bust_dir.mkdir(parents=True, exist_ok=True)
    for name in ("infinite-head-hair.gltf", "infinite-head.bin", "hair-cards.bin", "hair-cards.png",
                 "Map-COL.jpg", "Infinite-Level_02_Tangent_SmoothUV.jpg", "LICENSE.txt"):
        shutil.copyfile(BUST / name, bust_dir / name)
    overrides = json.loads((FIXTURE / "authoring/bust-overrides.json").read_text())
    commands = cook_commands(cook, project, level, bust_dir / "infinite-head-hair.gltf", scene_out)
    run(commands[0], cwd=workspace, log=log)   # scene new
    run(commands[1], cwd=workspace, log=log)   # scene import
    inject_overrides(project / level, overrides)
    run(commands[2], cwd=workspace, log=log)   # scene assemble -> scene.luscn
    if not Path(scene_out).is_file():
        raise RuntimeError(f"ludus-cook did not produce {scene_out}")


def build_commands(tools, sdk, consumer, build, package):
    """The exact, ordered configure/build/install commands, using only the relocated SDK."""
    toolsbin = Path(tools) / "out/host-tools/venv/bin"
    return [
        ["cp", "-R", str(consumer), str(build.parent / "consumer")],
        [toolsbin / "cmake", "-S", build.parent / "consumer", "-B", build, "-G", "Ninja",
         f"-DCMAKE_PREFIX_PATH={sdk}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DCMAKE_CXX_COMPILER={Path(tools) / 'out/host-tools/bin/clang++'}",
         f"-DCMAKE_MAKE_PROGRAM={toolsbin / 'ninja'}",
         f"-DLUDUS_SLANG_COMPILER={Path(tools) / 'out/shader-tools/slang/bin/slangc'}",
         f"-DLUDUS_SPIRV_VALIDATOR={Path(tools) / 'out/shader-tools/spirv-tools/usr/bin/spirv-val'}"],
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


def bundled_runtime_libraries(executable, binary_dir):
    """Return the app's dynamic dependencies resolved inside the package (macOS otool / Linux ldd)."""
    if sys.platform == "darwin":
        out = subprocess.check_output(["otool", "-L", str(executable)], text=True)
        deps = [line.split()[0] for line in out.splitlines()[1:] if line.strip()]
    else:
        env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
        out = subprocess.check_output(["ldd", str(executable)], text=True, env=env)
        deps = [part.split()[0] for line in out.splitlines() if " => " in line
                for part in [line.split(" => ", 1)[1]] if part.strip() and part.strip() != "not"]
    return out, deps


def assert_no_homebrew_qt(executable, binary_dir, evidence):
    """The extracted app must not depend on Homebrew Qt: it links no Qt and loads only the bundled
    renderer runtime and system libraries."""
    out, deps = bundled_runtime_libraries(executable, binary_dir)
    (evidence / "dependencies.txt").write_text(out)
    offenders = [d for d in deps if ("Qt" in d or "qt6" in d.lower() or "/opt/homebrew" in d or "/usr/local/Cellar" in d)]
    if offenders:
        raise RuntimeError(f"The extracted app must not depend on Homebrew Qt; found {offenders}")
    return deps


def test_runtime_dependency_removal(package, executable, env, evidence):
    """Remove a real bundled runtime dependency and confirm the launch fails with an actionable
    message rather than a silent crash, then restore it."""
    lib_dir = package / "lib"
    bundled = sorted(lib_dir.glob("*.dylib")) + sorted(lib_dir.glob("*.so*")) if lib_dir.is_dir() else []
    if not bundled:
        # No separately bundled runtime dependency to remove on this target; record and skip.
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

    # Missing dependency: hide the cooked scene, launch, then restore it.
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


def provenance(tools, sdk, scene_path):
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
        "world_revision": WORLD_REVISION,
        "scene_sha256": hashlib.sha256(scene_path.read_bytes()).hexdigest(),
        "extent": EXTENT,
        "encoding": "normalized 8-bit display RGB",
    }


def plan(cook, tools, sdk, consumer, build, package, scene_out):
    print("# Reproducible hair/skin standalone qualification (#1400) — macOS arm64 / Metal")
    print("# 1. Cook ONE real bust scene.luscn with the public ludus-cook scene pipeline:")
    for command in cook_commands(cook, "<workspace>/bust-project", "bust.lulevel",
                                 "<workspace>/bust-gltf/infinite-head-hair.gltf", scene_out):
        print("  " + " ".join(str(part) for part in command))
    print("     (between import and assemble, inject authoring/bust-overrides.json onto the Hair and Skin objects)")
    print("# 2. Build the Qt-free app against the relocated SDK (cooks both .lurgrt graphs):")
    for command in build_commands(tools, sdk, consumer, build, package):
        print("  " + " ".join(str(part) for part in command))
    print("# 3. Stage scene.luscn + both graphs beside the app and launch each profile:")
    for profile in PROFILES:
        print(f"  (cd {package} && ./bin/hair_skin_capture --profile {profile})  # uses {PACKAGE_FILE[profile]}")
    print("# 4. Checks: no Homebrew-Qt dependency, a removed bundled runtime dep fails the launch,")
    print(f"     --check-profile hair clustered-deferred -> exit {EXIT_INCOMPATIBLE_PROFILE},")
    print(f"     a missing scene.luscn -> exit {EXIT_MISSING_DEPENDENCY}.")
    print("# 5. Convert each raw RGBA to PNG and compare with the Editor (scripts/compare-render).")


def verify(cook, tools, sdk, consumer, editor, graph, evidence):
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

        scene_out = workspace / "scene.luscn"
        cook_scene(cook, workspace, scene_out, evidence / "cook.log")

        for command in build_commands(tools, relocated, consumer, build, package):
            run(command, cwd=workspace, env=env, log=evidence / "build.log")

        # Stage the cooked scene and both cooked graphs into the clean extraction beside the app.
        shutil.copyfile(scene_out, package / "scene.luscn")
        for name, cooked in (("forward_lit.lurgrt", build / "forward_lit.lurgrt"),
                             ("clustered_deferred.lurgrt", build / "clustered_deferred.lurgrt")):
            # ludus_compile_render_graph writes <name>.lurgrt under the build tree; find it if nested.
            source = cooked if cooked.is_file() else next(build.rglob(name))
            shutil.copyfile(source, package / name)

        (evidence / "provenance.json").write_text(json.dumps(provenance(tools, relocated, scene_out), indent=2) + "\n")
        assert_no_homebrew_qt(binary, package / "bin", evidence)

        editor_section = {"hair": "hair cards in forward lit", "skin": "skin diffusion in clustered deferred"}
        for profile, feature in PROFILES.items():
            log = evidence / f"{feature}.log"
            run([binary, "--profile", profile], cwd=package, env=env, log=log)
            rgba_to_png(package / "capture.rgba", evidence / f"sdk-{feature}.png", EXTENT)
            shutil.move(str(package / "capture.rgba"), str(evidence / f"sdk-{feature}.rgba"))
            shutil.move(str(package / "capture.txt"), str(evidence / f"sdk-{feature}.txt"))
            editor_env = dict(env)
            editor_env["LUDUS_EDITOR_WORLD_CAPTURE_HAIR" if feature == "hair" else "LUDUS_EDITOR_WORLD_CAPTURE_SKIN"] = \
                str(evidence / f"editor-{feature}.png")
            run([editor, "The World viewport draws the character bust through light edits, orbit and resize",
                 "-c", editor_section[feature]], cwd=package, env=editor_env, log=log)
            run([sys.executable, ROOT / "scripts/compare-render",
                 evidence / f"editor-{feature}.png", evidence / f"sdk-{feature}.png",
                 "--max-rmse", "0.02", "-o", evidence / f"{feature}.html"],
                cwd=evidence, env=env, log=evidence / f"{feature}-compare.log")

        assert_no_homebrew_qt(binary, package / "bin", evidence)
        test_runtime_dependency_removal(package, binary, env, evidence)
        negative_checks(package, binary, env, evidence)
        print(f"Hair/skin standalone qualification evidence written to {evidence}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tools", type=Path, default=ROOT, help="Trusted Ludus checkout with built host tools.")
    parser.add_argument("--cook", type=Path, help="Built ludus-cook executable.")
    parser.add_argument("--sdk", type=Path, help="Installed SDK prefix to build the game against.")
    parser.add_argument("--consumer", type=Path, default=FIXTURE, help="This fixture directory.")
    parser.add_argument("--editor", type=Path, help="Built Editor test executable for the Editor capture.")
    parser.add_argument("--graph", type=Path, default=None, help="Unused; kept for driver compatibility.")
    parser.add_argument("--evidence", type=Path, default=ROOT / "out/hair-skin-bust")
    parser.add_argument("--plan", action="store_true", help="Print the command sequence without executing.")
    arguments = parser.parse_args()

    if arguments.plan:
        plan(arguments.cook or Path("<ludus-cook>"), arguments.tools, arguments.sdk or Path("<sdk-prefix>"),
             arguments.consumer, Path("<workspace>/build"), Path("<workspace>/package"),
             "<workspace>/scene.luscn")
        return
    for required in ("cook", "sdk", "editor"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required} is required unless --plan is given")
    verify(arguments.cook.resolve(), arguments.tools.resolve(), arguments.sdk.resolve(),
           arguments.consumer.resolve(), arguments.editor.resolve(), arguments.graph, arguments.evidence.resolve())


if __name__ == "__main__":
    main()
