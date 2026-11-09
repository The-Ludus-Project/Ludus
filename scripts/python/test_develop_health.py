"""Exercise the develop health gate: red batches admit only proven fixes."""
from __future__ import annotations

from pathlib import Path
import re
import unittest

import batch_ci
import develop_health


HEAD = "d" * 40
CANDIDATE = "b" * 40
FAILED = ("native / ASan and UBSan", "native / Development and SDK", "native / Ubuntu Clang")


def job(name, conclusion, identifier=1):
    return {"id": identifier, "name": name, "conclusion": conclusion}


def no_op_jobs():
    return [job("Capture develop candidate", "success"), job("native / Classify changes", "skipped"),
            job("wiki / Build and check wiki", "skipped"), job("Promote validated candidate", "skipped")]


def validated_jobs(failed=FAILED, promoted=False):
    jobs = [job("Capture develop candidate", "success"), job("native / Source formatting", "success"),
            job("wiki / Build and check wiki", "success"), job("native / macOS analyzer baseline", "skipped"),
            job("Promote validated candidate", "success" if promoted else "skipped")]
    return jobs + [job(name, "failure") for name in failed]


class FakeGitHub:
    def __init__(self):
        self.responses = {}
        self.posts = []

    def add_runs(self, workflow, query, runs):
        self.responses[f"actions/workflows/{workflow}/runs?{query}"] = {"workflow_runs": runs}

    def add_jobs(self, run_id, jobs):
        self.responses[f"actions/runs/{run_id}/jobs?filter=latest&per_page=100&page=1"] = {
            "total_count": len(jobs), "jobs": jobs}

    def add_batches(self, *batches):
        """Each batch is (run id, conclusion, jobs), newest first."""
        runs = [{"id": run_id, "conclusion": conclusion, "html_url": f"https://example.test/runs/{run_id}"}
                for run_id, conclusion, _ in batches]
        self.responses[self.batch_page(1)] = {"workflow_runs": runs}
        self.responses[self.batch_page(2)] = {"workflow_runs": []}
        for run_id, _, jobs in batches:
            self.add_jobs(run_id, jobs)

    @staticmethod
    def batch_page(page):
        return (f"actions/workflows/{develop_health.BATCH_WORKFLOW}/runs?branch=main&status=completed"
                f"&per_page={develop_health.BATCH_RUNS_PAGE_SIZE}&page={page}")

    def get(self, path):
        if path not in self.responses:
            raise develop_health.HealthError(f"unexpected request {path}")
        return self.responses[path]

    def post(self, path):
        self.posts.append(path)
        return {}


