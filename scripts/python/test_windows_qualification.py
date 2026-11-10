"""Failure controls for Windows D7 runtime evidence on any host."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[2] / "tests/build_contract/windows_qualification.py"
SPEC = importlib.util.spec_from_file_location("windows_qualification", RUNNER)
qualification = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qualification)


class WindowsQualificationTests(unittest.TestCase):
    def test_required_inventory_rejects_missing_or_duplicate_tests(self):
        qualification.require_inventory(["one", "two"], ["two", "one"])
        for actual in (["one"], ["one", "one", "two"]):
            with self.subTest(actual=actual), self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
                qualification.require_inventory(["one", "two"], actual)

    def test_required_ctest_cases_must_run_and_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "ctest.xml"
            for status, detail in (("notrun", "<skipped/>"), ("fail", "<failure/>"), ("run", "<error/>")):
                with self.subTest(status=status):
                    report.write_text(f'<testsuite><testcase name="test" status="{status}">{detail}'
                                      '</testcase></testsuite>', encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "did not pass"):
                        qualification.check_results(report, ["test"])
            report.write_text('<testsuite><testcase name="test" status="run"><system-out>GPU'
                              '</system-out></testcase></testsuite>', encoding="utf-8")
            self.assertEqual(qualification.check_results(report, ["test"]), {"test": "GPU"})

    def test_performance_limits_reject_missing_and_nonfinite_measurements(self):
        report = {"scenarios": [{"id": "world", "metrics": {"cpu_ns": {"max": 10}}}]}
        with tempfile.TemporaryDirectory() as directory:
            limits = Path(directory) / "limits.json"
            limits.write_text(json.dumps({"schema": 1, "scenarios": {"world": {"cpu_ns": 10}}}))
            qualification.check_performance_limits(report, limits)
            for thresholds in ({}, {"missing": 10}, {"cpu_ns": 9}, {"cpu_ns": float("nan")}):
                limits.write_text(json.dumps({"schema": 1, "scenarios": {"world": thresholds}}))
                with self.subTest(thresholds=thresholds), self.assertRaises(RuntimeError):
                    qualification.check_performance_limits(report, limits)

    def test_gpu_budgets_require_real_memory_accounting(self):
        report = {"scenarios": [{"id": "rhi-ordered-graph-gpu-timing", "metrics": {
            "R7/layers=1/merge=1/peak_api_bytes_maximum": {"max": 100},
            "R7/layers=1/merge=1/api_bytes_available_minimum": {"min": 0, "max": 0},
        }}]}
        with tempfile.TemporaryDirectory() as directory:
            limits = Path(directory) / "limits.json"
            limits.write_text(json.dumps({"schema": 1, "scenarios": {
                "rhi-ordered-graph-gpu-timing": {"R7/layers=1/merge=1/peak_api_bytes_maximum": 100}}}))
            with self.assertRaisesRegex(RuntimeError, "available API allocation"):
                qualification.check_performance_limits(report, limits)
            report["scenarios"][0]["metrics"]["R7/layers=1/merge=1/api_bytes_available_minimum"]["min"] = 1
            qualification.check_performance_limits(report, limits)

    def test_physical_evidence_requires_the_reported_adapter_and_driver(self):
        host = {"adapters": [{"Name": "A GPU", "DriverVersion": "1.2.3", "PNPDeviceID": r"PCI\VEN_AAAA"}]}
        output = "Direct3D 12 hardware adapter 'A GPU'"
        self.assertEqual(qualification.check_adapter(host, output, "A GPU", "1.2.3"), host["adapters"][0])
        with self.assertRaisesRegex(RuntimeError, "one PCI display adapter"):
            qualification.check_adapter(host, output, "A GPU", "0.0")
        for different_adapter in ("Direct3D 12 software adapter 'A GPU'", "Direct3D 12 hardware adapter 'Other'"):
            with self.subTest(adapter=different_adapter), self.assertRaisesRegex(RuntimeError, "Packed World selected"):
                qualification.check_adapter(host, different_adapter, "A GPU", "1.2.3")


if __name__ == "__main__":
    unittest.main()
