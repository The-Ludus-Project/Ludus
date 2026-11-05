"""Exercise the shipped project CLI with four real, relocated Windows SDKs."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from ludus_tools.buildlock import BuildTreeLock
from ludus_tools.native import WINDOWS_CONFIGURATIONS, WINDOWS_VISUAL_STUDIO_PREFIX
from ludus_tools.project_setup import OWNER, cache_value

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent


def run(*arguments, contains=None, fails=False):
    result = subprocess.run(
        [sys.executable, "-m", "ludus_tools", "project", *map(str, arguments)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(result.stdout, end="", flush=True)
    if (result.returncode != 0) != fails or (contains and contains not in result.stdout):
        raise RuntimeError(f"Unexpected result ({result.returncode}) for {arguments}")
    return result.stdout


def check_read_only(project, profile, *, tools=ROOT, fails=False, contains=None):
    before = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in project.rglob("*") if p.is_file()}
    run("check", project, "--tools", tools, "--profile", profile, contains=contains, fails=fails)
    after = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in project.rglob("*") if p.is_file()}
    assert before == after, "Setup check wrote to the project"


def verify(workspace):
    installed = workspace / "relocated SDKs"
    project = workspace / "project setup"
    run("create", project, "--name", "ludus_windows_sdk_smoke", "--tools", ROOT,
        "--sdk", installed / "Debug", "--components", "FoundationBase,GraphicsRhi,Platform",
        contains="version_query")
    descriptor = project / "ludus.project.json"
    lockfile = project / "ludus.lock.json"
    assert json.loads(descriptor.read_bytes())["preset"] == "windows-msvc-debug"

    # Use the same Win32/DXIL/WARP smoke as the external Visual Studio consumer,
    # now driven by the generated Ninja presets and the real repair CLI.
    cmake = (FIXTURE / "CMakeLists.txt").read_text()
    cmake = cmake.replace('find_package(Ludus', 'set(EXPECTED_FLAVOR "${LUDUS_PROJECT_SDK_FLAVOR}")\n\nfind_package(Ludus', 1)
    (project / "CMakeLists.txt").write_text(cmake)
    shutil.copyfile(FIXTURE / "smoke.cpp", project / "smoke.cpp")
    (project / "shaders").mkdir()
    for name in ("diagnostic.slang", "uniforms.slang"):
        shutil.copyfile(FIXTURE.parent / "shaders" / name, project / "shaders" / name)

    # Recreate a fresh clone's state, preserving only portable project files.
    shutil.rmtree(project / ".ludus")
    presets = project / "CMakeUserPresets.json"
    presets.unlink()
    check_read_only(project, "windows-msvc-debug", fails=True, contains="Project setup is missing")
    presets.write_text(json.dumps({
        "version": 6,
        "configurePresets": [{"name": "custom", "inherits": "windows-msvc-debug",
                              "binaryDir": "${sourceDir}/out/custom"}],
    }))
    ide = project / ".vscode/settings.json"
    ide.write_text(json.dumps({"editor.tabSize": 4}))
    portable = {file: file.read_bytes() for file in (descriptor, lockfile, project / "CMakePresets.json")}
    for flavor, configuration in WINDOWS_CONFIGURATIONS.items():
        profile = "windows-msvc-" + flavor.lower()
        sdk = installed / flavor
        args = ("repair", project, "--tools", ROOT, "--profile", profile)
        if flavor == "Profile":
            # Development and Profile both use RelWithDebInfo. A configure
            # setting alone cannot distinguish their installed SDKs.
            run(*args, "--sdk", installed / "Development", fails=True, contains="flavor")
        output = run(*args, "--sdk", sdk, contains="100% tests passed")
        assert "ludus_windows_sdk_smoke" in output
        cache = project / "out/build" / profile / "CMakeCache.txt"
        assert cache_value(cache, "CMAKE_BUILD_TYPE") == configuration
        assert Path(cache_value(cache, "Ludus_DIR")).is_relative_to(sdk)

    for flavor in WINDOWS_CONFIGURATIONS:
        profile = "windows-msvc-" + flavor.lower()
        check_read_only(project, profile, contains=f" · {flavor} · x86_64-pc-windows-msvc")
        expected = project / "out/build" / profile / "ludus_windows_sdk_smoke.exe"
        run("run", project, "--profile", profile, contains=str(expected))

    for flavor, configuration in WINDOWS_CONFIGURATIONS.items():
        profile = WINDOWS_VISUAL_STUDIO_PREFIX + flavor.lower()
        tree = project / "out/build" / profile
        expected = tree / configuration / "ludus_windows_sdk_smoke.exe"
        output = run("repair", project, "--tools", ROOT, "--profile", profile,
                     "--sdk", installed / flavor, contains="100% tests passed")
        assert "ludus_windows_sdk_smoke" in output
        assert cache_value(tree / "CMakeCache.txt", "CMAKE_CONFIGURATION_TYPES") == configuration
        assert cache_value(tree / "CMakeCache.txt", "CMAKE_GENERATOR") == "Visual Studio 17 2022"
        assert (tree / "LudusWindowsSdkConsumer.sln").is_file()
        check_read_only(project, profile, contains=f" · {flavor} · x86_64-pc-windows-msvc")
        expected.unlink()
        output = run("run", project, "--profile", profile, contains=str(expected))
        assert f"--config {configuration}" in output and expected.is_file()

    profile = "windows-msvc-profile"
    sdk = installed / "Profile"
    moved = workspace / "moved SDK Profile"
    sdk.rename(moved)
    # The command-line SDK takes precedence over the old path in the VS preset.
    visual_studio = WINDOWS_VISUAL_STUDIO_PREFIX + "profile"
    run("run", project, "--profile", visual_studio, "--sdk", moved, contains="SDK inputs changed; reconfiguring")
    visual_studio_cache = project / "out/build" / visual_studio / "CMakeCache.txt"
    assert Path(cache_value(visual_studio_cache, "Ludus_DIR")).is_relative_to(moved)
    check_read_only(project, profile, fails=True)
    args = ("repair", project, "--tools", ROOT, "--profile", profile, "--sdk", moved)
    run(*args, contains="100% tests passed")
    cache = project / "out/build" / profile / "CMakeCache.txt"
    assert Path(cache_value(cache, "Ludus_DIR")).is_relative_to(moved)

    local = json.loads(presets.read_bytes())
    managed = next(p for p in local["configurePresets"] if p.get("vendor", {}).get(OWNER) == profile)
    managed["cacheVariables"]["CMAKE_MAKE_PROGRAM"] = "C:/removed-tools/ninja.exe"
    presets.write_text(json.dumps(local))
    check_read_only(project, profile, fails=True, contains="Stale local preset")
    moved_tools = workspace / "moved tools"
    scripts = moved_tools / "out/host-tools/venv/Scripts"
    scripts.mkdir(parents=True)
    for executable in ("cmake.exe", "ctest.exe", "ninja.exe"):
        os.link(ROOT / "out/host-tools/venv/Scripts" / executable, scripts / executable)
    shutil.copytree(ROOT / "out/shader-tools", moved_tools / "out/shader-tools", copy_function=os.link)
    (moved_tools / "config").mkdir()
    shutil.copyfile(ROOT / "config/tool_versions.json", moved_tools / "config/tool_versions.json")
    check_read_only(project, profile, tools=moved_tools, fails=True, contains="SDK or tool inputs changed")
    args = ("repair", project, "--tools", moved_tools, "--profile", profile, "--sdk", moved)
    run(*args, contains="100% tests passed")
    run(*args, contains="100% tests passed")
    assert Path(cache_value(cache, "CMAKE_MAKE_PROGRAM")).is_relative_to(scripts)
    with BuildTreeLock(cache.parent):
        run(*args, fails=True, contains="concurrent operations")
    run("run", project, "--profile", profile)

    local = json.loads(presets.read_bytes())
    assert [p["name"] for p in local["configurePresets"] if "vendor" not in p] == ["custom"]
    for key in ("configurePresets", "buildPresets", "testPresets"):
        assert len([p for p in local[key] if OWNER in p.get("vendor", {})]) == 8
    assert json.loads(ide.read_bytes())["editor.tabSize"] == 4
    assert all(file.read_bytes() == before for file, before in portable.items())
    for flavor in WINDOWS_CONFIGURATIONS:
        check_read_only(project, "windows-msvc-" + flavor.lower(),
                        tools=moved_tools if flavor == "Profile" else ROOT)


if __name__ == "__main__":
    verify(Path(sys.argv[1]).resolve())
