"""Regressions for the CTest timeout headroom warnings."""

import contextlib
import io
import json
import re
import tempfile
import unittest
from pathlib import Path

import ctest_headroom
from ctest_headroom import measure, read_default_timeout, read_results, read_timeouts

ROOT = Path(__file__).resolve().parents[2]

# Shapes match ctest 3.29 --show-only=json-v1 and --output-junit output.
LISTING = {
    "kind": "ctestInfo",
    "version": {"major": 1, "minor": 0},
    "tests": [
        {"name": "near_tests", "properties": [{"name": "LABELS", "value": ["unit"]}, {"name": "TIMEOUT", "value": 180.0}]},
        {"name": "quick_tests", "properties": [{"name": "TIMEOUT", "value": 60.0}]},
        {"name": "timed_out_tests", "properties": [{"name": "TIMEOUT", "value": 30.0}]},
        {"name": "default_tests", "properties": [{"name": "WORKING_DIRECTORY", "value": "/build"}]},
        {"name": "skipped_tests", "properties": [{"name": "TIMEOUT", "value": 10.0}]},
    ],
}

JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux" tests="6" failures="1" disabled="1" skipped="0" time="300">
\t<testcase name="near_tests" classname="near_tests" time="160.2" status="run">
\t\t<properties><property name="cmake_labels" value="unit"/></properties>
\t</testcase>
\t<testcase name="quick_tests" classname="quick_tests" time="12.5" status="run"><properties/></testcase>
\t<testcase name="timed_out_tests" classname="timed_out_tests" time="30.04" status="fail">
\t\t<failure message="Timeout"/><properties/>
\t</testcase>
\t<testcase name="default_tests" classname="default_tests" time="1200" status="run"><properties/></testcase>
\t<testcase name="skipped_tests" classname="skipped_tests" time="0" status="disabled"><properties/></testcase>
\t<testcase name="unlisted_tests" classname="unlisted_tests" time="5" status="run"><properties/></testcase>
</testsuite>
"""

DART = "# TimeOut is the amount of time in seconds to wait for processes\nTimeOut: 1500\n"


class HeadroomParsingTests(unittest.TestCase):
    def test_reads_timeout_properties_only_where_declared(self):
        timeouts = read_timeouts(LISTING)
        self.assertEqual(timeouts["near_tests"], 180.0)
        self.assertNotIn("default_tests", timeouts)

    def test_reads_the_build_tree_default_timeout(self):
        self.assertEqual(read_default_timeout(DART), 1500.0)
        self.assertIsNone(read_default_timeout("TimeOut: \n"))
        self.assertIsNone(read_default_timeout("TimeOut: 0\n"))

    def test_reads_run_cases_and_marks_timeouts(self):
        results = read_results(JUNIT)
        self.assertEqual(results["near_tests"], (160.2, False))
        self.assertEqual(results["timed_out_tests"], (30.04, True))
        self.assertNotIn("skipped_tests", results)

    def test_repeated_case_keeps_its_longest_run(self):
        text = (
            '<testsuite><testcase name="a" time="3" status="run"/>'
            '<testcase name="a" time="9" status="run"/><testcase name="a" time="4" status="run"/></testsuite>'
        )
        self.assertEqual(read_results(text)["a"], (9.0, False))


class HeadroomMeasurementTests(unittest.TestCase):
    def setUp(self):
        self.measurements = {
            item.name: item for item in measure(read_results(JUNIT), read_timeouts(LISTING), 1500.0)
        }

    def test_flags_only_tests_over_the_threshold(self):
        over = {
            name for name, item in self.measurements.items()
            if ctest_headroom.is_over(item, ctest_headroom.DEFAULT_THRESHOLD)
        }
        self.assertEqual(over, {"near_tests", "timed_out_tests", "default_tests"})

    def test_untimed_test_uses_the_default_and_unknown_has_none(self):
        self.assertEqual(self.measurements["default_tests"].timeout_source, "default")
        self.assertAlmostEqual(self.measurements["default_tests"].fraction, 0.8)
        unbounded = measure({"a": (5.0, False)}, {}, None)[0]
        self.assertIsNone(unbounded.fraction)
        self.assertFalse(ctest_headroom.is_over(unbounded, ctest_headroom.DEFAULT_THRESHOLD))

    def test_warning_names_elapsed_timeout_and_fraction(self):
        warning = ctest_headroom.format_warning(self.measurements["near_tests"], 0.75)
        self.assertTrue(warning.startswith("::warning title=CTest timeout headroom::near_tests "))
        for text in ("160.2 s", "180 s", "89%", "75%"):
            self.assertIn(text, warning)
        self.assertIn("timed out after", ctest_headroom.format_warning(self.measurements["timed_out_tests"], 0.75))
        unknown = measure({"a": (5.0, True)}, {}, None)[0]
        self.assertIn("unknown timeout", ctest_headroom.format_warning(unknown, 0.75))

    def test_summary_orders_by_fraction_and_counts_unbounded(self):
        measurements = measure(read_results(JUNIT), read_timeouts(LISTING), None)
        summary = ctest_headroom.format_summary("Shard", measurements, 0.75, 10)
        rows = [line for line in summary.splitlines() if line.startswith("| `")]
        self.assertEqual([row.split("`")[1] for row in rows], ["timed_out_tests", "near_tests", "quick_tests"])
        self.assertIn("2 without a timeout", summary)


class HeadroomCommandTests(unittest.TestCase):
    def run_main(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = ctest_headroom.main(list(arguments))
        return status, output.getvalue()

    def test_warns_and_writes_the_summary_without_failing(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "DartConfiguration.tcl").write_text(DART, encoding="utf-8")
            (build / "tests.json").write_text(json.dumps(LISTING), encoding="utf-8")
            (build / "results.xml").write_text(JUNIT, encoding="utf-8")
            summary = build / "summary.md"
            status, output = self.run_main(
                str(build), "--junit", str(build / "results.xml"), "--tests-json", str(build / "tests.json"),
                "--summary", str(summary), "--title", "ASan shard 4",
            )
            self.assertEqual(status, 0)
            warnings = [line for line in output.splitlines() if line.startswith("::warning")]
            self.assertEqual(len(warnings), 3)
            self.assertTrue(summary.read_text(encoding="utf-8").startswith("### ASan shard 4"))

    def test_missing_results_and_listing_are_not_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            status, output = self.run_main(str(build), "--junit", str(build / "absent.xml"), "--summary", str(build / "s.md"))
            self.assertEqual(status, 0)
            self.assertIn("::notice", output)
            self.assertFalse((build / "s.md").exists())
            (build / "results.xml").write_text(JUNIT, encoding="utf-8")
            status, output = self.run_main(
                str(build), "--junit", str(build / "results.xml"), "--ctest", str(build / "no-ctest"),
                "--summary", str(build / "s.md"),
            )
            self.assertEqual(status, 0)
            self.assertIn("could not list tests", output)
            # No listing and no default: only the reported timeout remains a warning.
            self.assertEqual(output.count("::warning title=CTest timeout headroom::timed_out_tests"), 1)


class HeadroomWorkflowTests(unittest.TestCase):
    def test_each_junit_ctest_report_feeds_a_headroom_step(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        reports = set(re.findall(r'--output-junit "\$RUNNER_TEMP/([\w.-]+\.xml)"', workflow))
        self.assertTrue(reports)
        checked = set()
        for step in workflow.split("      - name: ")[1:]:
            if "ctest_headroom.py" in step:
                self.assertRegex(step, r"\n        if: always\(\)")
                checked |= set(re.findall(r'--junit "\$RUNNER_TEMP/([\w.-]+\.xml)"', step))
        self.assertEqual(reports - checked, set())


if __name__ == "__main__":
    unittest.main()
