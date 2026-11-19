"""Build, verify and extract the World demo Metal pilot using a prepared Release SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts/python"))
from ludus_tools.package_verify import extract_archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path, help="new working directory")
    parser.add_argument("--sdk", type=Path, default=ROOT / "out/install/macos-clang-release")
    args = parser.parse_args()
    work = args.destination.resolve()
    work.mkdir(parents=True)
    project = work / "MetalBudget"
    environment = dict(os.environ, LUDUS_SDK_STORE=str(work / "store"))
    environment["PATH"] = os.pathsep.join((str(ROOT / "out/host-tools/venv/bin"),
                                           str(ROOT / "out/host-tools/bin"), environment["PATH"]))
    cli = ROOT / "scripts/ludus"

    def run(*arguments, capture=False):
        result = subprocess.run([cli, *arguments], env=environment, cwd=work, text=True,
                                stdout=subprocess.PIPE if capture else None)
        if result.returncode and result.stdout:
            print(result.stdout, file=sys.stderr)
        result.check_returncode()
        return result

    run("project", "create", project, "--name", "MetalBudget", "--sdk", args.sdk.resolve(),
        "--tools", ROOT, "--profile", "macos-clang-release", "--release")
    shutil.copyfile(FIXTURE / "pilot.cpp", project / "src/main.cpp")
    shutil.copyfile(FIXTURE / "WorldPilot.cmake", project / "cmake/WorldPilot.cmake")
    cmake_file = project / "CMakeLists.txt"
    cmake = cmake_file.read_text()
    # The starter game's default invocation prints its version. The pilot takes route arguments.
    starter_test = "add_test(NAME version_query COMMAND $<TARGET_FILE:MetalBudget>)"
    if starter_test not in cmake:
        raise ValueError("Expected the generated game's version_query test")
    cmake = cmake.replace(starter_test, "add_test(NAME version_query COMMAND $<TARGET_FILE:MetalBudget> --version)")
    cmake_file.write_text(cmake + f'\nset(pilot_root [[{ROOT}]])\ninclude(cmake/WorldPilot.cmake)\n')
    result = run("--json", "project", "package", project, "--profile", "macos-release",
                 "--version", "0.1.0", "--sdk", args.sdk.resolve(), capture=True)
    package = Path(json.loads(result.stdout.splitlines()[-1])["package"])
    run("project", "package", "verify", package)
    archive = package / "game.zip"
    extracted = work / "extracted"
    extracted.mkdir()
    extract_archive(archive, extracted)
    subprocess.run(["codesign", "--verify", "--strict", "--deep", extracted / "MetalBudget.app"], check=True)
    print("game.zip SHA-256", hashlib.sha256(archive.read_bytes()).hexdigest())
    print(extracted / "MetalBudget.app/Contents/MacOS/MetalBudget")


if __name__ == "__main__":
    main()
