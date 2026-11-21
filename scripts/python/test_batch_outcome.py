"""Replay GitHub Actions metadata through the same collector used on main."""
from contextlib import redirect_stdout
from copy import deepcopy
from datetime import datetime, timedelta, timezone
from io import StringIO
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import batch_ci
import batch_outcome


REPOSITORY = "The-Ludus-Project/Ludus"
STALE = 43303086030
REJECTED = 43199428395
PROMOTED = 43284736412
FIXTURES = Path(__file__).resolve().parent / "fixtures/batch_outcome"


def load_fixture(run_id):
    return json.loads((FIXTURES / f"{run_id}.json").read_text(encoding="utf-8"))


class ReplayClient:
    repository = REPOSITORY

    def __init__(self, *fixtures):
        self.attempts = {fixture["run"]["run_attempt"]: fixture for fixture in fixtures}
        self.run_id = fixtures[0]["run"]["id"]
        self.paths = []
        self.logs = {int(job_id): log for fixture in fixtures for job_id, log in fixture["logs"].items()}

    def request(self, path):
        self.paths.append(path)
        for attempt, fixture in self.attempts.items():
            prefix = f"actions/runs/{self.run_id}/attempts/{attempt}"
            if path == prefix:
                return fixture["run"]
            if path == f"{prefix}/jobs?per_page=100&page=1":
                jobs = fixture["jobs"]
                return {"total_count": len(jobs), "jobs": jobs}
        if path.startswith("commits/"):
            sha = path.removeprefix("commits/")
            for fixture in self.attempts.values():
                if any(sha in log for log in fixture["logs"].values()):
                    return fixture["commit"]
        if path == f"actions/runs/{self.run_id}/artifacts?per_page=100&page=1":
            return {"artifacts": self.attempts[max(self.attempts)]["artifacts"]}
        raise AssertionError(f"Unexpected API request: {path}")

    def report(self, attempt=1):
        with patch.object(batch_outcome, "read_job_log", side_effect=lambda _, job_id: self.logs.get(job_id, "")):
            return batch_outcome.collect_report(self, self.run_id, attempt)


