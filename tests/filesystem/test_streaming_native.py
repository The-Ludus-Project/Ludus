"""Real-SDK negative tests: interior corruption, malformed traces, cancellation and accounting."""
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from streaming import build_stream, make_bytes


class NativeStreamingTests(unittest.TestCase):
    def setUp(self):
        self.binary = os.environ.get("LUDUS_FS_R2_BENCHMARK")
        if not self.binary:
            self.skipTest("set LUDUS_FS_R2_BENCHMARK to the built native benchmark")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "small").mkdir()
        self.asset = self.root / "small/01"
        self.asset.write_bytes(make_bytes(1, 4096))
        self.trace = self.root / "fixture.trace"
        self.trace.write_text("fs-r2 1 16\n" + "0 1 0 4096 0 0\n" * 16)

    def run_benchmark(self, workers=1, tracing=0, open_mode="shared", layout="loose", reference=None):
        command = [self.binary, "--stream", str(self.root), layout, str(workers), str(self.trace),
                   "1000", "0", open_mode, str(tracing), "v1", "uncontrolled"]
        if reference is not None:
            command.append(str(reference))
        return subprocess.run(command, capture_output=True, text=True, timeout=20)

    def test_sync_and_async_full_validation_and_trace_accounting(self):
        for workers in (0, 1, 4):
            for open_mode in ("shared", "independent"):
                completed = self.run_benchmark(workers, 4, open_mode)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                result = json.loads(completed.stdout)
                self.assertEqual(result["bytes"], 16 * 4096)
                self.assertEqual(result["completion"]["count"], 16)
                self.assertEqual(result["cpp_hot_allocations"], 0)
                self.assertLessEqual(result["peak_requests"], 16)
                if workers:
                    self.assertEqual(result["trace_records"] + result["dropped_traces"], 16)
                    self.assertGreaterEqual(result["delivery"]["p50_ns"], 0)

    def test_captured_pack_uses_independent_reference_and_exact_ranges(self):
        entry = "World Rooms/first room.json"
        data = b"actual pack input bytes"
        reference = self.root / "loose" / entry
        reference.parent.mkdir(parents=True)
        reference.write_bytes(data)
        with (self.root / "lz4.pack").open("wb") as pack:
            build_stream([entry], lambda _: io.BytesIO(data), pack)
        self.trace.write_text(f"fs-r2 2 2 1\n{entry}\n0 0 2 8 0 0\n0 0 1 6 0 0\n")
        for workers in (0, 1, 4):
            result = self.run_benchmark(workers, tracing=4, layout="lz4.pack")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["bytes"], 14)

        reference.write_bytes(data[:4] + b"X" + data[5:])
        for workers in (0, 1):
            result = self.run_benchmark(workers, layout="lz4.pack")
            self.assertIn(result.returncode, (28, 32))

        self.trace.write_text(f"fs-r2 2 1 1\n{entry}\n0 0 {len(data)} 1 0 0\n")
        self.assertEqual(self.run_benchmark(layout="lz4.pack").returncode, 24)

    def test_captured_loose_requires_a_separate_reference(self):
        entry = "source.wav"
        data = b"native read source"
        asset = self.root / entry
        asset.write_bytes(data)
        reference = self.root / "reference"
        reference.mkdir()
        (reference / entry).write_bytes(data)
        self.trace.write_text(f"fs-r2 2 2 1\n{entry}\n0 0 1 7 0 0\n0 0 7 5 0 0\n")
        self.assertEqual(self.run_benchmark().returncode, 24)
        for workers in (0, 1, 4):
            completed = self.run_benchmark(workers, tracing=4, reference=reference)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(completed.stdout)["bytes"], 12)
        asset.write_bytes(data[:4] + b"X" + data[5:])
        for workers in (0, 1):
            self.assertIn(self.run_benchmark(workers, reference=reference).returncode, (28, 32))

    def test_interior_corruption_is_rejected(self):
        data = bytearray(self.asset.read_bytes())
        data[2047] ^= 1
        self.asset.write_bytes(data)
        for workers in (0, 1):
            self.assertIn(self.run_benchmark(workers).returncode, (28, 32))

    def test_cancelled_or_completed_races_are_accounted(self):
        self.trace.write_text("fs-r2 1 16\n" + "0 1 0 4096 0 1\n" * 16)
        for workers in (0, 1, 4):
            completed = self.run_benchmark(workers)
            self.assertEqual(completed.returncode, 0)
            result = json.loads(completed.stdout)
            self.assertEqual(result["cancel_intents"], 16)
            self.assertLessEqual(result["cancelled"], 16)
            self.assertEqual(result["bytes"], (16 - result["cancelled"]) * 4096)

    def test_malformed_or_out_of_bounds_trace_fails_before_reading(self):
        for trace in ("fs-r2 2 1\n0 1 0 4096 0 0\n", "fs-r2 1 1\n0 1 0 999999999999999999999999 0 0\n",
                      "fs-r2 1 1\n0 1 4090 4096 0 0\n", "fs-r2 1 2\n0 1 0 4096 0 0\n",
                      "fs-r2 1 1\n0 1 0 4096 0 0\njunk", "fs-r2 1 1\n0 72 0 4096 0 0\n"):
            self.trace.write_text(trace)
            self.assertEqual(self.run_benchmark().returncode, 22)


if __name__ == "__main__":
    unittest.main()
