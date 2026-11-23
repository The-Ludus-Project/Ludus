"""Qualify hair and skin in a standalone game built against the installed Ludus SDK (#1400).

Reproducible macOS arm64 / Metal driver. It builds the standalone app against a relocated SDK,
packages and extracts it to a clean directory with no engine-source or Editor paths, launches it for
each raster profile, records the commit/SDK version/target/profile/World revision provenance, compares
the clean package's capture with the Editor on the same fixture, and runs the negative checks for a
missing dependency and an incompatible renderer profile.

The native build, package, launch and visual comparison require the pinned Clang 23 toolchain, Slang,
a macOS host and a Metal GPU. In an environment without them, run with --plan to print the exact,
ordered command sequence (and the staged fixture) without executing, so the steps are reviewable and
Codex can run them verbatim on the Metal lane.

Example (Metal lane):

    python tests/sdk_consumer/hair_skin_bust/verify.py \
        --sdk  out/install/macos-clang-development \
        --tools . \
        --editor out/build/macos-clang-development/apps/editor/ludus_editor_tests \
        --graph  out/build/macos-clang-development/apps/editor/ludus-graphs/.../editor_renderer_static.lurgrt \
        --evidence out/hair-skin-bust
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
# Each raster profile shades the one feature it owns; see the fixture README and renderer-systems.md.
PROFILES = {"forward-lit": "hair", "clustered-deferred": "skin"}
# Distinct exit codes the standalone app returns; they must match capture.cpp.
EXIT_INCOMPATIBLE_PROFILE = 3
EXIT_MISSING_DEPENDENCY = 4


def run(command, cwd, env=None, log=None, check=True, timeout=1800):
    """Run a command, streaming to a log file when given. Return the completed process."""
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


def stage_authoring(destination):
    """Copy the saved World, its schema, the scene source and the bust fixtures beside the app, so the
    clean extraction carries no engine-source or Editor paths."""
    authoring = destination / "authoring"
    (authoring / "infinite-head").mkdir(parents=True, exist_ok=True)
    for name in ("bust.world.json", "bust.scene.json", "world.schema.json"):
        shutil.copyfile(FIXTURE / "authoring" / name, authoring / name)
    for name in ("infinite-head-hair.gltf", "infinite-head.bin", "hair-cards.bin", "hair-cards.png",
                 "Map-COL.jpg", "Infinite-Level_02_Tangent_SmoothUV.jpg", "LICENSE.txt"):
        shutil.copyfile(BUST / name, authoring / "infinite-head" / name)


def provenance(tools, sdk, world_path):
    """Collect the commit, SDK version, target and saved World revision for the evidence manifest."""
    commit = subprocess.check_output(["git", "-C", str(tools), "rev-parse", "HEAD"], text=True).strip()
    manifest = json.loads((Path(sdk) / "share/Ludus/LudusSdkManifest.json").read_text())
    world = json.loads(world_path.read_text())
    return {
        "issue": 1400,
        "commit": commit,
        "sdk_version": manifest.get("version", "unknown"),
        "sdk_source_revision": manifest.get("source_revision", "unknown"),
        "target_triple": manifest.get("target_triple", "unknown"),
        "build_flavor": manifest.get("build_flavor", "unknown"),
        "renderer_profiles": PROFILES,
        "world_revision": world.get("revision"),
        "world_sha256": hashlib.sha256(world_path.read_bytes()).hexdigest(),
        "extent": EXTENT,
        "encoding": "normalized 8-bit display RGB",
    }


def build_commands(tools, sdk, consumer, build, package):
    """The exact, ordered configure/build/package commands, using only the relocated SDK."""
    toolsbin = Path(tools) / "out/host-tools/venv/bin"
    return [
        # Relocate the installed SDK and bundle its dependency closure (fresh prefix, no checkout path).
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


def plan(tools, sdk, consumer, build, package, evidence):
    """Print the reproducible command sequence and the staged fixture without executing anything."""
    print("# Reproducible hair/skin standalone qualification (#1400) — macOS arm64 / Metal")
    print("# 1. Build the standalone app against the relocated installed SDK:")
    for command in build_commands(tools, sdk, consumer, build, package):
        print("  " + " ".join(str(part) for part in command))
    print("# 2. Stage the saved World, scene source and bust fixtures beside the app (verify.py does this).")
    print("# 3. Launch each profile from the clean extraction and record provenance + capture:")
    for profile in PROFILES:
        print(f"  (cd {package} && ./hair_skin_capture --profile {profile})")
    print("# 4. Negative checks — an actionable diagnostic before packaging:")
    print(f"  (cd {package} && ./hair_skin_capture --check-profile hair clustered-deferred)  # expect exit {EXIT_INCOMPATIBLE_PROFILE}")
    print(f"  (cd {package} && ./hair_skin_capture --check-profile skin forward-lit)          # expect exit {EXIT_INCOMPATIBLE_PROFILE}")
    print("  # missing dependency: remove authoring/bust.scene.json, then a launch exits "
          f"{EXIT_MISSING_DEPENDENCY} naming the file.")
    print("# 5. Compare each clean-package capture with the Editor on the same fixture (scripts/compare-render).")
    print(f"# Evidence is written under {evidence}: capture.png/json per profile, provenance.json, "
          "comparison HTML and the deliberate-failure control.")


def negative_checks(package, env, evidence):
    """Assert the standalone app names an incompatible profile and a missing dependency actionably."""
    results = []
    for feature, profile in (("hair", "clustered-deferred"), ("skin", "forward-lit")):
        process = subprocess.run([str(package / "hair_skin_capture"), "--check-profile", feature, profile],
                                 cwd=str(package), env=env, text=True, capture_output=True)
        message = (process.stdout + process.stderr).strip()
        if process.returncode != EXIT_INCOMPATIBLE_PROFILE:
            raise RuntimeError(f"--check-profile {feature} {profile}: expected exit {EXIT_INCOMPATIBLE_PROFILE}, "
                               f"got {process.returncode}: {message}")
        compatible = PROFILES_BY_FEATURE[feature]
        if "Incompatible renderer profile" not in message or compatible not in message:
            raise RuntimeError(f"--check-profile {feature} {profile} did not name the compatible profile: {message}")
        results.append({"feature": feature, "profile": profile, "exit": process.returncode, "diagnostic": message})

    # Missing dependency: hide the authored scene source, launch, then restore it.
    scene = package / "authoring/bust.scene.json"
    hidden = scene.with_suffix(".json.hidden")
    scene.rename(hidden)
    try:
        process = subprocess.run([str(package / "hair_skin_capture"), "--profile", "clustered-deferred"],
                                 cwd=str(package), env=env, text=True, capture_output=True)
    finally:
        hidden.rename(scene)
    message = (process.stdout + process.stderr).strip()
    if process.returncode != EXIT_MISSING_DEPENDENCY or "bust.scene.json" not in message:
        raise RuntimeError(f"A missing dependency must exit {EXIT_MISSING_DEPENDENCY} naming the file: "
                           f"exit {process.returncode}: {message}")
    results.append({"check": "missing-dependency", "exit": process.returncode, "diagnostic": message})
    (evidence / "negative-checks.json").write_text(json.dumps(results, indent=2) + "\n")


PROFILES_BY_FEATURE = {feature: profile for profile, feature in PROFILES.items()}


def verify(tools, sdk, consumer, editor, graph, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    with tempfile.TemporaryDirectory() as workspace:
        workspace = Path(workspace)
        relocated = workspace / "relocated-sdk"
        build = workspace / "build"
        package = workspace / "package"
        shutil.copytree(sdk, relocated)
        run([Path(tools) / "out/host-tools/venv/bin/python", "scripts/python/engine.py",
             "bundle-sdk-dependencies", "macos-clang-development", "--prefix", relocated],
            cwd=tools, env=env, log=evidence / "bundle.log")
        for command in build_commands(tools, relocated, consumer, build, package):
            run(command, cwd=workspace, env=env, log=evidence / "build.log")
        stage_authoring(package)
        shutil.copyfile(graph, package / "graph.lurgrt")
        names = Path(str(graph) + ".resources.json")
        if names.is_file():
            shutil.copyfile(names, package / "graph.lurgrt.resources.json")

        world_path = package / "authoring/bust.world.json"
        evidence_manifest = provenance(tools, relocated, world_path)
        (evidence / "provenance.json").write_text(json.dumps(evidence_manifest, indent=2) + "\n")

        # The Editor renders the same saved bust World through the same two profiles in its
        # [skin][hair-card] acceptance test; its LUDUS_EDITOR_WORLD_CAPTURE_<FEATURE> hook saves the
        # matched viewport. The Catch2 section selects the feature.
        editor_section = {"hair": "hair cards in forward lit", "skin": "skin diffusion in clustered deferred"}
        for profile, feature in PROFILES.items():
            log = evidence / f"{feature}.log"
            run([package / "hair_skin_capture", "--profile", profile], cwd=package, env=env, log=log)
            shutil.move(str(package / "capture.png"), str(evidence / f"sdk-{feature}.png"))
            shutil.move(str(package / "capture.json"), str(evidence / f"sdk-{feature}.json"))
            editor_env = dict(env)
            editor_env["LUDUS_EDITOR_WORLD_CAPTURE_HAIR" if feature == "hair" else "LUDUS_EDITOR_WORLD_CAPTURE_SKIN"] = \
                str(evidence / f"editor-{feature}.png")
            run([editor, "The World viewport draws the character bust through light edits, orbit and resize",
                 "-c", editor_section[feature]], cwd=package, env=editor_env, log=log)
            run([sys.executable, ROOT / "scripts/compare-render",
                 evidence / f"editor-{feature}.png", evidence / f"sdk-{feature}.png",
                 "--max-rmse", "0.02", "-o", evidence / f"{feature}.html"],
                cwd=evidence, env=env, log=evidence / f"{feature}-compare.log")

        negative_checks(package, env, evidence)
        print(f"Hair/skin standalone qualification evidence written to {evidence}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tools", type=Path, default=ROOT, help="Trusted Ludus checkout with built host tools.")
    parser.add_argument("--sdk", type=Path, help="Installed SDK prefix to build the game against.")
    parser.add_argument("--consumer", type=Path, default=FIXTURE, help="This fixture directory.")
    parser.add_argument("--editor", type=Path, help="Built Editor test executable for the Editor capture.")
    parser.add_argument("--graph", type=Path, help="Editor's cooked render-graph runtime package.")
    parser.add_argument("--evidence", type=Path, default=ROOT / "out/hair-skin-bust")
    parser.add_argument("--plan", action="store_true", help="Print the command sequence without executing.")
    arguments = parser.parse_args()

    if arguments.plan:
        plan(arguments.tools, arguments.sdk or Path("<sdk-prefix>"), arguments.consumer,
             Path("<workspace>/build"), Path("<workspace>/package"), arguments.evidence)
        return
    for required in ("sdk", "editor", "graph"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required} is required unless --plan is given")
    verify(arguments.tools.resolve(), arguments.sdk.resolve(), arguments.consumer.resolve(),
           arguments.editor.resolve(), arguments.graph.resolve(), arguments.evidence.resolve())


if __name__ == "__main__":
    main()
