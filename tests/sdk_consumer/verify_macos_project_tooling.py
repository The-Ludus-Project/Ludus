#!/usr/bin/env python3
"""Installed CLI acceptance: relocated macOS SDK, fresh projects and repairs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(argv, *, cwd, env, success=True):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, text=True,
                            capture_output=True, timeout=600)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Command status {result.returncode}: {argv}\n{result.stdout}\n{result.stderr}")
    print(result.stdout, end="")
    return result


def snapshot(project):
    return {str(p.relative_to(project)): p.read_bytes() for p in project.rglob("*") if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "tools", "cli"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    sdk, tools, cli = args.sdk.resolve(), args.tools.resolve(), args.cli.absolute()
    env = {key: value for key, value in os.environ.items()
           if key not in ("PYTHONPATH", "LUDUS_SDK_PREFIX", "LUDUS_SDK_STORE", "BUTLER_API_KEY", "CXX")}
    env["LUDUS_DIAGNOSTIC_INTERACTIVE"] = "0"
    with tempfile.TemporaryDirectory(prefix="ludus mac project ") as temporary:
        root = Path(temporary).resolve()
        env["LUDUS_SDK_STORE"] = str(root / "store")
        relocated = root / "relocated sdk"
        shutil.copytree(sdk, relocated, symlinks=True)
        for file in (relocated / "lib/cmake/Ludus/dependencies").rglob("*.cmake"):
            assert "/out/host-tools/macos-sdk/" not in file.read_text(), file
        run([cli, "--version"], cwd=root, env=env)
        run([cli.parent / "python", "-c", "import ludus_tools,sys; assert not any(m == 'engine' or m.startswith(('PyQt','PySide')) for m in sys.modules)"], cwd=root, env=env)
        projects = []
        for number in (1, 2):
            project = root / f"Game {number}"
            run([cli, "project", "create", project, "--name", f"Game {number}", "--sdk", relocated,
                 "--components", "FoundationBase,FoundationFilesystem,Platform,GraphicsRhi,Content,Audio,AudioContent,Text",
                 "--tools", tools], cwd=root, env=env)
            descriptor = json.loads((project / "ludus.project.json").read_text())
            assert descriptor["preset"] == "macos-clang-development", descriptor
            assert descriptor["template"]["version"] == 4
            for tracked in ("ludus.project.json", "ludus.lock.json", "CMakeLists.txt", "CMakePresets.json"):
                assert str(root) not in (project / tracked).read_text(), tracked
            run([cli, "project", "check", project, "--tools", tools], cwd=root, env=env)
            run([cli, "project", "run", project], cwd=root, env=env)
            database = json.loads((project / "out/build/macos-clang-development/compile_commands.json").read_text())
            assert database and all(Path(entry["file"]).is_relative_to(project) for entry in database)
            assert all(str(sdk) not in entry["command"] and "out/conan" not in entry["command"] for entry in database)
            local = json.loads((project / ".ludus/local.json").read_text())
            assert local["sdk_overrides"][0]["prefix"] == str(relocated)
            # All native flavors have selectable configure/build/test presets;
            # each additional flavor still requires its own matching SDK.
            cmake = tools / "out/host-tools/venv/bin/cmake"
            for kind in ("configure", "build", "test"):
                output = run([cmake, f"--list-presets={kind}"], cwd=project, env=env).stdout
                for flavor in ("debug", "development", "release"):
                    assert '"macos-clang-' + flavor + '"' in output, output
            projects.append(project)
        project = projects[0]
        cmake_file = project / "CMakeLists.txt"
        for fixture in ("diagnostic.slang", "uniforms.slang"):
            shutil.copy2(Path(__file__).with_name("shaders") / fixture, project / fixture)
        target = json.loads((project / "ludus.project.json").read_text())["target"]
        with cmake_file.open("a") as file:
            file.write(f"\nludus_compile_shader(TARGET {target} NAME sample SOURCE diagnostic.slang VERTEX VertexMain FRAGMENT FragmentMain)\n")
        run([cli, "project", "repair", project, "--tools", tools], cwd=root, env=env)
        shader = project / f"out/build/macos-clang-development/ludus-shaders/{target}/sample/sample.fragment.metal"
        assert shader.is_file(), shader
        # Fresh clone has no owned machine settings: read-only check must fail
        # without running project configuration or changing any file.
        clone = root / "Fresh clone"
        shutil.copytree(project, clone, ignore=shutil.ignore_patterns("out", ".ludus", "CMakeUserPresets.json", ".vscode"))
        before = snapshot(clone)
        run([cli, "project", "check", clone, "--tools", tools], cwd=root, env=env, success=False)
        assert before == snapshot(clone)
        run([cli, "project", "repair", clone, "--tools", tools, "--sdk", relocated], cwd=root, env=env)
        run([cli, "project", "run", clone], cwd=root, env=env)
        # Relocate both tools and SDK, preserve custom settings, refresh caches
        # and repeat repair. A symlink-only prepared tool tree is sufficient.
        moved = root / "moved sdk"
        relocated.rename(moved)
        moved_tools = root / "moved tools"
        (moved_tools / "out").mkdir(parents=True)
        for directory in ("host-tools", "shader-tools"):
            (moved_tools / "out" / directory).symlink_to(tools / "out" / directory)
        presets_file = project / "CMakeUserPresets.json"
        presets = json.loads(presets_file.read_text())
        custom = {"name": "my-build", "configurePreset": "macos-clang-development"}
        presets["buildPresets"].append(custom)
        presets_file.write_text(json.dumps(presets))
        settings_file = project / ".vscode/settings.json"
        settings = json.loads(settings_file.read_text())
        settings["editor.tabSize"] = 8
        settings_file.write_text(json.dumps(settings))
        before = snapshot(project)
        run([cli, "project", "check", project, "--tools", moved_tools], cwd=root, env=env, success=False)
        assert before == snapshot(project)
        for _ in range(2):
            run([cli, "project", "repair", project, "--tools", moved_tools, "--sdk", moved], cwd=root, env=env)
            run([cli, "project", "check", project, "--tools", moved_tools], cwd=root, env=env)
            assert custom in json.loads(presets_file.read_text())["buildPresets"]
            assert json.loads(settings_file.read_text())["editor.tabSize"] == 8
        run([cli, "project", "run", project], cwd=root, env=env)
    print("macOS installed CLI: two projects, relocated SDK, Metal shaders, fresh clone and repeated moved-input repair passed")


if __name__ == "__main__":
    main()
