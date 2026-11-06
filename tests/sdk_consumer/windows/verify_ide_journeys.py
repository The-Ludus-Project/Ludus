"""Exercise the VS Code/Ninja and Visual Studio/MSBuild journeys of a Windows project (D6b).

A project created by the shipped CLI against the relocated Debug and Development
SDKs is configured, built and tested by repair, then checked the way the IDEs
use it: the CMake File API code model that drives navigation, the tracked VS Code
launch configuration, the solution's startup project and debugger directory,
source-line breakpoints, stepping and locals in the Windows debugger engine
(cdb, which shares the engine behind Visual Studio and VS Code's cppvsdbg),
compiler diagnostics, install and configuration switching.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from types import SimpleNamespace

import cmake_targets

ROOT = Path(__file__).resolve().parents[3]
TARGET = "ludus_ide_journey"
CLIENT = "ludus-cli"
NINJA = "windows-msvc-debug"
VISUAL_STUDIO = "windows-msvc-visual-studio-debug"
# A breakpoint in a callee, a step out, a step over and two locals.
MAIN = """#include <ludus/foundation/base/types.h>
#include <ludus/foundation/base/version.hpp>

using ludus::foundation::int32;

int32 ComputeAnswer(int32 input)
{
    const int32 doubled = input * 2;
    return doubled; // ide-journey: breakpoint
}

