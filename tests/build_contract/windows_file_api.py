"""Launch the engine WARP suite from its configuration-specific File API reply."""
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import cmake_targets


CLIENT = "ludus-windows-ci"
TARGET = "ludus_rhi_d3d12_tests"
ROOT = Path(__file__).resolve().parents[2]


def launch(build, configuration, expected):
    engine = SimpleNamespace(EngineError=ValueError)
    cmake_targets.verify_identity(build, ROOT, engine, client=CLIENT)
    executable = cmake_targets.target_executable(build, TARGET, engine, client=CLIENT,
                                                configuration=configuration)
    if executable != expected:
        raise ValueError(f"{configuration}: File API selected {executable}, CTest selected {expected}")

    # Catch2 returns the number of matches when listing tests.
    cases = subprocess.run([str(executable), "--list-tests"], cwd=executable.parent,
                           capture_output=True, text=True, timeout=30).stdout
    for required in ("Portable texture transfers initialize mips",
                     "draws the fullscreen pipeline across resizes"):
        if required not in cases:
            raise ValueError(f"{executable}: missing D3D12 test {required!r}")
    print(f"File API launching {configuration}: {executable}", flush=True)
    subprocess.run([str(executable)], cwd=executable.parent, check=True, timeout=60)


if __name__ == "__main__":
    mode, tree, *arguments = sys.argv[1:]
    build = Path(tree).resolve()
    if mode == "query":
        cmake_targets.query_codemodel(build, CLIENT)
    elif mode == "launch":
        configuration, expected = arguments
        launch(build, configuration, Path(expected).resolve(strict=True))
    else:
        raise ValueError(f"Unknown File API verification mode {mode!r}")
