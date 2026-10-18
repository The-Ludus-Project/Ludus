"""Drive the L2 format probe: corpus, measured cost and fail-stop at every ICU allocation.

Usage: test_format_probe.py EMULATOR_OR_EMPTY PROBE EVIDENCE_JSON

A denied ICU allocation must terminate through FoundationBase's emergency path
(ICU4C 78.3 cannot recover; docs/architecture/localization-l0.md). Every point
runs in a fresh process because termination is the expected outcome. Writes
the metrics and sweep summary to EVIDENCE_JSON. Diagnostics, not budgets.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys

PHASES = ("host-initialization", "prepare", "format")
FATAL_MESSAGE = "ICU4C allocation failed"
POINT_TIMEOUT_SECONDS = 120


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, timeout=POINT_TIMEOUT_SECONDS)


def metrics(output: str) -> dict:
    found = {}
    for line in re.findall(r"L2 METRIC (\S+) (.*)$", output, re.MULTILINE):
        found[line[0]] = dict(re.findall(r"(\S+?)=(\S+)", line[1]))
    return found


def read_reports(receiver: socket.socket) -> str:
    receiver.setblocking(False)
    reports = []
    while True:
        try:
            reports.append(receiver.recv(65536).decode("utf-8", "replace"))
        except BlockingIOError:
            return "".join(reports)


def point(command: list[str], phase: int, fail_at: int, is_native: bool) -> dict:
    arguments = command + ["fail-point", str(phase), str(fail_at)]
    if not is_native:
        # Browser builds deliver emergency reports to console.error.
        result = run(arguments)
        combined = result.stdout + result.stderr
    else:
        # Native fatal reports never fall back to stderr; collect them from a socket.
        receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        with receiver, sender:
            environment = dict(os.environ, LUDUS_FORMAT_PROBE_DIAGNOSTIC_FD=str(sender.fileno()))
            result = subprocess.run(arguments, capture_output=True, text=True, timeout=POINT_TIMEOUT_SECONDS,
                                    env=environment, pass_fds=(sender.fileno(),))
            combined = result.stdout + result.stderr + read_reports(receiver)
    stopped = result.returncode != 0 and FATAL_MESSAGE in combined and "SURVIVED" not in combined
    return {"fail_at": fail_at, "exit": result.returncode, "fail_stop": stopped}


def main() -> int:
    emulator, probe, evidence_path = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    command = ([emulator] if emulator else []) + [probe]
    is_native = not emulator
    normal = run(command)
    output = normal.stdout + normal.stderr
    print(output)
    assert normal.returncode == 0, f"Probe exited {normal.returncode}"
    for name in ("host-initialization", "dynamic-format-measurements", "corpus-from-cooked-packages",
                 "host-cleanup-releases-every-allocation"):
        assert f"L2 PASS {name}" in output, f"Missing L2 PASS {name}"
    counted = run(command + ["count-points"])
    assert counted.returncode == 0, counted.stdout + counted.stderr
    points = {name: int(count) for name, count in
              re.findall(r"L2 POINTS phase=(\S+) allocations=(\d+)", counted.stdout + counted.stderr)}
    assert set(points) == set(PHASES), f"Missing allocation counts: {points}"
    sweeps = {}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for index, name in enumerate(PHASES):
            assert points[name] > 0, f"{name} made no ICU allocation"
            results = list(pool.map(lambda fail_at: point(command, index, fail_at, is_native),
                                    range(1, points[name] + 1)))
            survivors = [r for r in results if not r["fail_stop"]]
            sweeps[name] = {"points": len(results), "fail_stop": len(results) - len(survivors),
                            "violations": survivors}
            print(f"L2 SWEEP {name} " + json.dumps({k: v for k, v in sweeps[name].items() if k != "violations"}))
    evidence = {"metrics": metrics(output), "allocation_points": points, "sweeps": sweeps}
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    violations = {name: sweep["violations"] for name, sweep in sweeps.items() if sweep["violations"]}
    assert not violations, f"Fail-stop violated: {violations}"
    return 0


if __name__ == "__main__":
    sys.exit(main())