class HealthTests(unittest.TestCase):
    def test_no_op_ticks_are_skipped_and_the_latest_validated_batch_decides(self):
        client = FakeGitHub()
        client.add_batches((30, "success", no_op_jobs()), (20, "failure", validated_jobs()),
                           (10, "success", validated_jobs(failed=(), promoted=True)))
        health = develop_health.read_health(client)
        self.assertEqual(health.run_id, 20)
        self.assertTrue(health.is_red)
        self.assertEqual(health.failed_jobs, tuple(sorted(FAILED)))

    def test_cancelled_runs_and_failed_captures_prove_nothing(self):
        cancelled = {"id": 3, "conclusion": "cancelled"}
        self.assertIsNone(develop_health.classify_batch(cancelled, validated_jobs()))
        capture_failed = [job("Capture develop candidate", "failure"), job("native / Classify changes", "skipped")]
        self.assertIsNone(develop_health.classify_batch({"id": 4, "conclusion": "failure"}, capture_failed))

    def test_batch_stopped_after_a_failure_is_red_without_its_cancelled_jobs(self):
        jobs = [job("Capture develop candidate", "success"), job("Watch for failed suites", "success"),
                job("native / Clang static analysis (0)", "failure"),
                job("windows / MSVC host and D3D12 / MSBuild", "timed_out"),
                job("native / ASan and UBSan (1/4)", "cancelled"),
                job("native / Development and SDK", "success"),
                job(batch_ci.STOP_JOB, "cancelled"), job("Promote validated candidate", "skipped")]
        run = {"id": 7, "conclusion": "cancelled", "html_url": "https://example.test/runs/7"}
        health = develop_health.classify_batch(run, jobs)
        self.assertTrue(health.is_red)
        self.assertEqual(health.failed_jobs,
                         ("native / Clang static analysis (0)", "windows / MSVC host and D3D12 / MSBuild"))
        # The stop job also proves nothing when it never ran or nothing had failed.
        for stop in ("skipped", None):
            stopped = [dict(entry, conclusion=stop) if entry["name"] == batch_ci.STOP_JOB else entry
                       for entry in jobs]
            self.assertIsNone(develop_health.classify_batch(run, stopped))
        only_cancelled = [entry for entry in jobs if entry["conclusion"] not in {"failure", "timed_out"}]
        self.assertIsNone(develop_health.classify_batch(run, only_cancelled))
        # A stopped batch is the latest validated one, ahead of an older green run.
        client = FakeGitHub()
        client.add_batches((7, "cancelled", jobs), (5, "success", validated_jobs(failed=(), promoted=True)))
        self.assertEqual(develop_health.read_health(client).run_id, 7)

    def test_passing_suites_are_green_even_when_promotion_failed(self):
        jobs = validated_jobs(failed=())
        jobs[-1] = job("Promote validated candidate", "failure")
        health = develop_health.classify_batch({"id": 5, "conclusion": "failure"}, jobs)
        self.assertFalse(health.is_red)

    def test_timed_out_and_cancelled_suites_count_as_failed(self):
        jobs = [job("native / ASan and UBSan", "timed_out"), job("browser / compile-and-package", "cancelled"),
                job("sdk / Host tooling (ludus_tools) unit tests", "success")]
        health = develop_health.classify_batch({"id": 6, "conclusion": "failure"}, jobs)
        self.assertEqual(health.failed_jobs, ("browser / compile-and-package", "native / ASan and UBSan"))

    def test_scan_is_bounded_and_unknown_health_is_open(self):
        client = FakeGitHub()
        runs = [(1000 - i, "success", no_op_jobs()) for i in range(develop_health.MAX_SCANNED_BATCH_RUNS + 5)]
        client.responses[client.batch_page(1)] = {"workflow_runs": [
            {"id": run_id, "conclusion": conclusion} for run_id, conclusion, _ in runs]}
        for run_id, _, jobs in runs:
            client.add_jobs(run_id, jobs)
        health = develop_health.read_health(client)
        self.assertIsNone(health.run_id)
        is_open, _ = develop_health.evaluate(client, health, body="", head_sha=HEAD, branch="topic",
                                             is_documentation_only=False)
        self.assertTrue(is_open)

    def test_job_listing_pages_until_total_and_rejects_unbounded_runs(self):
        client = FakeGitHub()
        client.responses["actions/runs/7/jobs?filter=latest&per_page=100&page=1"] = {
            "total_count": 2, "jobs": [job("a", "success")]}
        client.responses["actions/runs/7/jobs?filter=latest&per_page=100&page=2"] = {
            "total_count": 2, "jobs": [job("b", "success")]}
        self.assertEqual([entry["name"] for entry in develop_health.list_jobs(client, 7)], ["a", "b"])
        for page in range(1, develop_health.MAX_JOB_PAGES + 1):
            client.responses[f"actions/runs/8/jobs?filter=latest&per_page=100&page={page}"] = {
                "total_count": 10_000, "jobs": [job("x", "success")]}
        with self.assertRaises(develop_health.HealthError):
            develop_health.list_jobs(client, 8)

    def test_api_failure_raises_so_the_gate_fails_closed(self):
        with self.assertRaises(develop_health.HealthError):
            develop_health.read_health(FakeGitHub())


