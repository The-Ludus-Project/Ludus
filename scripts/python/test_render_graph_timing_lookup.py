import json
from pathlib import Path
import tempfile
import unittest

import render_graph_timing_lookup as lookup


REVISION = "1" * 64
REBUILT = "2" * 64
LEFT = "Main/Left/draw"
RIGHT = "Main/Right/draw"
LEADING = "Main/Lighting/first"
FOLLOWING = "Main/Lighting/second"
PLAN = {1: [LEFT], 2: [RIGHT], 3: [LEADING, FOLLOWING]}


class RenderGraphTimingLookupTests(unittest.TestCase):
    def test_frame_and_scope_identity_survive_editor_and_sdk_export(self):
        frames = [
            {"deviceSession": 7, "frameId": frame, "runtime_sha256": revision}
            for frame, revision in [(4, REVISION), (5, REBUILT), (6, REVISION)]
        ]
        events = []
        csv_rows = [f"plan_sha256,{REVISION}", "frame,4,40,12,0", "frame,6,41,12,0", "prepare_ns,90"]
        for scope, name, count, duration in [(1, LEFT, 1, 10), (2, RIGHT, 1, 11), (3, LEADING, 2, 12)]:
            identity = {"frameId": 4, "deviceSession": 7, "scopeIdentity": scope, "scopePassIndex": 0,
                        "passName": name, "logicalPassCount": count}
            common = {"ts": 15, "pid": 2, "tid": 1}
            events.append({**common, "ph": "C", "name": f"GPU/Pass/{name} [{scope}:0:Raster]/DurationNanoseconds",
                           "args": {"value": duration}})
            events.append({**common, "ph": "i", "name": "GPU sample identity", "args": identity})
            csv_rows.append(f"scope,4,{scope},0,{count},{name},{duration}")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / "trace.json"
            trace.write_text(json.dumps({"traceEvents": events, "ludusGpuGraphFrames": frames}), encoding="utf-8")
            timing = root / "timing.csv"
            timing.write_text("\n".join(csv_rows), encoding="utf-8")
            for capture, reader, preparation in [(trace, lookup.read_editor_trace, None),
                                                   (timing, lookup.read_sdk_timing, 90)]:
                with self.subTest(capture=capture):
                    data = reader(capture)
                    plans = {REVISION: PLAN}
                    report = lookup.build_report(capture, *data, plans, 4, 6, None)
                    self.assertEqual([row["operations"] for row in report["scopes"]],
                                     [[LEFT], [RIGHT], [LEADING, FOLLOWING]])
                    self.assertEqual(report["scopes"][-1]["status"], "group_only")
                    self.assertEqual(report["cpu_preparation_nanoseconds"], preparation)
                    self.assertEqual(report["frames"][-1]["status"], "timing_unavailable")
                    for operation, scope in [(LEFT, 1), (RIGHT, 2), (LEADING, 3), (FOLLOWING, 3)]:
                        reverse = lookup.build_report(capture, *data, plans, 4, 4, operation)
                        row, = reverse["scopes"]
                        self.assertEqual(row["scope_identity"], scope)
                        self.assertIn(operation, row["operations"])
                    culled = lookup.build_report(capture, *data, plans, 4, 4, "Main/unused")
                    self.assertEqual(culled["frames"][0]["status"], "operation_not_in_plan")
                    self.assertEqual(culled["scopes"], [])
            rebuilt = lookup.build_report(trace, *lookup.read_editor_trace(trace), {REVISION: PLAN}, 5, 5, LEFT)
            self.assertEqual(rebuilt["frames"][0]["status"], "plan_unavailable")

    def test_same_label_in_new_plan_does_not_reassign_old_capture(self):
        frames = {(9, 20): REVISION}
        samples = [((9, 20), 1, 0, 1, LEFT, 13)]
        report = lookup.build_report("recording", frames, samples, None, {REBUILT: PLAN}, 20, 20, None)
        row, = report["scopes"]
        self.assertEqual(row["status"], "plan_unavailable")
        self.assertEqual(row["operations"], [])
        self.assertEqual(row["runtime_sha256"], REVISION)
        self.assertEqual(row["gpu_duration_nanoseconds"], 13)

    def test_reject_group_count_mismatch_and_preserve_unavailable_sample(self):
        frames = {(9, 20): REVISION}
        samples = [((9, 20), 3, 0, 1, LEADING, 13), ((9, 20), 2, 0, 1, RIGHT, None)]
        report = lookup.build_report("recording", frames, samples, None, {REVISION: PLAN}, 20, 20, None)
        mismatch, unavailable = report["scopes"]
        self.assertEqual(mismatch["status"], "scope_unmapped")
        self.assertEqual(mismatch["operations"], [])
        self.assertEqual(unavailable["status"], "timing_unavailable")
        self.assertEqual(unavailable["operations"], [RIGHT])
        self.assertIsNone(unavailable["gpu_duration_nanoseconds"])


if __name__ == "__main__":
    unittest.main()
