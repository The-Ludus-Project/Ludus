"""Run the RenderGraph camera contract pilot against the existing Linux Editor build."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

BUILD = Path("out/build/linux-clang-development")
RENDERER = BUILD / "modules/graphics/renderer/ludus_renderer_tests"
EDITOR = BUILD / "apps/editor/ludus_editor_tests"
CONVENTIONAL = "RG2 renders a retained extraction without creating shaders or pipelines during draw"
REVERSE = "RG2 raster graphs keep finite reverse-Z cameras and reject infinite ones"
CAPTURE = "Raster comparison reports a terminal failure for an infinite-far view"
OLD_CAPTURE = "A rejected raster comparison scene reaches a terminal failure"
ADAPTER_SKIP = "No offscreen viewport GPU adapter"


def outcome(root, name, adapter_optional):
    """A process exit alone cannot establish that its required Catch2 case ran."""
    matches = [case for case in root.findall("TestCase") if case.get("name") == name]
    if len(matches) != 1:
        return "Incomplete (expected one case, found %d)" % len(matches)
    result = matches[0].find("OverallResult")
    if result is None or result.get("success") not in ("true", "false"):
        return "Incomplete (missing result)"
    if result.get("success") == "false":
        return "Failed"
    skips = [node.text.strip() if node.text else "" for node in matches[0].iter("Skip")]
    if adapter_optional and skips == [ADAPTER_SKIP]:
        return f"Inapplicable ({ADAPTER_SKIP})"
    if skips or result.get("skips", "0") != "0":
        return f"Incomplete (unexpected skip: {', '.join(skips)})"
    return "Passed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-repair", action="store_true", help="select the #1326 Editor fixture")
    args = parser.parse_args()
    capture = OLD_CAPTURE if args.before_repair else CAPTURE
    inventory = [
        ("Finite conventional depth", "admit", RENDERER, CONVENTIONAL),
        ("Finite reverse-Z", "admit", RENDERER, REVERSE),
        ("Infinite far plane", "reject", RENDERER, REVERSE),
        ("Editor raster comparison", "admit finite reverse-Z" if args.before_repair else
         "terminal failure for infinite far plane", EDITOR, capture),
    ]
    start = time.monotonic()
    rows = ["### RenderGraph camera pilot (advisory)", "",
            "| Case | Contract | Catch2 result | Local command |", "|---|---|---|---|"]
    results = {}
    with tempfile.TemporaryDirectory() as directory:
        for index, (case, expected, binary, name) in enumerate(inventory):
            command = [str(binary), name]
            if name not in results:
                report = Path(directory) / f"{index}.xml"
                try:
                    run = subprocess.run(command + ["--reporter", "console", "--reporter", f"xml::out={report}"],
                                         env={**os.environ, "QT_QPA_PLATFORM": "offscreen"}, timeout=120)
                    status = outcome(ET.parse(report).getroot(), name, binary == EDITOR)
                    skip_exit = status.startswith("Inapplicable") and run.returncode == 4
                    if run.returncode and not skip_exit and not status.startswith(("Failed", "Incomplete")):
                        status = f"Incomplete (process exited {run.returncode})"
                except (OSError, ET.ParseError, subprocess.TimeoutExpired) as error:
                    status = f"Incomplete ({error})"
                results[name] = status
            rows.append(f"| {case} | {expected} | {results[name]} | `QT_QPA_PLATFORM=offscreen {shlex.join(command)}` |")
    seconds = time.monotonic() - start
    rows.extend(["", f"Focused execution: {seconds:.3f} s ({seconds / 60:.4f} Linux runner-minutes)."])
    complete = all(s == "Passed" or s.startswith("Inapplicable") for s in results.values())
    rows.append("Pilot result: " + ("complete" if complete else "failed or incomplete"))
    summary = "\n".join(rows) + "\n"
    print(summary)
    if destination := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(destination, "a", encoding="utf-8") as stream:
            stream.write(summary)
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
