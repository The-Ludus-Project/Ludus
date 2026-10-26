"""Exercise batch capture, failure preservation and exact-commit promotion."""
from __future__ import annotations

import json
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error

import batch_ci


BASE = "a" * 40
CANDIDATE = "b" * 40
LATER = "c" * 40


def successful_results(recovery=False):
    return {
        "snapshot": {"result": "success", "outputs": {"should_run": "true", "base_sha": BASE,
            "candidate_sha": CANDIDATE, "recovery_required": "true" if recovery else "false"}},
        **{name: {"result": "success"} for name in batch_ci.VALIDATION_JOBS},
        "recovery": {"result": "success" if recovery else "skipped"},
    }


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.client = mock.Mock(repository="example/engine", token="test-token")
        self.heads = {"main": BASE, "develop": CANDIDATE, batch_ci.ATTEMPT_BRANCH: None}
        self.client.head.side_effect = self.heads.__getitem__
        self.client.is_ancestor.return_value = True
        self.git = mock.patch("batch_ci.git", return_value="docs/index.md\0").start()
        self.addCleanup(mock.patch.stopall)

    def capture(self, **options):
        return batch_ci.snapshot(self.client, Path.cwd(), BASE, **options)

    def test_capture_freezes_exact_commit_and_marks_attempt_before_validation(self):
        result = self.capture()
        self.assertEqual(result["candidate_sha"], CANDIDATE)
        self.assertEqual(result["base_sha"], BASE)
        self.assertEqual(result["should_run"], "true")
        self.assertEqual(result["recovery_required"], "false")
        self.client.request.assert_called_once_with("git/refs", "POST",
            {"ref": f"refs/heads/{batch_ci.ATTEMPT_BRANCH}", "sha": CANDIDATE})
        self.assertIn(f"{BASE}..{CANDIDATE}", self.git.call_args.args[1])

    def test_unchanged_main_dev_and_stale_scheduler_do_no_work(self):
        for main, develop in ((BASE, BASE), (LATER, CANDIDATE)):
            with self.subTest(main=main, develop=develop):
                self.heads.update(main=main, develop=develop)
                self.assertEqual(self.capture()["should_run"], "false")
                self.client.request.assert_not_called()
                self.git.assert_not_called()

    def test_same_failed_or_cancelled_candidate_waits_for_fix_or_explicit_retry(self):
        self.heads[batch_ci.ATTEMPT_BRANCH] = CANDIDATE
        self.assertEqual(self.capture()["should_run"], "false")
        self.git.assert_not_called()
        self.assertEqual(self.capture(retry=True)["should_run"], "true")
        self.client.request.assert_not_called()

    def test_next_candidate_advances_marker_without_force(self):
        self.heads[batch_ci.ATTEMPT_BRANCH] = BASE
        self.capture()
        self.client.request.assert_called_once_with(f"git/refs/heads/{batch_ci.ATTEMPT_BRANCH}",
            "PATCH", {"sha": CANDIDATE, "force": False})

    def test_diverged_dev_and_api_failure_cannot_mark_a_candidate(self):
        self.client.is_ancestor.return_value = False
        with self.assertRaises(batch_ci.BatchError):
            self.capture()
        self.client.request.assert_not_called()
        self.client.head.side_effect = batch_ci.BatchError("API unavailable")
        with self.assertRaises(batch_ci.BatchError):
            self.capture()
        self.client.request.assert_not_called()

    def test_recovery_detection_covers_deletions_renames_and_large_diffs(self):
        for path in (*batch_ci.RECOVERY_PATHS, "tests/filesystem/recovery_block.py"):
            with self.subTest(path=path):
                self.git.return_value = "docs/other.md\0" * 400 + path + "\0"
                self.assertEqual(self.capture()["recovery_required"], "true")
                arguments = self.git.call_args.args[1]
                self.assertIn("--no-renames", arguments)
                self.assertIn("-z", arguments)