class GateTests(unittest.TestCase):
    RED = develop_health.Health(20, "https://example.test/runs/20", tuple(sorted(FAILED)))

    def setUp(self):
        self.client = FakeGitHub()
        self.client.responses[f"git/ref/heads/{batch_ci.ATTEMPT_BRANCH}"] = {"object": {"sha": CANDIDATE}}
        self.client.responses[f"compare/{CANDIDATE}...{HEAD}?per_page=1&page=2"] = {
            "status": "ahead", "merge_base_commit": {"sha": CANDIDATE}}

    def evaluate(self, body="", health=None, is_documentation_only=False, waiver=""):
        return develop_health.evaluate(self.client, health or self.RED, body=body, head_sha=HEAD,
                                       branch="codex/fix", is_documentation_only=is_documentation_only,
                                       waiver=waiver)

    def add_evidence(self, workflow, jobs, head_sha=HEAD):
        self.client.add_runs(workflow, f"head_sha={HEAD}&per_page={develop_health.MAX_EVIDENCE_RUNS}",
                             [{"id": 50, "head_sha": head_sha}])
        self.client.add_jobs(50, jobs)

    def test_green_develop_admits_every_change(self):
        green = develop_health.Health(21, "https://example.test/runs/21", ())
        self.assertTrue(self.evaluate(health=green)[0])

    def test_red_develop_blocks_undeclared_changes_and_names_failed_jobs(self):
        is_open, lines = self.evaluate(body="Adds a feature.")
        self.assertFalse(is_open)
        report = "\n".join(lines)
        for name in FAILED:
            self.assertIn(name, report)
        self.assertIn("Batch-Fix:", report)

    def test_documentation_only_change_passes_unless_documentation_suites_are_red(self):
        self.assertTrue(self.evaluate(is_documentation_only=True)[0])
        wiki_red = develop_health.Health(22, "u", ("wiki / Build and check wiki",))
        self.assertFalse(self.evaluate(health=wiki_red, is_documentation_only=True)[0])

    def test_waiver_opens_only_the_waived_run(self):
        self.assertTrue(self.evaluate(waiver="20")[0])
        self.assertFalse(self.evaluate(waiver="19")[0])

    def test_declared_fix_requires_a_failed_job_and_evidence_on_this_head(self):
        is_open, lines = self.evaluate(body="Batch-Fix: native / Source formatting")
        self.assertFalse(is_open)
        self.assertIn("not failed jobs", "\n".join(lines))

        self.add_evidence("ci.yml", [job("ASan and UBSan", "failure")])
        is_open, lines = self.evaluate(body="Fix.\r\nBatch-Fix: native / ASan and UBSan\r\n")
        self.assertFalse(is_open)
        self.assertIn("gh workflow run ci.yml --ref codex/fix -f validation_tier=full", "\n".join(lines))

        self.add_evidence("ci.yml", [job("ASan and UBSan", "success"), job("Ubuntu Clang", "failure")])
        is_open, lines = self.evaluate(body="Batch-Fix: native / ASan and UBSan")
        self.assertTrue(is_open)
        # Independent failures stay listed; one fix need not repair every suite.
        self.assertIn("Still red after this fix:\n- native / Development and SDK", "\n".join(lines))

    def test_evidence_from_another_commit_does_not_count(self):
        self.add_evidence("ci.yml", [job("ASan and UBSan", "success")], head_sha="e" * 40)
        self.assertFalse(self.evaluate(body="Batch-Fix: native / ASan and UBSan")[0])

    def test_evidence_must_include_the_attempted_candidate(self):
        self.client.responses[f"compare/{CANDIDATE}...{HEAD}?per_page=1&page=2"] = {
            "status": "diverged", "merge_base_commit": {"sha": "a" * 40}}
        self.add_evidence("ci.yml", [job("ASan and UBSan", "success")])
        is_open, lines = self.evaluate(body="Batch-Fix: native / ASan and UBSan")
        self.assertFalse(is_open)
        self.assertIn("Merge develop first", "\n".join(lines))

    def test_other_suites_dispatch_their_own_workflow(self):
        red = develop_health.Health(23, "u", ("wiki / Build and check wiki",))
        self.add_evidence("wiki.yml", [])
        is_open, lines = self.evaluate(body="Batch-Fix: wiki / Build and check wiki", health=red)
        self.assertFalse(is_open)
        self.assertIn("gh workflow run wiki.yml --ref codex/fix`", "\n".join(lines))

    def test_declarations_are_line_anchored_unique_and_bounded(self):
        body = ("Batch-Fix: native / ASan and UBSan\n  Batch-Fix:  native / ASan and UBSan  \n"
                "Not a Batch-Fix: native / Ubuntu Clang\nBatch-Fix:\n")
        self.assertEqual(develop_health.parse_declarations(body), ["native / ASan and UBSan"])
        self.assertEqual(develop_health.parse_declarations(None), [])
        many = "\n".join(f"Batch-Fix: native / job {i}" for i in range(develop_health.MAX_DECLARATIONS + 1))
        self.assertFalse(self.evaluate(body=many)[0])


