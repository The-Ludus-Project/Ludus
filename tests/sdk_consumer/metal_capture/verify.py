#!/usr/bin/env python3
"""Run the installed-SDK Metal capture fixture and check its .gputrace documents.

The fixture first runs without MTL_CAPTURE_ENABLED and must refuse to capture, naming the
relaunch. It then runs with capture enabled into a fresh directory and must publish two
verified traces with version 2 manifests. A trace that passes these file checks still has to
be opened in Xcode for acceptance; see docs/architecture/graphics-debugging.md, "Metal on
macOS".
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile

RESTART_REQUIRED_EXIT = 3
CAPTURE_COUNT = 2
RUN_TIMEOUT_SECONDS = 200
MANIFEST_SUFFIX = ".manifest.json"
# Text each trace must record, by request order: the first capture draws the LUSL diagnostic
# (its generated MSL entry point), the second clears the named texture in the named pass.
# Apple's trace stores labels and runtime-compiled source as UTF-8 text.
FRAME_TEXT = (b"Ludus/Frame", b"Fixture/View")
TRACE_TEXT = (
    FRAME_TEXT + (b"FragmentMain",),
    FRAME_TEXT + (b"Fixture/OffscreenPass", b"Fixture/OffscreenColor"),
)


class FixtureError(Exception):
    """A required acceptance check failed."""


def require(condition, message):
    if not condition:
        raise FixtureError(message)


def run_fixture(executable, destination, is_capture_enabled):
    environment = {key: value for key, value in os.environ.items() if key != "MTL_CAPTURE_ENABLED"}
    if is_capture_enabled:
        environment["MTL_CAPTURE_ENABLED"] = "1"
    completed = subprocess.run(
        [executable, str(destination)],
        env=environment,
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT_SECONDS,
        check=False,
    )
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    return completed


def does_trace_contain(bundle, text):
    for entry in bundle.iterdir():
        if entry.is_file() and text in entry.read_bytes():
            return True
    return False


def load_manifest(manifest_path):
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def check_capture(destination, manifest_path, expected_text):
    manifest = load_manifest(manifest_path)
    require(manifest.get("format") == "ludus-graphics-capture", f"{manifest_path.name}: unexpected format")
    require(manifest.get("version") == 2, f"{manifest_path.name}: expected manifest version 2")
    require(manifest.get("backend") == "Metal", f"{manifest_path.name}: backend is not Metal")
    require(manifest.get("tool") == "Metal", f"{manifest_path.name}: tool is not Metal")
    require(str(manifest.get("toolVersion", "")).startswith("Metal "), f"{manifest_path.name}: no tool version")
    require(
        str(manifest.get("shaderSource", "")).startswith("generated MSL"),
        f"{manifest_path.name}: shader-source capability is not recorded",
    )
    require(manifest.get("skippedFrames") == 1, f"{manifest_path.name}: the zero-size frame was not skipped")
    require(
        manifest.get("capturedFrame", 0) > manifest.get("requestedFrame", 0),
        f"{manifest_path.name}: the captured frame does not follow the request",
    )
    artifact = str(manifest.get("artifact", ""))
    stem = manifest_path.name[: -len(MANIFEST_SUFFIX)]
    require(artifact == stem + ".gputrace", f"{manifest_path.name}: artifact {artifact!r} does not match its stem")
    bundle = destination / artifact
    require(bundle.is_dir() and any(bundle.iterdir()), f"{artifact}: missing or empty .gputrace bundle")
    for text in expected_text:
        require(does_trace_contain(bundle, text), f"{artifact}: the trace does not record {text.decode()}")
    return bundle


def main():
    if len(sys.argv) != 3:
        raise FixtureError("usage: verify.py <fixture-executable> <artifacts-directory>")
    executable = sys.argv[1]
    root = pathlib.Path(sys.argv[2])
    root.mkdir(parents=True, exist_ok=True)

    # Fresh directories per run: earlier captures are never overwritten.
    disabled = pathlib.Path(tempfile.mkdtemp(prefix="disabled-", dir=root))
    completed = run_fixture(executable, disabled, False)
    require(completed.returncode == RESTART_REQUIRED_EXIT, "capture without enablement did not report a restart")
    require("capabilities=RestartRequired" in completed.stdout, "capabilities did not report RestartRequired")
    require("MTL_CAPTURE_ENABLED=1" in completed.stdout, "the restart report does not name its fix")
    require(not any(disabled.iterdir()), "a disabled run wrote into its destination")

    enabled = pathlib.Path(tempfile.mkdtemp(prefix="enabled-", dir=root))
    completed = run_fixture(executable, enabled, True)
    require(completed.returncode == 0, f"capture run failed with exit status {completed.returncode}")
    manifests = sorted(enabled.glob("*" + MANIFEST_SUFFIX), key=lambda path: load_manifest(path).get("request", 0))
    require(len(manifests) == CAPTURE_COUNT, f"expected {CAPTURE_COUNT} manifests, found {len(manifests)}")
    require(
        len(list(enabled.iterdir())) == 2 * CAPTURE_COUNT, "the destination holds entries besides the captures"
    )
    bundles = [check_capture(enabled, manifest, text) for manifest, text in zip(manifests, TRACE_TEXT)]
    print("Metal capture fixture passed. Open a trace in Xcode to qualify it:")
    for bundle in bundles:
        print(f"  open -a Xcode '{bundle}'")


if __name__ == "__main__":
    try:
        main()
    except (FixtureError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
