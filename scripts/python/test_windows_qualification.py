"""Failure controls for Windows D7 runtime evidence on any host."""

import importlib.util
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
