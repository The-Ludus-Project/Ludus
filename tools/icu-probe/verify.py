"""Require real execution, an exception-free compile profile and size evidence for L0."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import os
import json
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

CASES = ("embedded-data-registration", "pinned-icu-cldr-unicode-versions", "canonical-locales-with-sliced-data",
         "profile-accepts-every-corpus-pattern", "profile-rejects-every-negative-fixture",
         "corpus-format-and-argument-validation", "complete-output-or-unchanged-caller-bytes",
         "backend-observations-require-adapter-validation", "sweep-allocation-points",
         "dynamic-format-measurements", "host-cleanup-releases-every-allocation")
SWEEPS = ("nested-select-plural", "number-skeleton", "offset-plural-text", "host-initialization")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/python"))
from engine import pinned_llvm_tool  # noqa: E402

LLVM_STRIP = pinned_llvm_tool(ROOT, "llvm-strip")

# ASan reports these signals itself; they are crashes, as on unsanitized builds.
DEADLY_SIGNALS = ("SEGV", "BUS", "FPE", "ILL", "ABRT", "stack-overflow")


def compressed(data: bytes) -> int:
    return len(gzip.compress(data, compresslevel=9, mtime=0))


def artifact(path: Path) -> dict:
    data = path.read_bytes()
    result = {"bytes": len(data), "gzip_bytes": compressed(data), "sha256": hashlib.sha256(data).hexdigest()}
    strip = shutil.which(LLVM_STRIP) or shutil.which("strip")
    if path.suffix not in (".js", ".wasm") and strip is not None:
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory) / path.name
            shutil.copyfile(path, copy)
            if subprocess.run([strip, str(copy)], capture_output=True).returncode == 0:
                stripped = copy.read_bytes()
                result.update(stripped_bytes=len(stripped), stripped_gzip_bytes=compressed(stripped))
    return result


def classify_sanitizer(output: str) -> str | None:
    """Name the first ASan/UBSan finding; a deadly signal under ASan stays a crash."""
    report = re.search(r"ERROR: AddressSanitizer: ([\w-]+)", output)
    if report is not None and report.group(1) not in DEADLY_SIGNALS:
        return report.group(1)
    if "runtime error:" in output:
        return "undefined-behavior"
    return None


def classify_sanitizer_check(output: str) -> str | None:
    """Name the check behind the first finding: the ASan kind, or the UBSan check.

    UBSan prints its check name in the summary only with `report_error_type=1`;
    otherwise the check is the first runtime error's message with operand values,
    types and addresses removed, such as `reference binding to null pointer`. A
    report whose check cannot be parsed stays `undefined-behavior`.
    """
    finding = classify_sanitizer(output)
    if finding != "undefined-behavior":
        return finding
    summary = re.search(r"SUMMARY: UndefinedBehaviorSanitizer: ([\w-]+)", output)
    if summary is not None and summary.group(1) != "undefined-behavior":
        return summary.group(1)
    message = re.search(r"runtime error: (.*)$", output, re.MULTILINE)
    check = message.group(1) if message is not None else ""
    check = re.sub(r"^call to function .+? through pointer", "call through pointer", check)
    for marker in (" of type ", " for type ", ", which", " which ", ": ", "'"):
        check = check.split(marker, 1)[0]
    check = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)\b", "N", check)
    check = " ".join(check.split())
    return check if check else "undefined-behavior"


def run_point(command: list[str], index: int, fail_at: int) -> dict:
    """Run one fail point in a fresh process; a crash is data, not a verifier error.

    Sanitized builds turn faults into reports and a nonzero exit. A null dereference
    is still a crash, as without sanitizers. An ASan or UBSan finding is a
    `sanitizer-report`. LeakSanitizer reports only at exit, after the point printed
    its result, so that result is kept and the leak is recorded beside it.
    """
    try:
        result = subprocess.run(command + ["sweep", str(index), str(fail_at)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return {"fail_at": fail_at, "outcome": "timeout"}
    match = re.search(r"L0 SWEEP (.*)$", result.stdout, re.MULTILINE)
    finding = classify_sanitizer(result.stdout)
    has_leak_report = "ERROR: LeakSanitizer" in result.stdout
    if finding is None and match is None and has_leak_report is True:
        finding = "leak"
    if finding is not None or match is None or (result.returncode != 0 and has_leak_report is False):
        if finding is None:
            return {"fail_at": fail_at, "outcome": "crash", "exit": result.returncode, "sanitizer": None}
        check = "leak" if finding == "leak" else classify_sanitizer_check(result.stdout)
        return {"fail_at": fail_at, "outcome": "sanitizer-report", "exit": result.returncode, "sanitizer": finding,
                "sanitizer-check": check}
    fields = dict(pair.split("=", 1) for pair in match.group(1).split())
    fields["fail_at"] = fail_at
    fields["sanitizer-leak"] = has_leak_report
    if fields["denied"] == "0":
        fields["outcome"] = "control"
    elif fields["initialized"] == "false":
        fields["outcome"] = "reported-initialization-failure"
    elif fields["correct"] == "true":
        fields["outcome"] = "absorbed"
    elif fields["status"] == "Ok":
        fields["outcome"] = "wrong-output"
    elif fields["status"] == "OutOfMemory":
        fields["outcome"] = "reported-oom"
    else:
        fields["outcome"] = "other-error"
    return fields


def sweep(command: list[str], points: dict[str, int]) -> dict:
    """Deny each allocation once, in isolation, for every representative case."""
    report = {}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for index, name in enumerate(SWEEPS):
            count = points[name]
            results = list(pool.map(lambda f, i=index: run_point(command, i, f), range(1, count + 2)))
            control = results[-1]
            assert control["outcome"] == "control" and control["correct"] == "true" and \
                control["leaked-blocks"] == "0" and control["sanitizer-leak"] is False, \
                f"{name}: control point is not clean: {control}"
            failures = results[:-1]
            summary = {"allocation_points": count}
            for outcome in ("crash", "sanitizer-report", "timeout", "reported-initialization-failure", "reported-oom",
                            "other-error", "absorbed", "wrong-output", "control"):
                summary[outcome] = sum(r["outcome"] == outcome for r in failures)
            survived = [r for r in failures if r["outcome"] not in ("crash", "sanitizer-report", "timeout")]
            # A reported initialization failure has no formatter to recover.
            formatted = [r for r in survived if r["outcome"] != "reported-initialization-failure"]
            summary["sticky_after_failure"] = sum(r["recovered"] == "false" for r in formatted)
            summary["leaking_after_cleanup"] = sum(r["leaked-blocks"] != "0" or r["sanitizer-leak"] is True
                                                   for r in survived)
            summary["sanitizer_leak_reports"] = sum(r["sanitizer-leak"] is True for r in survived)
            summary["misreported_backend_codes"] = sorted({r["backend"] for r in survived
                                                           if r["outcome"] == "other-error"})
            summary["crash_points"] = [r["fail_at"] for r in failures if r["outcome"] == "crash"]
            summary["crash_exit_codes"] = sorted({r["exit"] for r in failures if r["outcome"] == "crash"})
            summary["sanitizer_report_points"] = {str(r["fail_at"]): r["sanitizer"] for r in failures
                                                  if r["outcome"] == "sanitizer-report"}
            summary["sanitizer_report_checks"] = {str(r["fail_at"]): r["sanitizer-check"] for r in failures
                                                  if r["outcome"] == "sanitizer-report"}
            summary["wrong_output_points"] = [r["fail_at"] for r in failures if r["outcome"] == "wrong-output"]
            assert summary["control"] == 0, f"{name}: allocation count changed between runs"
            report[name] = summary
            print(f"L0 SWEEP SUMMARY {name} " + json.dumps(summary))
    return report


def main() -> int:
    emulator, executable, baseline, database = sys.argv[1:]
    entries = json.loads(Path(database).read_text())
    relevant = [e for e in entries if "/tools/icu-probe/" in e["file"] or "/out/icu-probe/" in e["file"]]
    upstream = [e for e in relevant if "/out/icu-probe/source/" in e["file"]]
    assert len(upstream) > 400, f"ICU compile commands missing ({len(upstream)})"
    for entry in relevant:
        flags = shlex.split(entry["command"]) if "command" in entry else entry["arguments"]
        assert "-fno-exceptions" in flags and "-fexceptions" not in flags, entry["file"]
        if "/out/icu-probe/source/" in entry["file"]:
            assert "-DUCONFIG_NO_MF2=1" in flags and "-DUCONFIG_NO_FILE_IO=1" in flags, entry["file"]
    command = ([emulator] if emulator else []) + [executable]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=580)
    print(result.stdout, end="")
    assert result.returncode == 0, f"Probe exited {result.returncode}"
    for case in CASES:
        assert f"L0 PASS {case}" in result.stdout, f"Missing executed case: {case}"
    assert "L0 FAIL" not in result.stdout
    metrics = {}
    for line in result.stdout.splitlines():
        match = re.search(r"L0 METRIC (\S+) (.*)$", line)
        if match:
            metrics[match.group(1)] = dict(pair.split("=", 1) for pair in match.group(2).split())
    observations = [line.split("L0 OBSERVED ", 1)[1] for line in result.stdout.splitlines() if "L0 OBSERVED " in line]
    points = {}
    for line in result.stdout.splitlines():
        match = re.search(r"L0 METRIC sweep-points-(\S+) allocations=(\d+)", line)
        if match:
            points[match.group(1)] = int(match.group(2))
    assert set(points) == set(SWEEPS), f"Missing sweep allocation counts: {points}"
    sweeps = sweep(command, points)
    probe, reference = Path(executable), Path(baseline)
    artifacts = {"probe": artifact(probe), "baseline": artifact(reference)}
    if emulator:
        artifacts["probe_wasm"] = artifact(probe.with_suffix(".wasm"))
        artifacts["baseline_wasm"] = artifact(reference.with_suffix(".wasm"))
    libraries = {}
    for library in sorted(probe.parent.glob("*ludus_icu_probe_*.a")):
        libraries[library.name] = library.stat().st_size
    data = ROOT / "out/icu-probe/data/manifest.json"
    pin = json.loads((ROOT / "config/icu_toolchain.json").read_text())
    evidence = {
        "execution": "SDK Node wasm" if emulator else "native",
        "host": platform.platform(),
        "icu": pin["version"],
        "profile": pin["profile"],
        "configuration": pin["configuration"],
        "data": {key: value for key, value in json.loads(data.read_text()).items() if key != "items"},
        "data_items": len(json.loads(data.read_text())["items"]),
        "artifacts": artifacts,
        "static_libraries": libraries,
        "metrics": metrics,
        "observations": observations,
        "allocation_failure_sweep": sweeps,
        "scope": "private feasibility probe, not production API or platform acceptance",
    }
    (probe.parent / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({"artifacts": artifacts, "static_libraries": libraries}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
