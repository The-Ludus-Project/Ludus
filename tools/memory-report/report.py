#!/usr/bin/env python3
"""Validate a sealed Ludus Activity artifact and report requested-byte traffic."""

# Thanks to RudjiGames and MTuner contributors for the churn and transient-peak
# reporting examples. Ludus reconciles its bounded journal against quiescent
# domain counters. See docs/architecture/memory-profiling.md#artifacts-and-analysis.

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import sys


# Offline input guard, independent of the runtime's recorder storage budget.
MAX_ARTIFACT_BYTES = 32 * 1024 * 1024
UINT64_MAX = (1 << 64) - 1
TRAFFIC_FIELDS = ("allocatedBytes", "freedBytes", "allocationCount", "freeCount", "failureCount")
COUNTER_FIELDS = (*TRAFFIC_FIELDS, "rejectedCount", "liveBytes", "liveBlocks", "peakBytes", "validity")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", value), "invalid unsigned field")
    result = int(value)
    require(result <= UINT64_MAX, "unsigned field out of range")
    return result


def text(value):
    require(isinstance(value, str) and value and "\0" not in value, "invalid definition text")
    value.encode("utf-8")  # Reject lone escaped surrogate code points.
    return value


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        require(name not in result, f"duplicate field {name}")
        result[name] = value
    return result


def read_counters(encoded):
    counters = {name: number(encoded[name]) for name in COUNTER_FIELDS}
    require(counters["validity"] in (0, 1, 2), "unknown accounting validity")
    if counters["validity"] == 0:
        require(
            counters["allocatedBytes"] - counters["freedBytes"] == counters["liveBytes"]
            and counters["allocationCount"] - counters["freeCount"] == counters["liveBlocks"]
            and counters["peakBytes"] >= counters["liveBytes"],
            "invalid quiescent counters",
        )
    return counters


def read_domains(definitions):
    domains = {}
    names = set()
    for definition in definitions:
        identity = number(definition["id"])
        name = text(definition["name"])
        require(identity != 0 and identity not in domains and name not in names, "duplicate or invalid domain")
        first, last = (read_counters(definition[key]) for key in ("begin", "end"))
        require(first["validity"] == 0 or last["validity"] != 0, "invalid accounting validity transition")
        totals = None
        if first["validity"] == last["validity"] == 0:
            totals = {key: last[key] - first[key] for key in (*TRAFFIC_FIELDS, "rejectedCount")}
            require(min(totals.values()) >= 0 and last["peakBytes"] >= first["peakBytes"], "counters went backwards")
        domains[identity] = {
            "name": name,
            "begin": first,
            "end": last,
            "traffic": totals,
        }
        names.add(name)
    require(domains, "empty domain selection")
    return domains


def read_sites(definitions):
    sites = {0: None}
    locations = set()
    for identity, definition in enumerate(definitions, 1):
        require(number(definition["id"]) == identity, "invalid site ID order")
        line = number(definition["line"])
        location = (text(definition["path"]), text(definition["function"]), text(definition["module"]), line)
        require(0 < line <= (1 << 32) - 1 and location not in locations, "duplicate or invalid site definition")
        locations.add(location)
        sites[identity] = dict(zip(("path", "function", "module", "line"), location))
    return sites


