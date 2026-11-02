"""Locate and launch an already-built ludus_editor with explicit trusted tooling.

This is the thin launcher behind scripts/editor. It:
  * resolves the built `ludus_editor` executable through the shared CMake File
    API helper (never a guessed path),
  * passes the ABSOLUTE managed Python interpreter, the editor_tool.py adapter
    and the tooling root to the GUI, and the optional project to open,
  * refuses to build or install anything: if the editor target is not built, it
    prints the exact preparation commands and exits nonzero.

It does not read any Python adapter name from a project descriptor; the adapter
is always the trusted scripts/python/editor_tool.py in this checkout.
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import cmake_targets
import engine
from ludus_tools.native import default_profile


EDITOR_FILE_API_CLIENT = "ludus-editor"
EDITOR_TARGET = "ludus_editor"
SUPPORTED_PRESETS = tuple(f"{family}-clang-{flavor}" for family in ("linux", "macos")
                          for flavor in ("debug", "development", "profile", "asan-ubsan"))


def _preparation_help(root: Path, preset: str) -> str:
    return (
        "The editor is not built for this preset. Prepare it explicitly, then retry:\n"
        f"  ./init.sh --cli --with-editor {preset}\n"
        "Use --no-system-install with existing Qt 6.4+ packages; add --no-editor-preview for a lightweight document-only build."
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Launch the already-built Ludus editor")
    parser.add_argument("--preset", default=default_profile(), choices=SUPPORTED_PRESETS)
    parser.add_argument("--project", default=None, help="absolute or relative descriptor to open")
    parser.add_argument("editor_args", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)

    root = engine.repo_root()
    build_dir = engine.build_dir_for_preset(root, args.preset)

    python = engine.venv_python(root)
    if not python.exists():
        print("The managed Python interpreter is not available; run ./init.sh first.", file=sys.stderr)
        return 1

    # Ensure a File API codemodel query exists so the next configure publishes a
    # reply, then resolve the editor executable from the reply. We never build.
    cmake_targets.query_codemodel(build_dir, EDITOR_FILE_API_CLIENT)
    try:
        editor_exe = cmake_targets.target_executable(
            build_dir, EDITOR_TARGET, engine, client=EDITOR_FILE_API_CLIENT)
    except engine.EngineError as exc:
        print(str(exc), file=sys.stderr)
        print(_preparation_help(root, args.preset), file=sys.stderr)
        return 1
    if not (editor_exe.is_file() and os.access(editor_exe, os.X_OK)):
        print(_preparation_help(root, args.preset), file=sys.stderr)
        return 1

    adapter = (root / "scripts" / "python" / "editor_tool.py").resolve()
    command = [
        str(editor_exe),
        "--tooling-root", str(root),
        "--python", str(python.resolve()),
        "--adapter", str(adapter),
    ]
    if args.project:
        command += ["--project", str(Path(args.project).resolve())]
    command += [a for a in args.editor_args if a != "--"]

    print(f"+ {engine.command_line(command)}", flush=True)
    env = engine.tool_env(root)
    os.execve(command[0], command, env)
    return 0  # unreachable on success


if __name__ == "__main__":
    sys.exit(main())
