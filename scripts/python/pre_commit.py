"""Format staged C/C++ without including unstaged edits in the commit."""
from __future__ import annotations

import subprocess

from engine import EngineError, FORMAT_SUFFIXES, find_system_tool, load_tool_versions, repo_root
from formatting import FormattingError, format_source


def git(*args: str, input: bytes | None = None) -> bytes:
    return subprocess.run(["git", *args], input=input, capture_output=True, check=True).stdout


def main() -> None:
    root = repo_root()
    paths = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split(b"\0")
    files = [root / path.decode("utf-8") for path in paths if path]
    files = [path for path in files if path.suffix in FORMAT_SUFFIXES
             and path.relative_to(root).parts[0] in ("modules", "apps", "tests")]
    if not files:
        return
    formatter = find_system_tool(root, "clang-format", load_tool_versions(root))
    for path in files:
        relative = str(path.relative_to(root))
        entry = git("ls-files", "--stage", "-z", "--", relative).split(b"\t", 1)[0]
        mode, _, stage = entry.decode().split()
        if mode not in ("100644", "100755") or stage != "0":
            continue
        staged = git("show", f":{relative}")
        formatted = format_source(staged.decode("utf-8"), path, formatter).encode("utf-8")
        if formatted == staged:
            continue
        blob = git("hash-object", "-w", "--stdin", input=formatted).decode().strip()
        git("update-index", "--cacheinfo", mode, blob, relative)
        # Sync fully staged files only. Partial staging retains every unstaged byte.
        if not path.is_symlink() and path.is_file() and path.read_bytes() == staged:
            path.write_bytes(formatted)
        print(f"Auto-formatted staged {relative}")


if __name__ == "__main__":
    try:
        main()
    except (EngineError, FormattingError, OSError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"Pre-commit formatting failed: {error}")
        raise SystemExit(1) from error
