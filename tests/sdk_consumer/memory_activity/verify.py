"""Run the installed SDK workload and check the offline report against authored expectations."""

import json
from pathlib import Path
import subprocess
import sys


REPORT = Path(__file__).resolve().parents[3] / "tools" / "memory-report" / "report.py"
DOMAIN_NAME = "Acceptance.Burst"
BURST_BYTES = 192
BURST_COUNT = 3


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def analyze(artifact):
    result = subprocess.run(
        [sys.executable, "-O", REPORT, artifact, "--json"], check=True, capture_output=True, text=True
    )
    return json.loads(result.stdout)


def check_traffic(report, byte_count, operation_count):
    (domain,) = report["domains"]
    require(domain["name"] == DOMAIN_NAME, "wrong selected domain")
    require(
        domain["endpoints"] == {"beginLiveBytes": 0, "endLiveBytes": 0},
        "the burst should be invisible in live-byte boundary samples",
    )
    require(
        domain["traffic"]
        == {
            "allocatedBytes": byte_count,
            "freedBytes": byte_count,
            "allocationCount": operation_count,
            "freeCount": operation_count,
            "failureCount": 0,
            "rejectedCount": 0,
        },
        "unreconciled traffic",
    )
    require(report["baseline"] == "unproven", "Activity unexpectedly claims a proven baseline")


def check_rejected(artifact):
    result = subprocess.run([sys.executable, "-O", REPORT, artifact], capture_output=True, text=True)
    require(result.returncode == 1 and "Invalid memory artifact" in result.stderr, "corrupt artifact was accepted")


def main():
    producer, directory = sys.argv[1:]
    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    full, journal, table, unknown = (
        output / name for name in ("full.json", "journal-loss.json", "table-loss.json", "unknown-baseline.json")
    )
    subprocess.run([producer, full, journal, table, unknown], check=True)

    report = analyze(full)
    check_traffic(report, BURST_BYTES, BURST_COUNT)
    (site,) = report["sites"]
    interval = report["interval"]
    require(interval["beginNs"] < interval["endNs"] and interval["recordedOperations"] == 6, "wrong interval")
    require(report["coverage"] == "full-activity" and report["loss"] == "none", "unexpected full-capture loss")
    require(
        site["domain"] == DOMAIN_NAME
        and site["site"] == {
            "path": "tests/sdk_consumer/memory_activity/main.cpp",
            "function": "RunBurst",
            "module": "installed-sdk-memory-activity",
            "line": 71,
        }
        and (site["firstOperation"], site["lastOperation"]) == (1, 6)
        and (site["allocatedBytes"], site["freedBytes"]) == (BURST_BYTES, BURST_BYTES)
        and (site["allocationCount"], site["freeCount"]) == (BURST_COUNT, BURST_COUNT),
        "the burst did not reconcile to its authored site and operation interval",
    )
    recorded = json.loads(full.read_text(encoding="utf-8"))
    allocation, free = recorded["events"][:2]
    require(allocation["thread"] != free["thread"] and allocation["lifetime"] == free["lifetime"], "wrong remote free")
    require(report["domains"][0]["journalOrderPeakBytes"] == 64, "wrong transient peak")

    reduced = analyze(journal)
    check_traffic(reduced, BURST_BYTES, BURST_COUNT)
    require(reduced["capture"] > report["capture"], "restart reused an ID")
    require(
        reduced["coverage"] == "reduced-activity" and reduced["loss"] == "journal-full"
        and reduced["firstLostOperation"] == 2 and reduced["interval"]["recordedOperations"] == 1
        and reduced["sites"][0]["allocationCount"] == 1 and reduced["sites"][0]["freeCount"] == 0,
        "journal exhaustion did not mark the observed prefix",
    )
    exhausted = analyze(table)
    check_traffic(exhausted, 128, 2)
    require(exhausted["loss"] == "table-full" and exhausted["firstLostOperation"] == 2, "table limit was not enforced")
    baseline = analyze(unknown)
    require(
        baseline["baseline"] == "unproven" and baseline["sites"][0]["site"] is None
        and baseline["sites"][0]["freedBytes"] == 64 and baseline["coverage"] == "full-activity",
        "the capture invented an allocation origin for a pre-capture block",
    )

    corrupt = output / "corrupt.json"
    corrupt.write_bytes(full.read_bytes()[:-20])
    check_rejected(corrupt)
    recorded["events"][1]["lifetime"] = "2"
    corrupt.write_text(json.dumps(recorded), encoding="utf-8")
    check_rejected(corrupt)
    recorded["events"][1]["lifetime"] = "1"
    recorded["domains"][0]["end"]["allocationCount"] = "4"
    recorded["domains"][0]["end"]["freeCount"] = "4"
    corrupt.write_text(json.dumps(recorded), encoding="utf-8")
    check_rejected(corrupt)

    for artifact in (full, journal, table, unknown):
        subprocess.run([sys.executable, REPORT, artifact], check=True)


if __name__ == "__main__":
    main()
