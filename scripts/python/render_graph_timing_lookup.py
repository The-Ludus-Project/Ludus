"""Resolve captured GPU scope identities against exact cooked render-graph plans."""

import argparse
import csv
import json
from pathlib import Path
import re

SHA256 = re.compile(r"[a-f0-9]{64}")


def read_plan(path):
    document = json.loads(path.read_text(encoding="utf-8"))
    revision = document["runtime_sha256"]
    if not SHA256.fullmatch(revision):
        raise ValueError(f"{path}: invalid runtime SHA-256")
    entries = document.get("gpu_scopes")
    if entries is None:
        return revision, None
    scopes = {}
    for entry in entries:
        identity = entry["scope_identity"]
        operations = entry["operations"]
        if (type(identity) is not int or identity < 1 or identity in scopes
                or not isinstance(operations, list) or not operations
                or not all(isinstance(operation, str) and "/" in operation for operation in operations)
                or len(set(operations)) != len(operations)):
            raise ValueError(f"{path}: invalid or duplicate GPU scope mapping")
        scopes[identity] = operations
    return revision, scopes


def read_editor_trace(path):
    trace = json.loads(path.read_text(encoding="utf-8"))
    frames = {}
    for row in trace.get("ludusGpuGraphFrames", []):
        key = row["deviceSession"], row["frameId"]
        revision = row["runtime_sha256"]
        if not SHA256.fullmatch(revision) or (key in frames and frames[key] != revision):
            raise ValueError(f"{path}: invalid or conflicting cooked plan SHA-256 for frame {key}")
        frames[key] = revision
    samples = []
    previous = {}
    for event in trace["traceEvents"]:
        if event.get("name") == "GPU sample identity" and event.get("pid") == 2 and event.get("ph") == "i":
            identity = event["args"]
            key = identity["deviceSession"], identity["frameId"]
            frames.setdefault(key, None)
            scope = identity["scopeIdentity"]
            if scope:
                # Perfetto export pairs each identity with the immediately preceding
                # counter. Submission timestamps alone cannot distinguish multiple passes.
                is_paired = (previous.get("ph") == "C" and previous.get("pid") == event["pid"]
                             and previous.get("tid") == event["tid"] and previous.get("ts") == event["ts"]
                             and previous.get("name", "").startswith(f"GPU/Pass/{identity['passName']} ["
                                                                      f"{scope}:{identity['scopePassIndex']}:"))
                duration = previous["args"]["value"] if is_paired else None
                samples.append((key, scope, identity["scopePassIndex"], identity["logicalPassCount"],
                                identity["passName"], duration))
        previous = event
    return frames, samples, None


def read_sdk_timing(path):
    revision = None
    frame_ids = set()
    samples = []
    preparation_nanoseconds = None
    with path.open(newline="", encoding="utf-8") as capture:
        for row in csv.reader(capture):
            if not row:
                continue
            if row[0] == "plan_sha256":
                if revision is not None or not SHA256.fullmatch(row[1]):
                    raise ValueError(f"{path}: expected one valid cooked plan SHA-256")
                revision = row[1]
            elif row[0] == "frame":
                frame_ids.add(int(row[1]))
            elif row[0] == "scope":
                _, frame, scope, index, count, name, duration = row
                key = (None, int(frame))
                frame_ids.add(key[1])
                samples.append((key, int(scope), int(index), int(count), name,
                                int(duration) if duration else None))
            elif row[0] == "prepare_ns":
                preparation_nanoseconds = int(row[1])
    return {(None, frame): revision for frame in sorted(frame_ids)}, samples, preparation_nanoseconds


def build_report(capture, frames, samples, preparation_nanoseconds, plans, first, last, operation):
    frame_rows = {
        key: {"device_session": key[0], "frame": key[1], "runtime_sha256": revision,
              "status": "plan_unavailable" if revision not in plans else "timing_unavailable"}
        for key, revision in frames.items() if first <= key[1] <= last
    }
    scopes = []
    seen = set()
    for key, scope, index, count, name, duration in samples:
        if key not in frame_rows:
            continue
        sample_key = (key, scope, index)
        if sample_key in seen:
            raise ValueError(f"Duplicate physical scope sample {sample_key}")
        seen.add(sample_key)
        frame = frame_rows[key]
        plan = plans.get(frame["runtime_sha256"])
        declared_members = plan.get(scope, []) if plan is not None else []
        if operation is not None and operation not in declared_members:
            continue
        members = declared_members if len(declared_members) == count and declared_members[:1] == [name] else []
        if plan is None:
            status = "plan_unavailable"
        elif not members:
            status = "scope_unmapped"
        elif duration is None:
            status = "timing_unavailable"
        else:
            status = "group_only" if count > 1 else "measured"
        if status in ("measured", "group_only"):
            frame["status"] = "matched"
        scopes.append({"device_session": key[0], "frame": key[1], "runtime_sha256": frame["runtime_sha256"],
                       "scope_identity": scope, "scope_pass_index": index, "scope_name": name,
                       "logical_pass_count": count, "operations": members,
                       "gpu_duration_nanoseconds": duration, "status": status})

    if operation is not None:
        for frame in frame_rows.values():
            plan = plans.get(frame["runtime_sha256"])
            if plan is not None and not any(operation in members for members in plan.values()):
                frame["status"] = "operation_not_in_plan"

    return {"capture": str(capture), "first_frame": first, "last_frame": last, "operation": operation,
            "gpu_duration_unit": "nanoseconds", "cpu_preparation_nanoseconds": preparation_nanoseconds,
            "frames": list(frame_rows.values()), "scopes": scopes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument("--editor-trace", type=Path, help="Raw Editor Perfetto JSON export")
    sources.add_argument("--sdk-timing", type=Path, help="Installed SDK skylight timing CSV")
    parser.add_argument("--plan", action="append", default=[], type=Path, help="Saved .lurgrt.resources.json sidecar")
    parser.add_argument("--first-frame", required=True, type=int)
    parser.add_argument("--last-frame", required=True, type=int)
    parser.add_argument("--operation", help="Qualified authored operation ID for the reverse lookup")
    options = parser.parse_args()
    if not 0 <= options.first_frame <= options.last_frame:
        parser.error("expected a nonnegative inclusive frame range")
    try:
        plans = {}
        for path in options.plan:
            revision, scopes = read_plan(path)
            if scopes is None:
                continue
            if revision in plans and scopes != plans[revision]:
                raise ValueError(f"{path}: conflicting mappings for runtime {revision}")
            plans[revision] = scopes
        capture = options.editor_trace or options.sdk_timing
        reader = read_editor_trace if options.editor_trace else read_sdk_timing
        frames, samples, preparation = reader(capture)
        report = build_report(capture, frames, samples, preparation, plans,
                              options.first_frame, options.last_frame, options.operation)
    except (OSError, KeyError, IndexError, TypeError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
