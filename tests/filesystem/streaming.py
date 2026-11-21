"""Deterministic FS-R2 fixtures and repeated real-SDK streaming measurements.

Generate synthetic fixtures or capture a packed World startup for replay.
The native harness compares all delivered bytes with independent references.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import re
import shutil
import subprocess
import statistics
import sys
import time
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/python"))
from pack_builder import StableSource, build_stream, portable_path, source_paths

SEED = 16302
STARTUP_REPORT = re.compile(
    r"FS-R2 startup: open_start_ns=(\d+) read_start_ns=(\d+) read_end_ns=(\d+) "
    r"validation_end_ns=(\d+) world_ready_ns=(\d+) renderer_ready_ns=(\d+) present_ns=(\d+) "
    r"load_request_ns=(\d+) quad_ready_ns=(\d+) offset=(\d+) bytes=(\d+) entry=([^\r\n]+)"
)


def word_hash(value):
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    return value ^ (value >> 16)


def make_bytes(file_index, size):
    data = bytearray(size)
    for offset in range(0, size, 4):
        word = word_hash((offset // 4) ^ ((file_index * 0x9E3779B9) & 0xFFFFFFFF))
        random_bytes = word.to_bytes(4, "little")
        for byte in range(4):
            position = offset + byte
            if position >= size:
                break
            compressible = file_index % 3 == 0 or (file_index % 3 == 2 and position % 4096 < 2048)
            data[position] = (position + file_index) % 256 if compressible else random_bytes[byte]
    return data


def make_trace(family, count=256):
    if family not in ("startup", "transition", "background") or not 16 <= count <= 4096:
        raise ValueError("unknown family or count outside 16..4096")
    generator = random.Random(SEED)
    requests = []
    for index in range(count):
        is_large = index % 8 == 0
        file_index = 64 + generator.randrange(8) if is_large else generator.randrange(64)
        length = (256 * 1024 if family == "background" else 1024 * 1024) if is_large else 4096
        offset = generator.randrange((2 * 1024 * 1024 - length) // 4096 + 1) * 4096 if is_large else 0
        release = 0 if family == "startup" else (index // 32) * 4_000_000 if family == "transition" else index * 1_000_000
        priority = 0 if is_large else 2
        should_cancel = int(family == "transition" and index % 11 == 0)
        requests.append((release, file_index, offset, length, priority, should_cancel))
    return requests


def generate(directory, count):
    directory.mkdir(parents=True, exist_ok=True)
    corpus = directory / "loose"
    manifest = {"schema": 1, "seed": SEED, "kind": "synthetic-mixed-compressibility", "files": {}, "traces": {}}
    for file_index in range(72):
        kind = "small" if file_index < 64 else "large"
        index = file_index if file_index < 64 else file_index - 64
        path = corpus / kind / f"{index:02d}"
        path.parent.mkdir(parents=True, exist_ok=True)
        data = make_bytes(file_index, 4096 if file_index < 64 else 2 * 1024 * 1024)
        path.write_bytes(data)
        manifest["files"][path.relative_to(corpus).as_posix()] = hashlib.sha256(data).hexdigest()
    for layout, compress in (("raw.pack", False), ("lz4.pack", True)):
        with (directory / layout).open("wb") as destination:
            build_stream(source_paths(corpus), lambda path: StableSource(corpus / path), destination, compress=compress)
        manifest[layout] = {"bytes": (directory / layout).stat().st_size, "sha256": file_hash(directory / layout)}
    for family in ("startup", "transition", "background"):
        path = directory / f"{family}.trace"
        rows = make_trace(family, count)
        path.write_text(f"fs-r2 1 {len(rows)}\n" + "".join(" ".join(map(str, row)) + "\n" for row in rows))
        manifest["traces"][family] = file_hash(path)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture_startup(args):
    portable_path(args.entry)
    binary = args.world_demo.resolve(strict=True)
    pack = args.pack.resolve(strict=True)
    source = args.loose.resolve(strict=True) / args.entry
    binary_hash, pack_hash, source_hash = map(file_hash, (binary, pack, source))
    # Use a new corpus so a failed capture cannot overwrite prior evidence.
    args.directory.mkdir(parents=True)
    replay_pack = args.directory / args.layout
    shutil.copyfile(pack, replay_pack)
    reference = args.directory / "loose" / args.entry
    reference.parent.mkdir(parents=True)
    shutil.copyfile(source, reference)
    if file_hash(replay_pack) != pack_hash or file_hash(reference) != source_hash:
        raise ValueError("pack or independent reference changed during capture setup")

    command = [str(binary), "--startup-report", "--frames", "1", "--level-pack", str(pack), args.entry]
    reference_size = reference.stat().st_size
    if reference_size == 0:
        raise ValueError("World startup requires a nonempty reference asset")
    timing_names = ("open_start_ns", "read_start_ns", "read_end_ns", "validation_end_ns",
                    "world_ready_ns", "renderer_ready_ns", "present_ns", "load_request_ns", "quad_ready_ns")
    with (args.directory / "startup-runs.jsonl").open("w", buffering=1) as runs:
        for repetition in range(args.repetitions):
            completed = subprocess.run(command, text=True, capture_output=True, timeout=45)
            record = dict(repetition=repetition, status=completed.returncode, command=command,
                          stdout=completed.stdout, stderr=completed.stderr)
            if hasattr(os, "getloadavg"):
                record["host_load_average_after"] = os.getloadavg()
            matches = STARTUP_REPORT.findall(completed.stdout + "\n" + completed.stderr)
            if completed.returncode != 0 or len(matches) != 1:
                runs.write(json.dumps(record) + "\n")
                raise RuntimeError("native World startup failed or emitted no unique report; see startup-runs.jsonl")
            report = matches[0]
            timings = dict(zip(timing_names, map(int, report[:len(timing_names)]), strict=True))
            record["timings"] = timings
            offset, size = map(int, report[-3:-1])
            record["request"] = {"entry": report[-1], "offset": offset, "bytes": size}
            runs.write(json.dumps(record) + "\n")
            read_stages = [timings[name] for name in timing_names[:4]]
            ready_stages = [timings[name] for name in ("world_ready_ns", "renderer_ready_ns")]
            if (offset != 0 or size != reference_size or report[-1] != args.entry
                    or read_stages != sorted(read_stages)
                    or not all(read_stages[-1] <= ready <= timings["present_ns"] for ready in ready_stages)):
                raise ValueError("World report did not match the captured whole-entry route or startup milestone order")

    if (binary_hash, pack_hash, source_hash) != tuple(map(file_hash, (binary, pack, source))):
        raise ValueError("startup input or game build changed while capturing")
    trace = args.directory / "startup.trace"
    trace.write_text(f"fs-r2 2 1 1\n{args.entry}\n0 0 {offset} {size} 0 0\n", encoding="utf-8", newline="\n")
    manifest = {"schema": 1, "kind": "native-world-demo-startup",
                "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "game_binary_sha256": binary_hash, "files": {args.entry: source_hash},
                args.layout: {"bytes": replay_pack.stat().st_size, "sha256": pack_hash},
                "traces": {"startup": file_hash(trace)},
                "startup_runs_sha256": file_hash(args.directory / "startup-runs.jsonl")}
    (args.directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{args.repetitions} windowed startup runs and replay corpus saved to {args.directory}")


class NativeRead(NamedTuple):
    start: int
    end: int
    thread: int
    entry: str
    offset: int
    size: int


def import_macos_overlap(args):
    events = []
    counts = []
    for line in args.reads.read_text().splitlines():
        if line.startswith("read_count "):
            counts.append(int(line.split()[1]))
        if line.startswith("pread "):
            _, thread, start, end, offset, requested, received, entry = line.split(maxsplit=7)
            thread, start, end, offset, requested, received = map(
                int, (thread, start, end, offset, requested, received))
            portable_path(entry)
            if thread <= 0 or not 0 <= start <= end or offset < 0 or not 0 < received == requested:
                raise ValueError(f"invalid or incomplete native read: {line}")
            events.append(NativeRead(start, end, thread, entry, offset, received))
    if counts != [len(events)] or not events:
        raise ValueError("missing native events, duplicate capture, or capture overflow")
    events.sort()

    overlap = None
    for index, first in enumerate(events):
        for second in itertools.islice(events, index + 1, None):
            if second.start >= first.end:
                break
            if second.thread != first.thread and min(first.end, second.end) > second.start:
                overlap = (first, second)
                break
        if overlap is not None:
            break
    if overlap is None:
        raise ValueError("no overlapping native reads on different threads")

    first, second = overlap
    margin_ns = 100_000_000
    selected = [read for read in events if first.start - margin_ns <= read.start < max(
        first.end, second.end) + margin_ns]
    release_origin = selected[0].start
    entries = sorted({read.entry for read in selected})
    file_index = {entry: index for index, entry in enumerate(entries)}
    source = args.loose.resolve(strict=True)
    args.directory.mkdir(parents=True)
    hashes = {}
    for entry in entries:
        original = source / entry
        hashes[entry] = file_hash(original)
        for root in ("loose", "reference"):
            destination = args.directory / root / entry
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, destination)
            if file_hash(destination) != hashes[entry]:
                raise ValueError(f"native input changed while importing: {entry}")
    rows = [(r.start - release_origin, file_index[r.entry], r.offset, r.size, 0, 0) for r in selected]
    trace = args.directory / "transition.trace"
    trace.write_text(f"fs-r2 2 {len(rows)} {len(entries)}\n" + "".join(f"{entry}\n" for entry in entries) +
                     "".join(" ".join(map(str, row)) + "\n" for row in rows), encoding="utf-8", newline="\n")
    manifest = {"schema": 1, "kind": "native-audio-overlap", "loose_reference": True, "files": hashes,
                "game_binary_sha256": file_hash(args.native_demo), "read_log_sha256": file_hash(args.reads),
                "traces": {"transition": file_hash(trace)}, "captured_requests": len(events),
                "replayed_requests": len(rows), "replayed_bytes": sum(r.size for r in selected),
                "cross_thread_overlap_ns": min(first.end, second.end) - second.start,
                "overlapping_reads": [read._asdict() for read in overlap]}
    (args.directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Imported {len(rows)} of {len(events)} native reads around the first cross-thread overlap")


def environment():
    result = {"platform": platform.platform(), "machine": platform.machine(), "python": sys.version,
              "frame_impact": "NOT MEASURED: standalone byte-delivery harness",
              "content_decode_upload": "NOT MEASURED", "contention_kind": "integer-mix CPU load"}
    commands = [["git", "rev-parse", "HEAD"], ["uname", "-a"], ["df", "-T", "."] if sys.platform == "linux" else ["df", "."],
                ["lscpu"] if sys.platform == "linux" else ["sysctl", "-n", "machdep.cpu.brand_string"],
                ["mount"]]
    result["source_sha256"] = {str(path): file_hash(path) for path in (
        Path("modules/foundation/filesystem/tests/benchmark.cpp"),
        Path("modules/foundation/filesystem/tests/streaming_benchmark.cpp"),
        Path("modules/foundation/filesystem/src/async.cpp"), Path("modules/foundation/filesystem/src/pack.cpp"))}
    result["commands"] = []
    for command in commands:
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        result["commands"].append({"command": command, "status": completed.returncode,
                                    "stdout": completed.stdout, "stderr": completed.stderr})
    return result


def measure(args):
    directory = args.directory.resolve(strict=True)
    manifest_path = directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    reference = directory / "reference" if manifest.get("loose_reference") is True else None
    if reference is not None and args.layouts != ["loose"]:
        raise ValueError("captured loose files require --layouts loose")
    for layout in args.layouts:
        if layout != "loose" and file_hash(directory / layout) != manifest[layout]["sha256"]:
            raise ValueError(f"fixture hash mismatch: {layout}")
    # Validate every loose file before warming/cold-start setup. This intentionally
    # warms files unless the caller requests the explicit reset below.
    for name, expected in manifest["files"].items():
        if file_hash(directory / "loose" / name) != expected:
            raise ValueError(f"fixture hash mismatch: {name}")
        if reference is not None and file_hash(reference / name) != expected:
            raise ValueError(f"reference hash mismatch: {name}")
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = environment()
    if hasattr(os, "getloadavg"):
        metadata["host_load_average_before"] = os.getloadavg()
    metadata.update(manifest_sha256=file_hash(manifest_path), binary_sha256=file_hash(args.benchmark),
                    cache_method=args.cache, compiler=args.compiler_description, storage=args.storage_description,
                    repetitions=args.repetitions, order_seed=SEED, poll_us=args.poll_us,
                    sample_quantiles="nearest-rank, no interpolation; per run, never pooled")
    (args.output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    matrix = list(itertools.product(args.families, args.layouts, args.workers, args.contention,
                                    args.open_modes, args.tracing, range(args.repetitions)))
    random.Random(SEED).shuffle(matrix)
    with (args.output / "runs.jsonl").open("w", buffering=1) as destination:
        for family, layout, workers, contention, open_mode, tracing, repetition in matrix:
            trace = directory / f"{family}.trace"
            if file_hash(trace) != manifest["traces"][family]:
                raise ValueError(f"trace hash mismatch: {family}")
            if args.cache == "cold-start":
                if sys.platform != "linux":
                    raise ValueError("cold-start requires explicit Linux cache reset in a disposable runner")
                subprocess.run(["sync"], check=True)
                Path("/proc/sys/vm/drop_caches").write_text("3\n")
            command = [str(args.benchmark.resolve()), "--stream", str(directory / "loose" if layout == "loose" else directory),
                       layout, str(workers), str(trace), str(args.poll_us), str(contention), open_mode,
                       str(tracing), "v1", args.cache]
            if reference is not None:
                command.append(str(reference))
            started = time.time()
            completed = subprocess.run(command, text=True, capture_output=True, timeout=90)
            record = dict(family=family, layout=layout, open_mode=open_mode, cache=args.cache,
                          repetition=repetition, command=command, started_unix=started,
                          status=completed.returncode, stderr=completed.stderr, raw_stdout=completed.stdout)
            if completed.returncode == 0:
                record["result"] = json.loads(completed.stdout)
            destination.write(json.dumps(record) + "\n")
            if completed.returncode != 0:
                raise RuntimeError(f"benchmark failed ({completed.returncode}); evidence retained in {args.output}")
    groups = {}
    for line in (args.output / "runs.jsonl").read_text().splitlines():
        record = json.loads(line)
        result = record["result"]
        key = json.dumps(dict(family=record["family"], layout=record["layout"], workers=result["workers"],
                              open_mode=record["open_mode"], contention_threads=result["contention_threads"],
                              trace_capacity=result["trace_capacity"]), sort_keys=True)
        values = groups.setdefault(key, {"p99_completion_ns": [], "p99_service_ns": [], "cpu_ms": [], "mib_per_sec": []})
        for metric, value in (("p99_completion_ns", result["completion"]["p99_ns"]),
                              ("p99_service_ns", result["service"]["p99_ns"]),
                              ("cpu_ms", result["cpu_ms"]), ("mib_per_sec", result["mib_per_sec"])):
            values[metric].append(value)
    summary = [{"configuration": json.loads(key), "metrics": {
        metric: {"runs": len(values), "min": min(values), "median": statistics.median(values), "max": max(values)}
        for metric, values in metrics.items()}} for key, metrics in sorted(groups.items())]
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{len(matrix)} repeated streaming runs saved to {args.output / 'runs.jsonl'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    build = subparsers.add_parser("generate")
    build.add_argument("directory", type=Path)
    build.add_argument("--count", type=int, default=256)
    capture = subparsers.add_parser("capture-world-startup")
    capture.add_argument("directory", type=Path)
    capture.add_argument("--world-demo", type=Path, required=True)
    capture.add_argument("--pack", type=Path, required=True)
    capture.add_argument("--loose", type=Path, required=True, help="original directory used to build the pack")
    capture.add_argument("--entry", required=True)
    capture.add_argument("--layout", choices=("raw.pack", "lz4.pack"), default="lz4.pack")
    capture.add_argument("--repetitions", type=int, choices=range(1, 31), default=5)
    native = subparsers.add_parser("import-macos-overlap")
    native.add_argument("directory", type=Path)
    native.add_argument("--reads", type=Path, required=True, help="stderr from one successful native launch")
    native.add_argument("--loose", type=Path, required=True, help="the immutable source used during capture")
    native.add_argument("--native-demo", type=Path, required=True)
    run = subparsers.add_parser("measure")
    run.add_argument("directory", type=Path)
    run.add_argument("--benchmark", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--repetitions", type=int, default=3)
    run.add_argument("--families", nargs="+", choices=("startup", "transition", "background"), default=["startup", "transition", "background"])
    run.add_argument("--layouts", nargs="+", choices=("loose", "raw.pack", "lz4.pack"), default=["loose", "raw.pack", "lz4.pack"])
    run.add_argument("--workers", nargs="+", type=int, choices=(0, 1, 4), default=[0, 1, 4])
    run.add_argument("--contention", nargs="+", type=int, choices=range(9), default=[0, 2])
    run.add_argument("--open-modes", nargs="+", choices=("shared", "independent"), default=["shared", "independent"])
    run.add_argument("--tracing", nargs="+", type=int, choices=(0, 256, 4096), default=[0, 4096])
    run.add_argument("--poll-us", type=int, default=1000)
    run.add_argument("--cache", choices=("uncontrolled", "warm", "cold-start"), default="warm")
    run.add_argument("--disposable-runner", action="store_true")
    run.add_argument("--compiler-description", required=True)
    run.add_argument("--storage-description", required=True)
    args = parser.parse_args()
    if args.operation == "generate":
        generate(args.directory, args.count)
    elif args.operation == "capture-world-startup":
        capture_startup(args)
    elif args.operation == "import-macos-overlap":
        import_macos_overlap(args)
    else:
        if not 1 <= args.repetitions <= 30 or not 1 <= args.poll_us <= 1_000_000:
            parser.error("repetitions must be 1..30 and poll-us 1..1000000")
        if args.cache == "cold-start" and not args.disposable_runner:
            parser.error("global Linux cache reset requires --disposable-runner")
        measure(args)


if __name__ == "__main__":
    main()