class HistoricalReplayTests(unittest.TestCase):
    def test_stale_request_did_no_validation(self):
        report = ReplayClient(load_fixture(STALE)).report()
        self.assertIn("**Stale no-op**. main advanced", report)
        self.assertIn("protected-develop wake or stale-request handoff unknown", report)
        self.assertIn("Candidate: none captured", report)
        self.assertIn("| Capture to first required failure | unknown |", report)
        self.assertIn("| Capture to promotion | unknown |", report)

    def test_rejection_keeps_signal_observation_and_request_distinct(self):
        report = ReplayClient(load_fixture(REJECTED)).report()
        self.assertIn("**Rejected**", report)
        self.assertIn("Trigger: schedule", report)
        self.assertIn("First failed required job: [native / ASan and UBSan (4/4)]", report)
        self.assertIn("Actions job completion: 2026-11-12T10:55:31+00:00", report)
        self.assertIn("Watcher first observed this failure: unknown.", report)
        self.assertIn("had seen the failure by 2026-11-12T11:26:16+00:00", report)
        self.assertIn("submission of the cancellation request: 2026-11-12T11:26:21+00:00", report)
        self.assertIn("| Diagnostic grace (first failure to cancellation request) | 30m 50s |", report)
        self.assertIn("**Running (1)**\n- [native / macOS native backends and diagnostics]", report)
        self.assertIn("**Unobserved", report)
        self.assertIn("[sanitizer-controls]", report)

    def test_promotion_and_publication_have_separate_results(self):
        report = ReplayClient(load_fixture(PROMOTED)).report()
        self.assertIn("**Promoted**", report)
        self.assertIn("Trigger: maintainer dispatch", report)
        self.assertIn("| Submission to capture | 0m 18s |", report)
        self.assertIn("| Candidate age at capture (committer date) | 0m 38s |", report)
        self.assertIn("| Capture to promotion | 2h 35m 13s |", report)
        self.assertIn("Documentation publication: success", report)
        self.assertIn("wiki-macos / Documentation tools (macos-15-intel)", report)
        self.assertIn("native / macOS native backends and diagnostics", report)
        self.assertIn("to promotion job start: 0m 05s", report)

    def test_cancelled_before_capture_and_missing_timestamps(self):
        fixture = load_fixture(REJECTED)
        fixture["jobs"] = []
        self.assertIn("**Cancelled**", ReplayClient(fixture).report())
        fixture = load_fixture(REJECTED)
        for job in fixture["jobs"]:
            job["completed_at"] = None
        report = ReplayClient(fixture).report()
        self.assertIn("**Rejected**", report)
        self.assertIn("First failed required job and completion: unknown.", report)
        self.assertIn("| Diagnostic grace (first failure to cancellation request) | unknown |", report)

    def test_full_rerun_has_its_own_jobs_and_trigger(self):
        first = load_fixture(REJECTED)
        second = deepcopy(load_fixture(STALE))
        second["run"] = dict(first["run"], run_attempt=2)
        second["jobs"] = [dict(job, run_attempt=2) for job in second["jobs"]]
        # A previous stop returned by a listing must never become attempt two's stop.
        second["jobs"].append(batch_outcome.find_job(first["jobs"], batch_ci.STOP_JOB))
        client = ReplayClient(first, second)
        self.assertIn("**Rejected**", client.report(1))
        report = client.report(2)
        self.assertIn(f"/runs/{REJECTED}/attempts/2", report)
        self.assertIn("GitHub event: schedule. Trigger: rerun.", report)
        self.assertIn("**Stale no-op**", report)
        self.assertNotIn("Rejection evidence", report)

    def test_publication_only_rerun_reuses_capture_without_claiming_promotion(self):
        first = load_fixture(PROMOTED)
        second = {"run": dict(first["run"], run_attempt=2, conclusion="failure"),
                  "logs": {}, "artifacts": [], "commit": {},
                  "jobs": [dict(batch_outcome.find_job(first["jobs"], batch_outcome.PUBLISH),
                                run_attempt=2, conclusion="failure")]}
        report = ReplayClient(first, second).report(2)
        self.assertIn("**Previously promoted**", report)
        self.assertIn("inherited from attempt 1", report)
        self.assertIn("Documentation publication: failure", report)
        self.assertIn("| Submission to capture | unknown |", report)
        self.assertIn("| Capture to promotion | unknown |", report)

    def test_first_observation_and_queued_jobs_use_live_watcher_evidence(self):
        fixture = load_fixture(REJECTED)
        jobs = fixture["jobs"]
        watch_id = batch_outcome.find_job(jobs, batch_outcome.WATCH)["id"]
        failed_id = batch_outcome.find_job(jobs, "native / ASan and UBSan (4/4)")["id"]
        queued_id = batch_outcome.find_job(jobs, "native / macOS analyzer baseline")["id"]
        fixture["logs"][str(watch_id)] += (
            f"\n2026-11-12T10:56:15Z Batch report observed failure {failed_id} at 2026-11-12T10:56:15Z"
            f"\n2026-11-12T11:26:16Z Batch report observed queued job {queued_id}"
        )
        report = ReplayClient(fixture).report()
        self.assertIn("Watcher first observed this failure: 2026-11-12T10:56:15+00:00.", report)
        self.assertIn("**Queued (1)**\n- [native / macOS analyzer baseline]", report)

    def test_non_coordinator_is_rejected_before_reading_any_jobs(self):
        fixture = load_fixture(STALE)
        fixture["run"]["path"] = ".github/workflows/ci.yml"
        client = ReplayClient(fixture)
        with self.assertRaises(batch_ci.BatchError):
            client.report()
        self.assertEqual(len(client.paths), 1)


class WatchTelemetryTests(unittest.TestCase):
    def test_watcher_records_first_sighting_once_and_queued_at_stop(self):
        start = datetime(2026, 11, 12, tzinfo=timezone.utc)
        now = start
        jobs = [{"id": 101, "name": "native / ASan", "conclusion": "failure", "completed_at": start.isoformat()},
                {"id": 102, "name": "native / Editor", "status": "queued"}]

        def advance(seconds):
            nonlocal now
            now += timedelta(seconds=seconds)

        output = StringIO()
        with patch.object(batch_ci, "list_run_jobs", return_value=jobs), redirect_stdout(output):
            self.assertEqual(batch_ci.watch(None, 1, clock=lambda: now, sleep=advance,
                                            grace_seconds=120), (True, ["native / ASan"]))
        self.assertEqual(output.getvalue().splitlines(), [
            "Batch report observed failure 101 at 2026-11-12T00:00:00+00:00",
            "Batch report observed queued job 102",
        ])

    def test_reporter_runs_after_cancellation_with_read_only_permissions(self):
        workflows = Path(__file__).resolve().parents[2] / ".github/workflows"
        report = (workflows / "batch-outcome.yml").read_text()
        coordinator = (workflows / "batch-ci.yml").read_text()
        self.assertIn("types: [completed]\n    branches: [main]", report)
        self.assertIn("permissions:\n  contents: read\n  actions: read", report)
        self.assertNotIn(": write", report)
        self.assertIn("github.event.workflow_run.run_attempt", report)
        self.assertIn("actions/workflows/batch-outcome.yml", coordinator)


if __name__ == "__main__":
    unittest.main()