class SnapshotReportTests(unittest.TestCase):
    """A green no-op tick must say that nothing was validated or promoted."""

    def run_snapshot(self, outputs):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            summary = Path(directory) / "summary"
            environment = {"GITHUB_REF": "refs/heads/main", "GITHUB_REPOSITORY": "example/engine",
                           "GH_TOKEN": "test-token", "GITHUB_SHA": BASE,
                           "GITHUB_OUTPUT": str(output), "GITHUB_STEP_SUMMARY": str(summary)}
            with mock.patch.dict("os.environ", environment), \
                    mock.patch("sys.argv", ["batch_ci.py", "snapshot"]), \
                    mock.patch("batch_ci.snapshot", return_value=outputs), \
                    mock.patch("builtins.print") as printed:
                self.assertEqual(batch_ci.main(), 0)
            return output.read_text(), summary.read_text(), printed.call_args.args[0]

    def test_skipped_capture_annotates_run_and_summary_with_reason(self):
        reason = f"develop {CANDIDATE} was already attempted; fix develop or dispatch with retry."
        output, summary, annotation = self.run_snapshot({"should_run": "false", "reason": reason})
        self.assertEqual(output, "should_run=false\n")
        self.assertTrue(annotation.startswith("::notice title=No promotion this run::"))
        self.assertIn(reason, annotation)
        self.assertIn("main is unchanged", annotation)
        self.assertIn("### No promotion this run", summary)
        self.assertIn(reason, summary)

    def test_notice_escapes_workflow_command_data(self):
        annotation, _ = batch_ci.describe_snapshot({"should_run": "false", "reason": "100%\nnext\rline"})
        self.assertIn("100%25%0Anext%0Dline", annotation)
        self.assertNotIn("\n", annotation)

    def test_selected_capture_reports_validation_without_skip_notice(self):
        reason = f"Validate frozen develop commit {CANDIDATE} above main {BASE}."
        _, summary, printed = self.run_snapshot({"should_run": "true", "base_sha": BASE,
                                                  "candidate_sha": CANDIDATE, "recovery_required": "false",
                                                  "reason": reason})
        self.assertEqual(printed, reason)
        self.assertIn("### Validating develop candidate", summary)
        self.assertNotIn("No promotion", summary)


class PromotionTests(unittest.TestCase):
    def setUp(self):
        self.client = mock.Mock(repository="example/engine", token="test-token")
        self.client.head.side_effect = {"main": BASE, "develop": LATER}.__getitem__
        self.client.is_ancestor.return_value = True
        self.push = mock.patch("batch_ci.push_fast_forward").start()
        self.addCleanup(mock.patch.stopall)

    def promote(self, results=None):
        batch_ci.promote(self.client, Path.cwd(), BASE, CANDIDATE,
                         successful_results() if results is None else results)

    def test_new_dev_commits_do_not_change_the_commit_being_promoted(self):
        self.promote()
        self.push.assert_called_once_with(Path.cwd(), "https://github.com/example/engine.git",
                                         BASE, CANDIDATE, "test-token")
        self.client.is_ancestor.assert_any_call(CANDIDATE, LATER)

    def test_every_failed_cancelled_missing_or_skipped_suite_preserves_main(self):
        for name in batch_ci.VALIDATION_JOBS:
            for state in ("failure", "cancelled", "skipped", None):
                with self.subTest(job=name, state=state):
                    results = successful_results()
                    results[name]["result"] = state
                    with self.assertRaises(batch_ci.BatchError):
                        self.promote(results)
                    self.push.assert_not_called()
            results = successful_results()
            del results[name]
            with self.assertRaises(batch_ci.BatchError):
                self.promote(results)
        self.push.assert_not_called()

    def test_invalid_or_forged_capture_and_unexpected_jobs_preserve_main(self):
        for key, value in (("base_sha", LATER), ("candidate_sha", LATER),
                           ("should_run", "false"), ("recovery_required", "")):
            results = successful_results()
            results["snapshot"]["outputs"][key] = value
            with self.subTest(key=key), self.assertRaises(batch_ci.BatchError):
                self.promote(results)
        results = successful_results()
        results["unexpected"] = {"result": "success"}
        with self.assertRaises(batch_ci.BatchError):
            self.promote(results)
        self.push.assert_not_called()

    def test_recovery_is_required_only_when_selected(self):
        batch_ci.validate_results(successful_results(True), BASE, CANDIDATE)
        for required, status in ((True, "skipped"), (True, "failure"), (False, "cancelled")):
            results = successful_results(required)
            results["recovery"]["result"] = status
            with self.subTest(required=required, status=status), self.assertRaises(batch_ci.BatchError):
                self.promote(results)
        self.push.assert_not_called()

    def test_moved_main_or_rewritten_dev_preserves_main(self):
        self.client.head.side_effect = {"main": LATER, "develop": CANDIDATE}.__getitem__
        with self.assertRaises(batch_ci.BatchError):
            self.promote()
        self.client.head.side_effect = {"main": BASE, "develop": CANDIDATE}.__getitem__
        for answers in ((False,), (True, False)):
            self.client.is_ancestor.side_effect = answers
            with self.assertRaises(batch_ci.BatchError):
                self.promote()
        self.push.assert_not_called()