def validate_events(events, domains, sites, begin, end, slots, loss):
    live = {}
    activity = {}
    domain_traffic = defaultdict(lambda: dict.fromkeys(TRAFFIC_FIELDS, 0))
    live_bytes = {identity: data["begin"]["liveBytes"] for identity, data in domains.items()}
    observed_peaks = dict(live_bytes)
    for sequence, event in enumerate(events, 1):
        require(number(event["sequence"]) == sequence, "invalid operation order")
        domain, site = (number(event[key]) for key in ("domain", "site"))
        require(domain in domains and site in sites, "undefined event domain or site")
        ticks = number(event["ticks"])
        require(begin <= ticks <= end, "event outside capture interval")
        number(event["thread"])
        address, size, alignment, lifetime = (
            number(event[key]) for key in ("address", "bytes", "alignment", "lifetime")
        )
        require(size and alignment and not (alignment & (alignment - 1)), "invalid event layout")
        kind = event["kind"]
        if kind == "alloc":
            require(lifetime == sequence and address and address not in live, "invalid allocation identity")
            live[address] = (lifetime, domain, site, size, alignment)
            require(len(live) <= slots // 2, "live table exceeded its load limit")
            byte_field, count_field = "allocatedBytes", "allocationCount"
            live_bytes[domain] += size
            observed_peaks[domain] = max(observed_peaks[domain], live_bytes[domain])
        elif kind == "free":
            require(address, "null free event")
            if lifetime == 0:
                require(address not in live and site == 0, "invalid baseline-unknown free")
            else:
                require(live.get(address) == (lifetime, domain, site, size, alignment), "free disagrees with origin")
                del live[address]
            byte_field, count_field = "freedBytes", "freeCount"
            live_bytes[domain] -= size
        elif kind == "failure":
            require(address == 0 and lifetime == 0, "failed request has a lifetime")
            byte_field, count_field = None, "failureCount"
        else:
            raise ValueError("unknown event kind")

        row = activity.setdefault(
            (domain, site),
            {
                **dict.fromkeys(TRAFFIC_FIELDS, 0),
                "firstOperation": sequence,
                "lastOperation": sequence,
                "firstNs": ticks,
                "lastNs": ticks,
            },
        )
        row["lastOperation"] = sequence
        row["firstNs"] = min(row["firstNs"], ticks)
        row["lastNs"] = max(row["lastNs"], ticks)
        row[count_field] += 1
        domain_traffic[domain][count_field] += 1
        if byte_field is not None:
            row[byte_field] += size
            domain_traffic[domain][byte_field] += size

    for identity, domain in domains.items():
        totals = domain["traffic"]
        if totals is not None:
            for key, observed in domain_traffic[identity].items():
                require(observed <= totals[key], f"{domain['name']}: journal exceeds {key} counter delta")
                if loss == "none":
                    require(observed == totals[key], f"{domain['name']}: journal disagrees with {key} counter delta")
        domain["counterHighWaterBytes"] = domain["end"]["peakBytes"] if domain["end"]["validity"] == 0 else None
        domain["endpoints"] = {
            "beginLiveBytes": domain["begin"]["liveBytes"] if domain["begin"]["validity"] == 0 else None,
            "endLiveBytes": domain["end"]["liveBytes"] if domain["end"]["validity"] == 0 else None,
        }
        # Hook sequence is authoritative for lifetimes. During concurrency it need
        # not match the order of the independent domain counter updates.
        domain["journalOrderPeakBytes"] = observed_peaks[identity] if loss == "none" and totals is not None else None
        del domain["begin"], domain["end"]
    return [
        {"domain": domains[domain]["name"], "site": sites[site], **row}
        for (domain, site), row in sorted(
            activity.items(), key=lambda item: (-item[1]["allocatedBytes"], item[0])
        )
    ]


def validate(artifact):
    require(
        artifact["schema"] == "ludus-memory-activity"
        and artifact["version"] == "1"
        and artifact["baseline"] == "unproven"
        and artifact["clockUnit"] == "ns",
        "unsupported Activity schema or baseline",
    )
    build_flavor = number(artifact["buildFlavorId"])
    require(build_flavor in (1, 2, 3, 4), "unknown build flavor")
    capture = number(artifact["capture"])
    begin, end = (number(artifact[key]) for key in ("beginTicks", "endTicks"))
    slots = number(artifact["liveSlots"])
    limit = number(artifact["eventLimit"])
    tooling = number(artifact["toolingBytes"])
    require(capture and begin <= end and slots >= 2 and limit and tooling, "invalid capture boundary or budget")
    seal = artifact["seal"]
    events = artifact["events"]
    count = number(seal["eventCount"])
    loss = seal["loss"]
    first_lost = number(seal["firstLostOperation"])
    require(isinstance(events, list) and len(events) == count <= limit, "invalid sealed record count")
    if loss == "journal-full":
        require(count == limit and first_lost == count + 1, "invalid journal-full seal")
    elif loss in ("table-full", "invalid-ownership"):
        require(count < limit and first_lost == count + 1, "invalid first lost operation")
    else:
        require(loss in ("none", "reentry") and first_lost == 0, "invalid loss seal")
    for exclusion in artifact["exclusions"]:
        text(exclusion["name"])
        text(exclusion["reason"])
    domains = read_domains(artifact["domains"])
    sites = read_sites(artifact["sites"])
    rows = validate_events(events, domains, sites, begin, end, slots, loss)
    return {
        "capture": capture,
        "buildFlavorId": build_flavor,
        "baseline": artifact["baseline"],
        "coverage": "full-activity" if loss == "none" else "reduced-activity",
        "loss": loss,
        "firstLostOperation": first_lost or None,
        "interval": {"beginNs": begin, "endNs": end, "recordedOperations": count},
        "toolingBytes": tooling,
        "domains": list(domains.values()),
        "sites": rows,
    }


def read_report(path):
    with path.open("rb") as source:
        data = source.read(MAX_ARTIFACT_BYTES + 1)
    require(len(data) <= MAX_ARTIFACT_BYTES, "artifact exceeds the offline reader limit")
    return validate(json.loads(data.decode("utf-8"), object_pairs_hook=unique_object))


def print_report(report):
    interval = report["interval"]
    print(
        f"Activity {report['capture']} | baseline {report['baseline']} | {report['coverage']} "
        f"| {interval['beginNs']}–{interval['endNs']} ns"
    )
    if report["loss"] != "none":
        first = report["firstLostOperation"] or "unknown"
        print(f"Loss: {report['loss']}, first lost operation: {first}. Site totals are observed events only.")
    for domain in report["domains"]:
        traffic = domain["traffic"]
        print(f"{domain['name']}: {traffic if traffic is not None else 'counter deltas unavailable'}")
    print("Domain | Site | Operations | Alloc bytes/count | Free bytes/count | Failures")
    for row in report["sites"]:
        site = row["site"]
        location = "unknown" if site is None else f"{site['path']}:{site['line']} {site['function']} [{site['module']}]"
        print(
            f"{row['domain']} | {location} | {row['firstOperation']}–{row['lastOperation']} "
            f"| {row['allocatedBytes']}/{row['allocationCount']} "
            f"| {row['freedBytes']}/{row['freeCount']} | {row['failureCount']}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--json", action="store_true", help="write a machine-readable report")
    arguments = parser.parse_args()
    try:
        report = read_report(arguments.artifact)
    except (OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError) as error:
        print(f"Invalid memory artifact: {error}", file=sys.stderr)
        return 1
    if arguments.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_report(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
