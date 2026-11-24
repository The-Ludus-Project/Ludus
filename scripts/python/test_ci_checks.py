"""Contracts of scripts/python/ci_checks.py, the batch job check runner."""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

import ci_checks
from ci_checks import Check


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.environment = dict(os.environ)

    def tearDown(self):
        self.directory.cleanup()

    def run_plan(self, checks, selected=None):
        # Keep the runner's ::error workflow commands out of the job log, where GitHub
        # would turn them into failure annotations on a passing job.
        self.log = io.StringIO()
        with contextlib.redirect_stdout(self.log):
            return ci_checks.run_plan(checks, self.environment, self.root, selected)

    def run_checks(self, checks, selected=None):
        return {outcome.check.id: outcome for outcome in self.run_plan(checks, selected)}

    def test_a_failure_skips_only_its_dependents_and_later_checks_still_run(self):
        checks = [
            Check("build", "Build", "touch built"),
            Check("broken", "Broken", "exit 3", needs=("build",)),
            Check("after-broken", "After broken", "touch dependent", needs=("broken",)),
            Check("independent", "Independent", "touch independent", needs=("build",)),
        ]
        outcomes = self.run_checks(checks)
        self.assertEqual({key: value.result for key, value in outcomes.items()},
                         {"build": "success", "broken": "failure", "after-broken": "skipped", "independent": "success"})
        self.assertEqual(outcomes["broken"].reason, "exit status 3")
        self.assertEqual(outcomes["after-broken"].reason, "needs broken")
        self.assertTrue((self.root / "independent").exists())
        self.assertFalse((self.root / "dependent").exists())

    def test_checks_use_githubs_strict_shell(self):
        outcomes = self.run_checks([Check("pipe", "Pipe", "false | true\ntouch reached")])
        self.assertEqual(outcomes["pipe"].result, "failure")
        self.assertFalse((self.root / "reached").exists())

    def test_check_environment_expands_job_variables(self):
        self.environment["LUDUS_TEST_ROOT"] = str(self.root)
        check = Check("environment", "Environment", 'test "$TARGET" = "$LUDUS_TEST_ROOT/out"',
                      env={"TARGET": "$LUDUS_TEST_ROOT/out"})
        self.assertEqual(self.run_checks([check])["environment"].result, "success")

    def test_a_timeout_stops_the_whole_process_group(self):
        marker = self.root / "late"
        check = Check("slow", "Slow", f"(sleep 5; touch {marker}) & sleep 5", timeout_minutes=1 / 60)
        started = time.monotonic()
        outcome = self.run_checks([check])["slow"]
        self.assertLess(time.monotonic() - started, 4)
        self.assertEqual(outcome.result, "failure")
        self.assertIn("timed out", outcome.reason)
        time.sleep(5.5)
        self.assertFalse(marker.exists(), "a timed-out check's background child kept running")

    def test_selected_checks_assume_their_unselected_needs(self):
        checks = [Check("build", "Build", "exit 1"), Check("test", "Test", "touch tested", needs=("build",))]
        outcomes = self.run_checks(checks, selected=["test"])
        self.assertEqual(list(outcomes), ["test"])
        self.assertEqual(outcomes["test"].result, "success")

    def test_outputs_and_summary_name_every_check(self):
        checks = [Check("build", "Build", "true"), Check("broken", "Broken", "exit 1", needs=("build",)),
                  Check("skipped", "Skipped", "true", needs=("broken",))]
        outcomes = self.run_plan(checks)
        self.assertIn("::error title=Broken::Broken failed: exit status 1", self.log.getvalue())
        output = self.root / "output"
        summary = self.root / "summary"
        ci_checks.write_outputs(outcomes, str(output))
        text = ci_checks.write_summary("plan checks", outcomes, str(summary))
        self.assertEqual(output.read_text(), "build=success\nbroken=failure\nskipped=skipped\n")
        self.assertIn("| Broken | failure |", text)
        self.assertIn("| Skipped | skipped |  | needs broken |", summary.read_text())


class PlanTests(unittest.TestCase):
    def write_plan(self, body: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "plan.py"
        path.write_text("from ci_checks import Check\n" + body, encoding="utf-8")
        return path

    def test_plans_reject_duplicates_unknown_needs_and_workflow_expressions(self):
        for body, message in (
            ('CHECKS = [Check("a", "A", "true"), Check("a", "A again", "true")]', "duplicate"),
            ('CHECKS = [Check("a", "A", "true", needs=("b",)), Check("b", "B", "true")]', "no earlier check"),
            ('CHECKS = [Check("a", "A", "echo ${{ github.sha }}")]', "workflow expression"),
            ("CHECKS = []", "no CHECKS"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                ci_checks.load_plan(self.write_plan(body))

    def test_main_fails_after_running_every_check_and_reports_each_failure(self):
        plan = self.write_plan('CHECKS = [Check("one", "One", "exit 1"), Check("two", "Two", "exit 2")]')
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        output = Path(directory.name) / "output"
        previous = {name: os.environ.get(name) for name in ("GITHUB_OUTPUT", "GITHUB_STEP_SUMMARY")}
        os.environ["GITHUB_OUTPUT"] = str(output)
        os.environ.pop("GITHUB_STEP_SUMMARY", None)
        log = io.StringIO()
        try:
            with contextlib.redirect_stdout(log):
                self.assertEqual(ci_checks.main([str(plan)]), 1)
        finally:
            for name, value in previous.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
        self.assertEqual(output.read_text(), "one=failure\ntwo=failure\n")
        self.assertIn("::error title=Two::Two failed: exit status 2", log.getvalue())

    def test_the_macos_native_plan_loads(self):
        plan = Path(__file__).resolve().parents[2] / "scripts/ci/macos_native.py"
        checks = ci_checks.load_plan(plan)
        self.assertGreater(len(checks), 30)


if __name__ == "__main__":
    # Plans import Check from ci_checks; keep one module for both.
    sys.modules.setdefault("ci_checks", ci_checks)
    unittest.main()
