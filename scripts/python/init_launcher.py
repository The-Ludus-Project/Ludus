"""Select a supported Python before importing setup or creating its GUI.

Keep this bootstrap readable by macOS's bundled Python 3.9. Actual setup uses
the minimum version recorded in tool_versions.json.

The macOS Dock labels a running process with the CFBundleName of the bundle
that holds its executable, read from disk. A framework Python runs its GUI
from Python.app, so the setup window appeared as "Python". Changing the name
at run time renames only the menu bar and the LaunchServices display name, and
LaunchServices refuses a new CFBundleName. The launcher therefore runs setup
from out/init/Ludus Setup.app, whose executable is a symbolic link to that
Python.app executable.
"""
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import subprocess
import sys


MACOS_APPLICATION_NAME = "Ludus Setup"
MACOS_BUNDLE_IDENTIFIER = "org.ludus.setup"
MACOS_EXECUTABLE_NAME = "ludus-setup"
# Setup runs without a window, so the Dock never shows it.
TERMINAL_ONLY_FLAGS = ("--cli", "--ci")


def interpreter_candidates(root):
    candidates = []
    if platform.system() == "Darwin":
        # Reuse the interpreter prepared by init rather than Apple's old Tk.
        candidates.append(str(root / "out/host-tools/venv/bin/python"))
    candidates.append(sys.executable)
    versioned = []
    for directory in os.get_exec_path():
        try:
            for path in Path(directory).iterdir():
                match = re.fullmatch(r"python(\d+)\.(\d+)", path.name)
                if match and os.access(path, os.X_OK) and path.is_file():
                    versioned.append((tuple(map(int, match.groups())), str(path)))
        except OSError:
            continue
    candidates.extend(path for _, path in sorted(versioned, reverse=True))
    return list(dict.fromkeys(candidates))


def select_interpreter(root):
    versions = json.loads((root / "config/tool_versions.json").read_text(encoding="utf-8"))
    minimum = tuple(map(int, versions["minimum"]["python"].split(".")))
    for candidate in interpreter_candidates(root):
        if not Path(candidate).is_file():
            continue
        try:
            result = subprocess.run(
                [candidate, "-I", "-c", "import json,sys; print(json.dumps(list(sys.version_info[:3])))"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            if result.returncode == 0 and tuple(json.loads(result.stdout)) >= minimum:
                return candidate
        except (OSError, ValueError, subprocess.TimeoutExpired):
            continue
    required = versions["minimum"]["python"]
    raise RuntimeError(
        "Ludus setup requires Python " + required + " or later. Install a supported Python "
        "and add it to PATH, then rerun ./init.sh. On macOS, use Homebrew or python.org.")


def macos_application_executable(interpreter):
    """Return the Python.app executable that a framework interpreter runs, or None."""
    try:
        result = subprocess.run(
            [interpreter, "-I", "-c", "import sys; print(sys.base_prefix)"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    executable = Path(result.stdout.strip()) / "Resources/Python.app/Contents/MacOS/Python"
    return str(executable) if executable.is_file() else None


def replace_file(path, write):
    temporary = path.with_name(path.name + ".tmp" + str(os.getpid()))
    try:
        write(temporary)
        os.replace(temporary, path)
    finally:
        if temporary.is_symlink() or temporary.exists():
            temporary.unlink()


def prepare_macos_application(root, interpreter):
    """Return the Ludus Setup.app executable that runs interpreter, or None to run it directly."""
    target = macos_application_executable(interpreter)
    if target is None:
        return None
    contents = root / "out/init" / (MACOS_APPLICATION_NAME + ".app") / "Contents"
    executable = contents / "MacOS" / MACOS_EXECUTABLE_NAME
    information = plistlib.dumps({
        "CFBundleDisplayName": MACOS_APPLICATION_NAME,
        "CFBundleExecutable": MACOS_EXECUTABLE_NAME,
        "CFBundleIdentifier": MACOS_BUNDLE_IDENTIFIER,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": MACOS_APPLICATION_NAME,
        "CFBundlePackageType": "APPL",
        "NSHighResolutionCapable": True,
    })
    property_list = contents / "Info.plist"
    try:
        executable.parent.mkdir(parents=True, exist_ok=True)
        # A link, unlike a copy, follows the interpreter when Homebrew upgrades it.
        if not executable.is_symlink() or os.readlink(executable) != target:
            replace_file(executable, lambda path: os.symlink(target, path))
        if not property_list.is_file() or property_list.read_bytes() != information:
            replace_file(property_list, lambda path: path.write_bytes(information))
    except OSError:
        return None
    return str(executable)


def main():
    shell_sdkroot = os.environ.pop("LUDUS_INIT_SHELL_SDKROOT", None)
    if shell_sdkroot is not None:
        if shell_sdkroot:
            os.environ["SDKROOT"] = shell_sdkroot
        else:
            os.environ.pop("SDKROOT", None)
    root = Path(__file__).resolve().parents[2]
    try:
        interpreter = select_interpreter(root)
    except (OSError, ValueError, RuntimeError) as error:
        print("Ludus setup: " + str(error), file=sys.stderr)
        return 1
    executable = interpreter
    if platform.system() == "Darwin" and not any(flag in sys.argv[1:] for flag in TERMINAL_ONLY_FLAGS):
        application = prepare_macos_application(root, interpreter)
        if application is not None:
            # Like the framework's own interpreter stub, name the interpreter that
            # was selected so Python keeps its sys.executable and virtual environment.
            os.environ["__PYVENV_LAUNCHER__"] = interpreter
            executable = application
    os.execv(executable, [executable, str(root / "scripts/python/engine.py"), "init", *sys.argv[1:]])


if __name__ == "__main__":
    sys.exit(main())
