#!/usr/bin/env python3
"""Regression checks that layout drift fails rather than silently changing uploads."""
import copy
import json
from pathlib import Path
import runpy
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path(sys.argv.pop(1))
VERIFY = runpy.run_path(str(ROOT / "scripts/shader-probe"))


class ContractTests(unittest.TestCase):
    def test_actual_outputs(self):
        for target in ("vertex", "fragment", "wgsl"):
            VERIFY["check_reflection"](OUTPUT / f"{target}.reflection.json")
        VERIFY["check_wgsl"]((OUTPUT / "probe.wgsl").read_text())

    def test_reflection_drift(self):
        original = json.loads((OUTPUT / "wgsl.reflection.json").read_text())
        mutations = []
        offsets = copy.deepcopy(original)
        offsets["parameters"][0]["type"]["elementType"]["fields"][2]["binding"]["offset"] = 12
        mutations.append(offsets)
        size = copy.deepcopy(original)
        size["parameters"][0]["type"]["elementVarLayout"]["binding"]["size"] = 44
        mutations.append(size)
        binding = copy.deepcopy(original)
        binding["parameters"][0]["binding"]["index"] = 1
        mutations.append(binding)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reflection.json"
            for mutation in mutations:
                path.write_text(json.dumps(mutation))
                with self.assertRaises(RuntimeError):
                    VERIFY["check_reflection"](path)

    def test_emitted_wgsl_drift(self):
        original = (OUTPUT / "probe.wgsl").read_text()
        for before, after in (("@align(8) ElapsedTime", "@align(16) ElapsedTime"),
                              ("@binding(0)", "@binding(1)"),
                              ("fn VertexMain", "fn renamedMain")):
            self.assertIn(before, original)
            with self.assertRaises(RuntimeError):
                VERIFY["check_wgsl"](original.replace(before, after))


if __name__ == "__main__":
    unittest.main()
