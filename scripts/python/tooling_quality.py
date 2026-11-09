"""Pinned, explicit installation and bounded checks for repository host tooling."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile
import urllib.request

import engine
from quality_process import run_bounded


def python_sources(root: Path) -> list[Path]:
    """Include tracked Python entry points without extensions; exclude vendor code."""
    result = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True, capture_output=True, timeout=15)
    sources = []
    for name in result.stdout.decode().split("\0"):
        if not name or name.startswith(("third_party/", "out/")):
            continue
        path = root / name
        if not path.is_file():
            continue
        with path.open("rb") as source:
            first = source.readline(256)
        if path.suffix == ".py" or (first.startswith(b"#!") and b"python" in first):
            sources.append(path)
    if not sources:
        raise engine.EngineError("Required Python scope is empty")
    return sources


def actionlint_platform() -> str:
    system = {"Darwin": "darwin", "Linux": "linux"}.get(platform.system())
    arch = {"x86_64": "amd64", "amd64": "amd64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine().lower())
    if not system or not arch:
        raise engine.EngineError("Quality tools support Linux/macOS x86_64 and arm64")
    return f"{system}_{arch}"


def install_actionlint(root: Path, archive: bytes, expected: str) -> None:
    if hashlib.sha256(archive).hexdigest() != expected:
        raise engine.EngineError("actionlint archive checksum mismatch")
    directory = engine.host_tools_bin_dir(root)
    directory.mkdir(parents=True, exist_ok=True)
    # Extract only the executable, never archive paths, links or helper scripts.
    with tempfile.TemporaryFile() as stream:
        stream.write(archive)
        stream.seek(0)
        with tarfile.open(fileobj=stream, mode="r:gz") as package:
            members = [entry for entry in package.getmembers() if entry.name == "actionlint"]
            if len(members) != 1 or not members[0].isfile() or members[0].size > 64 * 1024 * 1024:
                raise engine.EngineError("Invalid actionlint executable in archive")
            source = package.extractfile(members[0])
            if source is None:
                raise engine.EngineError("Missing actionlint executable")
            data = source.read()
    temporary = directory / "actionlint.download"
    temporary.write_bytes(data)
    temporary.chmod(0o755)
    temporary.replace(directory / "actionlint")


def bootstrap(root: Path) -> None:
    engine.check_current_python(root, engine.load_tool_versions(root)["minimum"]["python"])
    pins = engine.load_tool_versions(root)["quality"]
    key = actionlint_platform()
    engine.create_or_update_venv(root)
    engine.run([engine.venv_python(root), "-m", "pip", "install", "--disable-pip-version-check",
                f"ruff=={pins['ruff']}"], cwd=root)
    pin = pins["actionlint"]
    # Thanks to rhysd, "actionlint Installation": use the versioned release
    # archive. Its reviewed checksum is committed, not fetched during validation.
    # https://github.com/rhysd/actionlint/blob/main/docs/install.md
    # The download query avoids gateway failures seen on the bare archive URL.
    url = (
        f"https://github.com/rhysd/actionlint/releases/download/v{pin['version']}/"
        f"actionlint_{pin['version']}_{key}.tar.gz?download=1"
    )
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(64 * 1024 * 1024 + 1)
    if len(data) > 64 * 1024 * 1024:
        raise engine.EngineError("actionlint archive exceeds download limit")
    install_actionlint(root, data, pin["sha256"][key])


def check(root: Path) -> None:
    parent = root / "out/quality/tooling"
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    print(f"Tooling evidence: {directory}", flush=True)
    report = {"version": 1, "state": "error", "results": []}
    try:
        pins = engine.load_tool_versions(root)["quality"]
        sources = python_sources(root)
        workflow_root = root / ".github/workflows"
        workflows = sorted([*workflow_root.glob("*.yml"), *workflow_root.glob("*.yaml")])
        if not workflows:
            raise engine.EngineError("Required workflow scope is empty")
        tools = [("ruff", engine.managed_executable(root, "ruff"), pins["ruff"], ["--version"],
                  ["check", "--config", str(root / "ruff.toml"), *map(str, sources)]),
                 ("actionlint", engine.host_tools_bin_dir(root) / "actionlint", pins["actionlint"]["version"],
                  ["-version"], ["-color", "-shellcheck=", "-pyflakes=", *map(str, workflows)])]
        for name, executable, expected, version_args, args in tools:
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise engine.EngineError(f"Missing {name}; run ./scripts/setup-quality-tools")
            version = run_bounded([executable, *version_args], cwd=root, log=directory / f"{name}-version.log", timeout=15)
            output = version.log.read_text(errors="replace")
            match = re.search(r"\b\d+\.\d+\.\d+\b", output)
            if not version.complete or version.returncode or not match or match.group() != expected:
                raise engine.EngineError(f"{name} must be pinned version {expected}, got {output.strip()}")
            result = run_bounded([executable, *args], cwd=root, log=directory / f"{name}.log", timeout=120)
            print(result.log.read_text(errors="replace"), end="", flush=True)
            report["results"].append({"tool": name, "version": expected,
                "sha256": engine.file_sha256(executable), "argv": [str(executable), *args],
                "expected_sources": len(sources) if name == "ruff" else len(workflows),
                "exit_code": result.returncode, "complete": result.complete, "log": result.log.name})
        report["state"] = "pass" if all(r["complete"] and r["exit_code"] == 0 for r in report["results"]) else "fail"
    except (OSError, ValueError, subprocess.SubprocessError, engine.EngineError) as error:
        report["error"] = str(error)
        raise engine.EngineError(str(error)) from error
    finally:
        (directory / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    if report["state"] != "pass":
        raise engine.EngineError(f"Tooling checks failed; see {directory}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", action="store_true")
    args = parser.parse_args()
    try:
        (bootstrap if args.bootstrap else check)(engine.repo_root())
        return 0
    except (OSError, ValueError, engine.EngineError) as error:
        print(f"Quality tools: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