int main()
{
    const int32 answer = ComputeAnswer(21);
    const int32 status = answer == 42 && ludus::foundation::version_string().empty() == false ? 0 : 1; // ide-journey: step
    return status;
}
"""
# Source-mode stepping (l+t) matches the IDEs' Step Over and Step Out.
DEBUGGER_SCRIPT = """.lines -e
l+t
bp `{module}!main.cpp:{breakpoint}`
g
dv /t doubled
k 2
gu
p
.frame 0
dv /t answer
q
"""


def line_of(marker):
    return next(number for number, line in enumerate(MAIN.splitlines(), 1) if marker in line)


CMAKE = ROOT / "out/host-tools/venv/Scripts/cmake.exe"


def run(*arguments, cwd=ROOT, fails=False, contains=(), timeout=1800):
    """Run a command; ``fails=None`` accepts any exit status."""
    environment = dict(os.environ, PYTHONPATH=str(ROOT / "scripts/python"))
    result = subprocess.run(list(map(str, arguments)), cwd=cwd, env=environment, text=True, errors="replace",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    print(result.stdout, end="", flush=True)
    if fails is not None and (result.returncode != 0) != fails:
        raise RuntimeError(f"Unexpected result {result.returncode} for {arguments}")
    for expected in [contains] if isinstance(contains, str) else contains:
        if expected not in result.stdout:
            raise RuntimeError(f"Expected {expected!r} from {arguments}")
    return result.stdout


def project_cli(*arguments, **options):
    return run(sys.executable, "-m", "ludus_tools", "project", *arguments, **options)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def same_path(left, right):
    return os.path.normcase(os.path.normpath(str(left))) == os.path.normcase(os.path.normpath(str(right)))


def executable_for(project, profile, configuration):
    engine = SimpleNamespace(EngineError=RuntimeError)
    selected = configuration if profile.startswith("windows-msvc-visual-studio-") else None
    return cmake_targets.target_executable(project / "out/build" / profile, TARGET, engine, client=CLIENT,
                                           configuration=selected)


def verify_navigation(project, profile, sdk):
    """The code model CMake Tools gives the C/C++ language service names the SDK headers."""
    reply = project / "out/build" / profile / ".cmake/api/v1/reply"
    index = json.loads(sorted(reply.glob("index-*.json"))[-1].read_text(encoding="utf-8"))
    model = json.loads((reply / index["reply"][f"client-{CLIENT}"]["codemodel-v2"]["jsonFile"]).read_text())
    (configuration,) = model["configurations"]
    (target,) = [entry for entry in configuration["targets"] if entry["name"] == TARGET]
    details = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
    sources = [source["path"] for source in details["sources"]]
    require("src/main.cpp" in sources, f"{profile}: the code model omits src/main.cpp: {sources}")
    includes = [entry["path"] for group in details["compileGroups"] for entry in group.get("includes", [])]
    require(any(same_path(path, sdk / "include") for path in includes),
            f"{profile}: the code model lacks the SDK include directory: {includes}")
    if profile == NINJA:
        commands = json.loads((project / "out/build" / profile / "compile_commands.json").read_text())
        require(any(entry["file"].replace("\\", "/").endswith("src/main.cpp") for entry in commands),
                "compile_commands.json omits src/main.cpp")
    print(f"{profile}: code model resolves src/main.cpp against {sdk / 'include'}", flush=True)


def verify_visual_studio_launch(project, profile):
    tree = project / "out/build" / profile
    (solution,) = tree.glob("*.sln")
    projects = re.findall(r'^Project\("\{[^}]+\}"\) = "([^"]+)"', solution.read_text(encoding="utf-8-sig"),
                          re.MULTILINE)
    require(projects and projects[0] == TARGET, f"F5 would start {projects[:1]}, not {TARGET}")
    settings = (tree / f"{TARGET}.vcxproj").read_text(encoding="utf-8-sig")
    directory = re.search(r"<LocalDebuggerWorkingDirectory>([^<]+)</LocalDebuggerWorkingDirectory>", settings)
    require(directory is not None and same_path(directory.group(1), project),
            "The Visual Studio debugger does not start in the project directory")
    print(f"{profile}: F5 starts {TARGET} in {project}", flush=True)


def find_debugger():
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10/Debuggers/x64"
    debugger = shutil.which("cdb") or (kits / "cdb.exe")
    require(Path(debugger).is_file(), "Install the Windows SDK Debugging Tools for cdb.exe")
    return Path(debugger)


def verify_debugging(project, profile, debugger, workspace):
    """Source breakpoint, locals, step out, step over and stack, as the IDE debuggers drive them."""
    executable = executable_for(project, profile, "Debug")
    require(executable.with_suffix(".pdb").is_file(), f"{profile}: no program database beside {executable}")
    script = workspace / f"{profile}.cdb"
    script.write_text(DEBUGGER_SCRIPT.format(module=TARGET, breakpoint=line_of("ide-journey: breakpoint")),
                      encoding="utf-8")
    # The session ends by terminating the debuggee, so only its transcript is checked.
    output = run(debugger, "-lines", "-y", executable.parent, "-srcpath", project, "-cf", script, executable,
                 cwd=project, fails=None, timeout=300)
    step = line_of("ide-journey: step")
    for expected in ("Breakpoint 0 hit", "int doubled = 0n42", f"{TARGET}!ComputeAnswer", f"{TARGET}!main",
                     f"main.cpp @ {step}", "int answer = 0n42"):
        require(expected in output, f"{profile}: the debugger session did not show {expected!r}")
    print(f"{profile}: breakpoint, locals, step out and step over reached main.cpp line {step}", flush=True)


def verify_diagnostics(project, profile):
    source = project / "src/main.cpp"
    broken = MAIN.replace("    return status;", "    return undeclared_status;")
    source.write_text(broken, encoding="utf-8")
    line = line_of("    return status;")
    output = project_cli("build", project, "--profile", profile, fails=True)
    location = re.search(r"([A-Za-z]:[^\r\n(]*main\.cpp)\((\d+)(?:,\d+)?\): error C\d{4}", output)
    require(location is not None and same_path(location.group(1), source) and int(location.group(2)) == line,
            f"{profile}: MSVC did not report an absolute main.cpp({line}) location for the Problems panel")
    source.write_text(MAIN, encoding="utf-8")
    project_cli("build", project, "--profile", profile)
    print(f"{profile}: compiler diagnostic located main.cpp({line}); the fix rebuilt", flush=True)


def verify_install(project, profile, configuration, workspace):
    prefix = workspace / f"installed {profile}"
    if prefix.exists():
        shutil.rmtree(prefix)
    run(CMAKE, "--install", project / "out/build" / profile, "--config", configuration, "--prefix", prefix)
    installed = prefix / "bin" / f"{TARGET}.exe"
    require((prefix / "bin/D3D12/D3D12Core.dll").is_file(), f"{profile}: install omitted the Agility runtime")
    run(installed, cwd=prefix)
    print(f"{profile}: installed and launched {installed}", flush=True)


def verify(workspace):
    sdks = workspace / "relocated SDKs"
    journeys = workspace / "ide journeys"
    if journeys.exists():
        shutil.rmtree(journeys)
    journeys.mkdir(parents=True)
    project = journeys / "project"
    project_cli("create", project, "--name", TARGET, "--tools", ROOT, "--sdk", sdks / "Debug",
                "--profile", NINJA, "--components", "FoundationBase", contains="version_query")
    launch = json.loads((project / ".vscode/launch.json").read_text(encoding="utf-8"))
    require(launch["configurations"][0]["type"] == "cppvsdbg", "The VS Code launch configuration is not cppvsdbg")
    require(launch["configurations"][0]["program"] == "${command:cmake.launchTargetPath}",
            "The VS Code launch configuration does not follow the CMake Tools target")
    (project / "src/main.cpp").write_text(MAIN, encoding="utf-8")

    debugger = find_debugger()
    for profile in (NINJA, VISUAL_STUDIO):
        project_cli("repair", project, "--tools", ROOT, "--profile", profile, "--sdk", sdks / "Debug",
                    contains="100% tests passed")
        settings = json.loads((project / ".vscode/settings.json").read_text(encoding="utf-8"))
        require(settings.get("C_Cpp.default.configurationProvider") == "ms-vscode.cmake-tools",
                "Repair did not route C/C++ navigation through CMake Tools")
        verify_navigation(project, profile, sdks / "Debug")
        if profile == VISUAL_STUDIO:
            verify_visual_studio_launch(project, profile)
        verify_debugging(project, profile, debugger, journeys)
        verify_diagnostics(project, profile)
        verify_install(project, profile, "Debug", journeys)

    # Switch configurations both ways without disturbing the other build trees.
    for profile in ("windows-msvc-development", "windows-msvc-visual-studio-development"):
        project_cli("repair", project, "--tools", ROOT, "--profile", profile, "--sdk", sdks / "Development",
                    contains="100% tests passed")
        verify_install(project, profile, "RelWithDebInfo", journeys)
    for profile in (NINJA, VISUAL_STUDIO, "windows-msvc-development", "windows-msvc-visual-studio-development"):
        project_cli("check", project, "--tools", ROOT, "--profile", profile)
        project_cli("run", project, "--profile", profile)


if __name__ == "__main__":
    verify(Path(sys.argv[1]).resolve())
