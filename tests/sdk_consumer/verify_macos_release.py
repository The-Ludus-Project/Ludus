#!/usr/bin/env python3
"""Installed CLI and full Release SDK: signed macOS package relocation/failures."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def run(argv, *, cwd, env, success=True):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, text=True,
                            capture_output=True, timeout=600)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Command status {result.returncode}: {argv}\n{result.stdout}\n{result.stderr}")
    print(result.stdout, end="")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "tools", "cli"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    sdk, tools, cli = args.sdk.resolve(), args.tools.resolve(), args.cli.absolute()
    env = {key: value for key, value in os.environ.items()
           if key not in ("PYTHONPATH", "LUDUS_SDK_PREFIX", "LUDUS_SDK_STORE", "BUTLER_API_KEY", "CXX") and not key.startswith("DYLD_")}
    env["PATH"] = str(tools / "out/host-tools/venv/bin") + os.pathsep + str(tools / "out/host-tools/bin") + os.pathsep + os.defpath
    with tempfile.TemporaryDirectory(prefix="ludus mac release ") as temporary:
        root = Path(temporary).resolve()
        env["LUDUS_SDK_STORE"] = str(root / "store")
        relocated = root / "relocated Release SDK"
        shutil.copytree(sdk, relocated, symlinks=True)
        game = root / "Game"
        run([cli, "project", "create", game, "--name", "Game", "--sdk", relocated, "--tools", tools, "--profile", "macos-clang-release", "--release", "--itch-target", "tester/game",
             "--components", "FoundationBase,FoundationFilesystem,Platform,GraphicsRhi,GraphicsRenderer,Content,Audio,AudioContent,Text"], cwd=root, env=env)
        config = json.loads((game / "ludus.release.json").read_text())
        assert config["profiles"]["macos-release"]["entryPoint"] == "Game.app/Contents/MacOS/Game"
        def package():
            result = run([cli, "--json", "project", "package", game, "--profile", "macos-release", "--version", "0.1.0"], cwd=root, env=env)
            return Path(json.loads(result.stdout.splitlines()[-1])["package"])
        # A preset/cache pointing at another SDK must not defeat --sdk resolution.
        trap = root / "wrong SDK/lib/cmake/Ludus"
        trap.mkdir(parents=True)
        (trap / "LudusConfig.cmake").write_text('message(FATAL_ERROR "Unresolved SDK was selected")\n')
        local_presets = game / "CMakeUserPresets.json"
        local = json.loads(local_presets.read_text())
        local["configurePresets"][0]["cacheVariables"]["CMAKE_PREFIX_PATH"] = str(root / "wrong SDK")
        local_presets.write_text(json.dumps(local))
        packaged = package()
        assert package() == packaged, "identical payload must reuse the verified package"
        run([cli, "project", "package", "verify", packaged], cwd=root, env=env)
        run([cli, "project", "publish", "plan", game, "--package", packaged, "--destination", "macos", "--allow-local-inputs"], cwd=root, env=env)
        extracted = root / "unrelated location"
        # Use the installed wheel's checked extractor, outside the source tree.
        program = "from pathlib import Path; from ludus_tools.package_verify import extract_archive; import sys; root=Path(sys.argv[2]); root.mkdir(); extract_archive(Path(sys.argv[1]), root)"
        run([cli.parent / "python", "-c", program, packaged / "game.zip", extracted], cwd=root, env=env)
        runtime_env = {"PATH": os.defpath, "LUDUS_DIAGNOSTIC_INTERACTIVE": "0"}
        run(["/usr/bin/codesign", "--verify", "--strict", "--deep", extracted / "Game.app"], cwd=root, env=runtime_env)
        run([extracted / "Game.app/Contents/MacOS/Game", "--version"], cwd=root, env=runtime_env)
        manifest = json.loads((extracted / "build-info.json").read_text())
        assert manifest["policy"] == "macos-release-adhoc-1"
        assert manifest["sdk"]["flavor"] == "Release"
        # Renderer games load Platform's libbsm, GraphicsRhi's MetalFX/IOSurface and,
        # when the SDK has it, the bundled OIDN denoiser runtime.
        assert {"/usr/lib/libbsm.0.dylib", "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface",
                "/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX"} <= set(manifest["systemLibraries"])
        if (relocated / "lib/libOpenImageDenoise.2.dylib").exists():
            assert (extracted / "Game.app/Contents/Frameworks/libOpenImageDenoise.2.dylib").is_file()
        assert manifest["localInputs"] is True
        assert any((extracted / "licenses/Ludus").rglob("*"))
        assert not any(file.suffix in (".a", ".h", ".hpp") for file in extracted.rglob("*") if file.is_file())
        # A failed current build cannot repackage its previous executable.
        source = game / "src/main.cpp"
        before = source.read_bytes()
        source.write_bytes(before + b"\ninvalid compilation fixture\n")
        failure = run([cli, "project", "package", game, "--profile", "macos-release", "--version", "0.1.1"], cwd=root, env=env, success=False)
        assert "BuildFailed" in failure.stderr
        source.write_bytes(before)
        assert len([p for p in (game / "out/packages/macos-release").iterdir() if p.is_dir() and not p.name.startswith(".")]) == 1
        run([cli, "project", "package", "verify", packaged], cwd=root, env=env)
        # Existing Development projects can explicitly author release setup.
        other = root / "Existing Game"
        run([cli, "project", "create", other, "--name", "Existing", "--sdk", relocated, "--tools", tools, "--profile", "macos-clang-release"], cwd=root, env=env)
        run([cli, "project", "release", "init", other, "--platform", "macos"], cwd=root, env=env)
        result = run([cli, "project", "release", "init", other, "--platform", "macos"], cwd=root, env=env, success=False)
        assert "Conflict" in result.stderr
    print("macOS installed Release SDK: renderer-linked app signing, deterministic reuse, relocation, offline plan, no-overwrite setup and stale-build refusal passed")


if __name__ == "__main__": main()