class GitPromotionTests(unittest.TestCase):
    """Real repositories prove the push never promotes a new merge or overwrites main."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        self.remote = str(self.root / "remote.git")
        self.run_git(self.root, "init", "--bare", self.remote)
        self.run_git(self.checkout, "init", "-b", "main")
        self.run_git(self.checkout, "config", "user.name", "Batch CI test")
        self.run_git(self.checkout, "config", "user.email", "batch@example.invalid")
        self.base = self.commit("stable")
        self.run_git(self.checkout, "push", self.remote, "HEAD:refs/heads/main")
        self.candidate = self.commit("candidate")

    @staticmethod
    def run_git(root, *arguments):
        return subprocess.run(["git", *arguments], cwd=root, capture_output=True, text=True,
                              check=True).stdout.strip()

    def commit(self, content):
        (self.checkout / "source").write_text(content)
        self.run_git(self.checkout, "add", "source")
        self.run_git(self.checkout, "commit", "-m", content)
        return self.run_git(self.checkout, "rev-parse", "HEAD")

    def remote_main(self):
        return self.run_git(self.root, "--git-dir", self.remote, "rev-parse", "refs/heads/main")

    def prepare_remote(self):
        # Make the candidate available without updating stable, as develop would.
        self.run_git(self.checkout, "push", self.remote, f"{self.candidate}:refs/heads/develop")

    def test_success_promotes_exact_sha_even_when_checkout_has_later_changes(self):
        self.prepare_remote()
        self.commit("later develop")
        batch_ci.push_fast_forward(self.checkout, self.remote, self.base, self.candidate)
        self.assertEqual(self.remote_main(), self.candidate)

    def test_lease_rejects_main_advance_even_when_candidate_could_fast_forward_it(self):
        self.prepare_remote()
        # Another actor advances main to the candidate after the API check.
        self.run_git(self.checkout, "push", self.remote, f"{self.candidate}:refs/heads/main")
        later = self.commit("later")
        self.run_git(self.checkout, "push", self.remote, f"{later}:refs/heads/develop")
        with self.assertRaises(batch_ci.BatchError):
            batch_ci.push_fast_forward(self.checkout, self.remote, self.base, later)
        self.assertEqual(self.remote_main(), self.candidate)

    def test_divergent_candidate_cannot_rewrite_main_even_with_valid_lease(self):
        self.prepare_remote()
        self.run_git(self.checkout, "checkout", "--orphan", "unrelated")
        unrelated = self.commit("unrelated")
        self.run_git(self.checkout, "push", self.remote, f"{unrelated}:refs/heads/unrelated")
        with self.assertRaises(batch_ci.BatchError):
            batch_ci.push_fast_forward(self.checkout, self.remote, self.base, unrelated)
        self.assertEqual(self.remote_main(), self.base)


class GitHubTests(unittest.TestCase):
    def test_ancestry_avoids_large_first_page_without_relaxing_response_limit(self):
        client = batch_ci.GitHub("example/engine", "test-token")
        comparison = {"status": "ahead", "merge_base_commit": {"sha": BASE}, "commits": []}
        first_page = {**comparison, "files": [{"patch": "x" * (2 * 1024 * 1024)}]}

        def open_response(request, timeout):
            response = mock.MagicMock()
            payload = comparison if request.full_url.endswith("?per_page=1&page=2") else first_page
            response.__enter__.return_value.read.side_effect = lambda size: json.dumps(payload).encode()[:size]
            return response

        with mock.patch("urllib.request.urlopen", side_effect=open_response):
            self.assertTrue(client.is_ancestor(BASE, CANDIDATE))
            with self.assertRaises(batch_ci.BatchError):
                client.request(f"compare/{BASE}...{CANDIDATE}")

    def test_ancestry_requires_whole_comparison_status_and_matching_merge_base(self):
        client = batch_ci.GitHub("example/engine", "test-token")
        for status, merge_base, expected in (("ahead", BASE, True), ("ahead", LATER, False),
                                              ("behind", BASE, False), ("diverged", BASE, False),
                                              ("identical", BASE, False), (None, None, False)):
            with self.subTest(status=status, merge_base=merge_base):
                with mock.patch.object(client, "request", return_value={
                    "status": status, "merge_base_commit": {"sha": merge_base}, "commits": [],
                }) as request:
                    self.assertEqual(client.is_ancestor(BASE, CANDIDATE), expected)
                    request.assert_called_once_with(f"compare/{BASE}...{CANDIDATE}?per_page=1&page=2")
        with mock.patch.object(client, "request") as request:
            self.assertTrue(client.is_ancestor(BASE, BASE))
            request.assert_not_called()

    def test_only_marker_404_is_treated_as_absent(self):
        client = batch_ci.GitHub("example/engine", "test-token")
        for status in (401, 403, 404, 429, 500):
            error = urllib.error.HTTPError("https://api.github.com", status, "failure", {}, None)
            with mock.patch("urllib.request.urlopen", side_effect=error):
                if status == 404:
                    self.assertIsNone(client.head(batch_ci.ATTEMPT_BRANCH))
                else:
                    with self.assertRaises(batch_ci.BatchError):
                        client.head(batch_ci.ATTEMPT_BRANCH)
                with self.assertRaises(batch_ci.BatchError):
                    client.head("develop")

    def test_api_metadata_and_response_boundaries(self):
        client = batch_ci.GitHub("example/engine", "test-token")
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(
            {"object": {"type": "commit", "sha": CANDIDATE}}).encode()
        with mock.patch("urllib.request.urlopen", return_value=response) as request:
            self.assertEqual(client.head("develop"), CANDIDATE)
            self.assertEqual(request.call_args.args[0].full_url,
                             "https://api.github.com/repos/example/engine/git/ref/heads/develop")
        response.__enter__.return_value.read.return_value = b"x" * (2 * 1024 * 1024 + 1)
        with mock.patch("urllib.request.urlopen", return_value=response), self.assertRaises(batch_ci.BatchError):
            client.head("develop")
        for value in ("", "0" * 40, "--help", CANDIDATE + "\n"):
            with self.subTest(value=value), self.assertRaises(batch_ci.BatchError):
                batch_ci.validate_sha(value)


class WorkflowContractTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]

    def test_all_candidate_checkouts_use_the_same_frozen_sha(self):
        for name in ("ci.yml", "project-sdk.yml", "wiki.yml", "webgpu-probe.yml", "filesystem-recovery.yml",
                     "threading-platforms.yml", "windows-direct3d12.yml"):
            workflow = (self.ROOT / ".github/workflows" / name).read_text()
            checkouts = re.findall(r"      - uses: actions/checkout@[^\n]+\n(.*?)(?=      - |\n  [a-z]|\Z)",
                                   workflow, re.DOTALL)
            self.assertTrue(checkouts, name)
            for step in checkouts:
                with self.subTest(workflow=name):
                    self.assertIn("ref: ${{ inputs.batch_sha || github.sha }}", step)
                    self.assertIn("persist-credentials: false", step)

    def test_promotion_privilege_is_separate_from_candidate_execution(self):
        workflow = (self.ROOT / ".github/workflows/batch-ci.yml").read_text()
        self.assertIn("group: ludus-batch-promotion\n  cancel-in-progress: false", workflow)
        for name in batch_ci.VALIDATION_JOBS:
            job = workflow.split(f"\n  {name}:\n", 1)[1].split("\n\n  ", 1)[0]
            self.assertIn("batch_sha: ${{ needs.snapshot.outputs.candidate_sha }}", job)
            self.assertNotIn("secrets:", job)
        promote = workflow.split("\n  promote:\n", 1)[1].split("\n  publish:\n", 1)[0]
        self.assertIn("environment: batch-promotion", promote)
        dependencies = re.search(r"needs: \[([^\]]+)\]", promote)
        self.assertCountEqual(dependencies.group(1).split(", "),
                              ["snapshot", "recovery", *batch_ci.VALIDATION_JOBS])
        checkout = promote.split("uses: actions/checkout@", 1)[1].split("      - name:", 1)[0]
        self.assertNotIn("batch_sha", checkout)
        self.assertIn("!cancelled()", promote)
        self.assertIn("BATCH_CI_ENABLED", workflow)
        snapshot = workflow.split("\n  snapshot:\n", 1)[1].split("\n  native:\n", 1)[0]
        self.assertIn("environment: batch-promotion", snapshot)
        self.assertNotRegex(snapshot, r"(?m)^      contents: write$")
        self.assertIn("    needs: promote", workflow.split("\n  publish:\n", 1)[1])

    def test_publication_explicitly_checks_promotion_after_optional_ancestor_skips(self):
        workflow = (self.ROOT / ".github/workflows/batch-ci.yml").read_text()
        publish = workflow.split("\n  publish:\n", 1)[1].split("    steps:\n", 1)[0]
        condition = re.search(r"(?m)^    if: (.*)$", publish)
        self.assertIsNotNone(condition, "Implicit success() also rejects intentionally skipped recovery ancestors.")
        self.assertIn("always()", condition.group(1))
        self.assertIn("!cancelled()", condition.group(1))
        self.assertIn("needs.promote.result == 'success'", condition.group(1))


class WorkflowTargetTests(unittest.TestCase):
    """Batch jobs run main's workflow definitions against the develop candidate.

    A CMake target that main's workflows still build must exist in the candidate,
    or every batch fails with "ninja: error: unknown target" until promotion.
    """
    ROOT = Path(__file__).resolve().parents[2]
    TRUSTED_REFERENCE = "origin/main"
    # Names separated by blanks or shell line continuations, ending at the next option or command.
    TARGET_LIST = re.compile(r"--target((?:(?:[ \t]+|[ \t]*\\\n\s*)[A-Za-z_]\w*(?![\w/.-]))+)")
    TARGET_NAME = re.compile(r"[A-Za-z_]\w*")
    CMAKE_NAME = re.compile(r"ludus_[\w${}]*")

    def defined_target_patterns(self) -> list[re.Pattern[str]]:
        listing = subprocess.run(["git", "-C", str(self.ROOT), "ls-files", "*CMakeLists.txt", "*.cmake"],
                                 capture_output=True, text=True, check=True)
        names = set()
        for name in listing.stdout.split("\n"):
            if name:
                text = (self.ROOT / name).read_text(encoding="utf-8", errors="replace")
                names.update(self.CMAKE_NAME.findall(text))
        # Generated names such as ludus_${name}_control_child match any expansion.
        return [re.compile(re.sub(r"\\\$\\\{\w+\\\}", r"\\w+", re.escape(name)) + r"\Z") for name in names]

    def workflow_targets(self, texts: dict[str, str]) -> set[tuple[str, str]]:
        return {(workflow, name) for workflow, text in texts.items()
                for targets in self.TARGET_LIST.findall(text) for name in self.TARGET_NAME.findall(targets)}

    def assert_targets_defined(self, texts: dict[str, str]):
        patterns = self.defined_target_patterns()
        missing = sorted((workflow, name) for workflow, name in self.workflow_targets(texts)
                         if any(pattern.match(name) for pattern in patterns) == False)
        self.assertEqual(missing, [], "workflows build targets this checkout does not define")

    def test_workflow_targets_exist_in_this_checkout(self):
        workflows = sorted((self.ROOT / ".github/workflows").glob("*.yml"))
        self.assert_targets_defined({path.name: path.read_text(encoding="utf-8") for path in workflows})

    def test_trusted_workflow_targets_exist_in_this_checkout(self):
        listing = subprocess.run(["git", "-C", str(self.ROOT), "ls-tree", "--name-only",
                                  self.TRUSTED_REFERENCE, ".github/workflows/"],
                                 capture_output=True, text=True, check=False)
        if listing.returncode != 0:
            self.skipTest(f"{self.TRUSTED_REFERENCE} is not available in this checkout")
        texts = {}
        for name in listing.stdout.split():
            shown = subprocess.run(["git", "-C", str(self.ROOT), "show", f"{self.TRUSTED_REFERENCE}:{name}"],
                                   capture_output=True, text=True, check=True)
            texts[name] = shown.stdout
        self.assert_targets_defined(texts)


if __name__ == "__main__":
    unittest.main()
