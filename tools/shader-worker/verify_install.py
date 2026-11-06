#!/usr/bin/env python3
"""Check an installed tools-only shader worker prefix with one real compile.

The installed worker must report the pinned Slang version, carry its runtime libraries
and license texts, and compile a request through its native endpoint: a local named
pipe emitting signed DXIL on Windows, a private Unix socket elsewhere. Nothing is built.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
IS_WINDOWS = sys.platform == "win32"
PIPE_PREFIX = "\\\\.\\pipe\\"
STATUS_READY = 0
STAGE_FRAGMENT = 1
# Target wire values: SPIR-V 0, Metal 2, DXIL 4 (ludus::tooling::shader_compiler::Target).
TARGET = 4 if IS_WINDOWS else 2 if sys.platform == "darwin" else 0
SOURCE = '[shader("fragment")] float4 FragmentMain() : SV_Target { return float4(1, 0, 0, 1); }\n'
START_SECONDS = 10


def require(condition, message):
    if not condition:
        raise SystemExit(f"Installed shader worker check failed: {message}")


def load_client():
    spec = importlib.util.spec_from_file_location("native_worker", ROOT / "cmake/shaders/native_worker.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def endpoint_exists(endpoint):
    if IS_WINDOWS:
        return endpoint[len(PIPE_PREFIX):] in os.listdir(PIPE_PREFIX)
    return Path(endpoint).exists()


def check_layout(prefix):
    worker = prefix / "bin" / ("ludus-shader-worker.exe" if IS_WINDOWS else "ludus-shader-worker")
    require(worker.is_file(), f"missing {worker}")
    licenses = prefix / "share/ludus-shader-tools/licenses"
    require(any(path.is_file() for path in licenses.rglob("*")), f"no license texts under {licenses}")
    if IS_WINDOWS:
        for library in ("slang-compiler.dll", "slang-glslang.dll", "dxcompiler.dll", "dxil.dll"):
            require((prefix / "bin" / library).is_file(), f"{library} is not beside the worker")
        require(any((licenses / "dxc").glob("*")), "the DXC license texts are missing")
    pin = json.loads((ROOT / "config/shader_toolchain.json").read_text())["slang"]["version"]
    version = subprocess.run([str(worker), "--version"], capture_output=True, text=True, timeout=30)
    require(version.returncode == 0 and version.stdout.strip() == pin,
            f"--version reported {version.stdout.strip()!r}, expected {pin}")
    return worker


def check_compile(worker, client):
    with tempfile.TemporaryDirectory(prefix="ludus-worker-install-") as directory:
        endpoint = (PIPE_PREFIX + f"ludus-worker-install-{os.getpid()}") if IS_WINDOWS \
            else str(Path(directory) / "compiler.sock")
        process = subprocess.Popen([str(worker), "--serve", endpoint, "--stop-with-parent"])
        try:
            deadline = time.monotonic() + START_SECONDS
            while not endpoint_exists(endpoint):
                require(process.poll() is None and time.monotonic() < deadline, "the worker did not start")
                time.sleep(0.01)
            reply = client.exchange(endpoint, {"version": 1, "identity": 1, "source": "main.slang",
                                               "entry": "FragmentMain", "stage": STAGE_FRAGMENT,
                                               "target": TARGET, "files": {"main.slang": SOURCE}})
            require(reply["status"] == STATUS_READY, reply.get("diagnostics") or f"status {reply['status']}")
            artifact = bytes.fromhex(reply["artifact"])
            require(artifact, "empty artifact")
            if IS_WINDOWS:
                require(artifact[:4] == b"DXBC" and struct.unpack_from("<I", artifact, 24)[0] == len(artifact)
                        and len(set(artifact[4:20])) > 1, "the installed worker returned unsigned DXIL")
        finally:
            process.terminate()
            process.wait(timeout=START_SECONDS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    args = parser.parse_args()
    worker = check_layout(args.prefix.resolve())
    check_compile(worker, load_client())
    print(f"Installed shader worker at {args.prefix} reports the pinned Slang and compiles target {TARGET}.")


if __name__ == "__main__":
    main()