class ReevaluateTests(unittest.TestCase):
    def setUp(self):
        self.client = FakeGitHub()
        self.client.responses["actions/runs/40"] = {"id": 40, "conclusion": "failure", "html_url": "u",
                                                    "path": ".github/workflows/batch-ci.yml", "head_branch": "main"}
        self.client.responses["pulls?state=open&base=develop&per_page=100&page=1"] = [
            {"number": 7, "head": {"sha": HEAD}}, {"number": 8, "head": {"sha": "e" * 40}}]
        self.client.add_runs("ci.yml", f"event=pull_request&head_sha={HEAD}&per_page=20",
                             [{"id": 60, "head_sha": HEAD, "status": "completed"},
                              {"id": 55, "head_sha": HEAD, "status": "completed"}])
        self.client.add_jobs(60, [job("Ubuntu Clang", "success", 600), job("Develop health", "success", 601)])
        self.client.add_runs("ci.yml", f"event=pull_request&head_sha={'e' * 40}&per_page=20",
                             [{"id": 61, "head_sha": "e" * 40, "status": "in_progress"}])

    def test_red_batch_reruns_each_latest_gate_and_reports_in_progress_runs(self):
        self.client.add_jobs(40, validated_jobs())
        self.client.add_batches((40, "failure", validated_jobs()))
        lines = develop_health.reevaluate(self.client, 40)
        self.assertEqual(self.client.posts, ["actions/jobs/601/rerun"])
        self.assertIn("still in progress", "\n".join(lines))

    def test_green_after_green_and_no_op_ticks_do_not_rerun(self):
        green = validated_jobs(failed=(), promoted=True)
        self.client.add_jobs(40, green)
        self.client.add_batches((40, "success", green), (30, "success", green))
        develop_health.reevaluate(self.client, 40)
        self.client.add_jobs(40, no_op_jobs())
        develop_health.reevaluate(self.client, 40)
        self.assertEqual(self.client.posts, [])

    def test_green_after_red_reopens_blocked_pull_requests(self):
        green = validated_jobs(failed=(), promoted=True)
        self.client.add_jobs(40, green)
        self.client.add_batches((40, "success", green), (30, "failure", validated_jobs()))
        develop_health.reevaluate(self.client, 40)
        self.assertEqual(self.client.posts, ["actions/jobs/601/rerun"])

    def test_manual_dispatch_always_reruns(self):
        develop_health.reevaluate(self.client, None)
        self.assertEqual(self.client.posts, ["actions/jobs/601/rerun"])


class WorkflowContractTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]

    def test_suite_map_matches_the_batch_coordinator(self):
        workflow = (self.ROOT / ".github/workflows/batch-ci.yml").read_text(encoding="utf-8")
        callers = dict(re.findall(r"(?m)^  ([a-z][a-z0-9_-]*):\n(?:    .*\n)*?    uses: \./\.github/workflows/(\S+)$",
                                  workflow))
        self.assertEqual(callers, develop_health.BATCH_WORKFLOWS)
        self.assertEqual(set(batch_ci.VALIDATION_JOBS) | {"recovery"}, set(develop_health.BATCH_WORKFLOWS))

    def test_evidence_workflows_can_be_dispatched_on_a_branch(self):
        for workflow in develop_health.BATCH_WORKFLOWS.values():
            with self.subTest(workflow=workflow):
                text = (self.ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
                self.assertRegex(text.split("\njobs:\n", 1)[0], r"(?m)^  workflow_dispatch:")

    def test_gate_job_reads_only_and_runs_for_develop_pull_requests(self):
        workflow = (self.ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        gate = workflow.split("\n  develop-health:\n", 1)[1].split("\n\n  ", 1)[0]
        self.assertIn(f"name: {develop_health.GATE_JOB}", gate)
        self.assertIn("if: github.event_name == 'pull_request' && github.base_ref == 'develop'", gate)
        self.assertNotIn("needs:", gate)
        self.assertNotIn("write", gate)
        self.assertIn("develop_health.py gate", gate)

    def test_reevaluation_runs_after_main_batches_with_rerun_permission_only(self):
        workflow = (self.ROOT / ".github/workflows/develop-health.yml").read_text(encoding="utf-8")
        self.assertIn("workflows: [Batched integration and promotion]", workflow)
        self.assertIn("branches: [main]", workflow)
        self.assertIn("actions: write", workflow)
        self.assertNotIn("contents: write", workflow)
        self.assertIn("develop_health.py reevaluate", workflow)


if __name__ == "__main__":
    unittest.main()
