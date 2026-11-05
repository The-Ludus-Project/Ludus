"""Verify source and shader-include rebuilds using the installed SDK."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
TARGET = "ludus_windows_sdk_smoke"


def run(*arguments):
    result = subprocess.run(list(map(str, arguments)), cwd=ROOT, text=True,
                            errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=600)
    print(result.stdout, end="", flush=True)
    result.check_returncode()


def snapshot(files):
    return {path: (path.stat().st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest()) for path in files}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify(sdk, flavor, workspace, visual_studio):
    generator = "MSBuild" if visual_studio else "Ninja"
    print(f"\n=== {generator} / {flavor} ===", flush=True)
    source = workspace / generator / flavor / "source files"
    build = source.parent / "build"
    shaders = source / "shaders"
    shaders.mkdir(parents=True)
    for name in ("CMakeLists.txt", "smoke.cpp"):
        shutil.copyfile(FIXTURE / name, source / name)
    for name in ("diagnostic.slang", "uniforms.slang"):
        shutil.copyfile(FIXTURE.parent / "shaders" / name, shaders / name)

    configuration = "RelWithDebInfo" if flavor in ("Development", "Profile") else flavor
    if visual_studio:
        toolset = os.environ["VCToolsVersion"].rstrip("\\")
        options = ("-G", "Visual Studio 17 2022", "-A", "x64,version=10.0.26100.0",
                   "-T", f"v143,version={toolset},host=x64", f"-DCMAKE_CONFIGURATION_TYPES={configuration}")
    else:
        options = ("-G", "Ninja", "-DCMAKE_CXX_COMPILER=cl.exe", f"-DCMAKE_BUILD_TYPE={configuration}")
    shader_tools = ROOT / "out/shader-tools"
    run("cmake", "-S", source, "-B", build, *options, f"-DCMAKE_PREFIX_PATH={sdk}",
        f"-DEXPECTED_FLAVOR={flavor}", f"-DLUDUS_SLANG_COMPILER={shader_tools / 'slang/bin/slangc.exe'}",
        f"-DLUDUS_DXC_DIRECTORY={shader_tools / 'dxc'}")
    build_command = ("cmake", "--build", build, "--config", configuration, "--target", TARGET)
    test_command = ("ctest", "--test-dir", build, "-C", configuration, "--output-on-failure", "--no-tests=error")
    run(*build_command)
    run(*test_command)

    shader_output = build / "ludus-shaders" / TARGET / "diagnostic"
    header = shader_output / "diagnostic.h"
    vertex = shader_output / "diagnostic.vertex.dxil"
    fragment = shader_output / "diagnostic.fragment.dxil"
    shader_files = (header, vertex, fragment, shader_output / "manifest.json")
    executable = (build / configuration if visual_studio else build) / (TARGET + ".exe")
    objects = [path for path in build.rglob("*.obj") if path.name in ("smoke.cpp.obj", "smoke.obj")]
    require(len(objects) == 1, f"Expected one smoke object, got {objects}")
    object_file = objects[0]
    runtime_directory = executable.parent / "D3D12"
    runtime_files = tuple(runtime_directory / name for name in
                          ("D3D12Core.dll", "d3d12SDKLayers.dll", "LICENSE.txt"))
    installed_runtime = sdk / "lib" / "cmake" / "Ludus" / "agility"
    require(all(path.read_bytes() == (installed_runtime / path.name).read_bytes() for path in runtime_files),
            "The executable did not stage the installed Agility runtime")
    outputs = (*shader_files, object_file, executable)

    baseline = snapshot(outputs)
    core = runtime_directory / "D3D12Core.dll"
    core.unlink()
    run(*build_command)
    require(core.read_bytes() == (installed_runtime / core.name).read_bytes(),
            "An incremental build did not restore the missing Agility core")
    require(snapshot(outputs) == baseline, "Restoring the Agility runtime relinked the executable")

    def verify_idle(previous):
        run(*build_command)
        require(snapshot(outputs) == previous, "Build rewrote outputs without any input changes")

    verify_idle(baseline)

    # Cross the timestamp resolution used by build tools, then change a literal
    # that survives optimization so the object and executable must change.
    time.sleep(1.1)
    cpp = source / "smoke.cpp"
    code = cpp.read_bytes()
    title = b"Installed Ludus SDK WARP smoke"
    require(code.count(title) == 1, "Expected the smoke window title in one place")
    cpp.write_bytes(code.replace(title, title + b" (incremental C++)"))
    run(*build_command)
    after_cpp = snapshot(outputs)
    require(all(after_cpp[path] == baseline[path] for path in shader_files),
            "A C++-only edit regenerated shaders")
    require(all(after_cpp[path][1] != baseline[path][1] for path in (object_file, executable)),
            "The C++ edit did not change the object and executable")
    run(*test_command)
    verify_idle(after_cpp)
    print(f"{generator} / {flavor}: C++ edit rebuilt and relinked without shader generation", flush=True)

    # Edit only a transitive input discovered by Slang's depfile. The scale is
    # used by the fragment stage, so the fragment and embedded header must differ.
    time.sleep(1.1)
    with (shaders / "uniforms.slang").open("ab") as include:
        include.write(b"\n#define DIAGNOSTIC_SCALE 0.5\n")
    run(*build_command)
    after_include = snapshot(outputs)
    require(all(after_include[path][1] != after_cpp[path][1]
                for path in (fragment, header, object_file, executable)),
            "The include edit did not regenerate DXIL and rebuild the dependent executable")
    require(after_include[vertex][1] == after_cpp[vertex][1], "The fragment edit changed vertex DXIL")
    run(*test_command)
    verify_idle(after_include)
    print(f"{generator} / {flavor}: include edit regenerated DXIL, rebuilt and passed WARP", flush=True)


if __name__ == "__main__":
    workspace = Path(sys.argv[1]).resolve()
    for flavor in ("Debug", "Development", "Profile", "Release"):
        for visual_studio in (False, True):
            verify(workspace / "relocated SDKs" / flavor, flavor, workspace / "incremental", visual_studio)
