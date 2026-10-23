"""Regression coverage for complete-change routing and required CI results."""
from __future__ import annotations

import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

import ci_scope


class DocumentationScopeTests(unittest.TestCase):
    def test_documentation_and_its_tooling_can_skip_native_validation(self):
        paths = ["docs/development/api-reference.md", "docs/assets/wiki.css", "README.md"]
        self.assertTrue(ci_scope.documentation_only(paths))
        self.assertTrue(ci_scope.documentation_only(sorted(ci_scope.DOCUMENTATION_FILES)))

    def test_mixed_unknown_build_engine_and_policy_paths_require_native_validation(self):
        for path in (
            "modules/foundation/base/include/ludus/foundation/base/types.h",
            "apps/smoke/main.cpp", "CMakeLists.txt", "conan.lock",
            "config/tool_versions.json", "scripts/python/engine.py",
            ".github/workflows/ci.yml", ".github/workflows/project-sdk.yml",
            ".github/actions/change-scope/action.yml", "scripts/python/ci_scope.py",
            "scripts/python/test_ci_scope.py", "AGENTS.md", "unknown.md",
            "docs/../modules/engine.cpp", "/docs/page.md", "docs-other/page.md",
        ):
            with self.subTest(path=path):
                self.assertFalse(ci_scope.documentation_only(["docs/index.md", path]))
        self.assertFalse(ci_scope.documentation_only([]))

    def test_manual_missing_malformed_and_unavailable_comparisons_require_native_validation(self):
        for event_name, event in (
            ("workflow_dispatch", {}), ("schedule", {}), ("pull_request", {}),
            ("pull_request", {"pull_request": None}),
            ("pull_request", {"pull_request": {"base": {"sha": "0" * 40}, "head": {"sha": "a" * 40}}}),
            ("pull_request", {"pull_request": {"base": {"sha": "--help"}, "head": {"sha": "a" * 40}}}),
            # No workflow triggers on these events; they must not route around native validation.
            ("push", {"before": "a" * 40, "after": "b" * 40}),
            ("merge_group", {"merge_group": {"base_sha": "a" * 40, "head_sha": "b" * 40}}),
        ):
            with self.subTest(event_name=event_name, event=event):
                self.assertTrue(ci_scope.classify(event_name, event, Path.cwd())[0])
        with mock.patch("ci_scope.changed_paths", side_effect=subprocess.CalledProcessError(128, "git")):
            self.assertTrue(ci_scope.classify("pull_request", {}, Path.cwd())[0])


class ValidationTierTests(unittest.TestCase):
    @staticmethod
    def pull_request(base_ref, *labels):
        return {"pull_request": {"base": {"ref": base_ref}, "labels": [{"name": label} for label in labels]}}

    def test_only_develop_pull_requests_defer_compiled_jobs_and_labels_do_not_route(self):
        for labels in ((), ("editor",), ("ci:full",)):
            with self.subTest(labels=labels):
                self.assertEqual(ci_scope.validation_tier("pull_request", self.pull_request("develop", *labels)),
                                 ci_scope.INTEGRATION_TIER)
                for base_ref in ("main", "feature/rhi-vulkan"):
                    self.assertEqual(ci_scope.validation_tier("pull_request", self.pull_request(base_ref, *labels)),
                                     ci_scope.FULL_TIER)

    def test_unknown_events_run_the_full_tier(self):
        for event_name in ("merge_group", "push", ""):
            with self.subTest(event_name=event_name):
                self.assertEqual(ci_scope.validation_tier(event_name, {}), ci_scope.FULL_TIER)

    def test_only_standalone_scheduled_or_manual_runs_select_nightly(self):
        for event_name in ("schedule", "workflow_dispatch"):
            self.assertEqual(ci_scope.validation_tier(event_name, {}), ci_scope.NIGHTLY_TIER)
            self.assertEqual(ci_scope.validation_tier(event_name, {}, is_batch=True), ci_scope.FULL_TIER)

    def test_malformed_pull_request_metadata_runs_the_full_tier(self):
        for event in ({}, {"pull_request": None}, {"pull_request": {"base": None}},
                      {"pull_request": {"base": {"ref": None}}}):
            with self.subTest(event=event):
                self.assertEqual(ci_scope.validation_tier("pull_request", event), ci_scope.FULL_TIER)


class GitComparisonTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "CI scope test")
        self.git("config", "user.email", "ci@example.invalid")
        self.write("docs/index.md", "Original docs")
        self.write("modules/engine.cpp", "Original engine")
        self.base = self.commit()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True, text=True).stdout.strip()

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def commit(self):
        self.git("add", "--all")
        self.git("commit", "-m", "test change")
        return self.git("rev-parse", "HEAD")

    def pull_request(self, head, base=None, base_ref="develop"):
        return {"pull_request": {"base": {"sha": base or self.base, "ref": base_ref}, "head": {"sha": head}}}

    def test_documentation_only_pr_is_exempt(self):
        self.write("docs/index.md", "Updated docs")
        head = self.commit()
        self.assertFalse(ci_scope.classify("pull_request", self.pull_request(head), self.root)[0])

    def test_latest_docs_commit_does_not_hide_earlier_engine_changes(self):
        self.write("modules/engine.cpp", "Changed engine")
        self.commit()
        self.write("docs/index.md", "Updated docs")
        head = self.commit()
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(head), self.root)[0])

    def test_pr_uses_merge_base_when_target_branch_advances(self):
        self.git("checkout", "-b", "docs-change")
        self.write("docs/index.md", "Updated docs")
        head = self.commit()
        self.git("checkout", "main")
        self.write("modules/engine.cpp", "Unrelated upstream change")
        base = self.commit()
        self.assertFalse(ci_scope.classify("pull_request", self.pull_request(head, base), self.root)[0])

    def test_engine_rename_into_documentation_is_not_exempt(self):
        self.git("mv", "modules/engine.cpp", "docs/engine.cpp")
        head = self.commit()
        paths = ci_scope.changed_paths("pull_request", self.pull_request(head), self.root)
        self.assertIn("modules/engine.cpp", paths)
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(head), self.root)[0])

    def test_documentation_deletions_and_unusual_names_remain_exempt(self):
        self.git("rm", "docs/index.md")
        self.write("docs/space and\nnewline.md", "New documentation")
        head = self.commit()
        paths = ci_scope.changed_paths("pull_request", self.pull_request(head), self.root)
        self.assertIn("docs/space and\nnewline.md", paths)
        self.assertIn("docs/index.md", paths)
        self.assertFalse(ci_scope.classify("pull_request", self.pull_request(head), self.root)[0])

    def test_complete_file_list_includes_engine_change_after_300_docs_files(self):
        for index in range(305):
            self.write(f"docs/page-{index:03}.md", "New docs")
        self.write("modules/engine.cpp", "Changed engine")
        head = self.commit()
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(head), self.root)[0])

    def test_empty_diff_and_missing_history_require_native_validation(self):
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(self.base), self.root)[0])
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(self.base, "a" * 40), self.root)[0])

    def test_cli_outputs_github_routing_and_summary(self):
        self.write("docs/index.md", "Updated docs")
        head = self.commit()
        event = self.root / "event.json"
        output, summary = self.root / "output", self.root / "summary"
        cases = (
            ("pull_request", self.pull_request(head), "", "false", ci_scope.INTEGRATION_TIER),
            ("pull_request", self.pull_request(head, base_ref="main"), "", "false", ci_scope.FULL_TIER),
            ("workflow_dispatch", {}, "", "true", ci_scope.NIGHTLY_TIER),
            ("workflow_dispatch", {}, head, "true", ci_scope.FULL_TIER),
            ("schedule", {}, head, "true", ci_scope.FULL_TIER),
        )
        for event_name, payload, batch_sha, required, tier in cases:
            with self.subTest(event_name=event_name, batch_sha=batch_sha):
                event.write_text(json.dumps(payload), encoding="utf-8")
                output.unlink(missing_ok=True)
                summary.unlink(missing_ok=True)
                subprocess.run(
                    [sys.executable, ci_scope.__file__, "classify"], cwd=self.root, check=True, capture_output=True,
                    env={**os.environ, "GITHUB_EVENT_NAME": event_name, "GITHUB_EVENT_PATH": str(event),
                         "GITHUB_OUTPUT": str(output), "GITHUB_STEP_SUMMARY": str(summary),
                         # Model this invocation explicitly, even inside a batch runner.
                         "LUDUS_CI_BATCH_SHA": batch_sha},
                )
                self.assertEqual(output.read_text(), f"native_required={required}\nvalidation_tier={tier}\n")
                self.assertIn(f"Native validation required: {required}. Validation tier: {tier}.", summary.read_text())


class RequiredGateTests(unittest.TestCase):
    def test_workflow_gate_requires_every_classified_native_job(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        gate = workflow.split("\n  linux:", 1)[1]
        dependencies = re.search(r"needs: \[([^\]]+)\]", gate)
        self.assertIsNotNone(dependencies)
        self.assertCountEqual([name.strip() for name in dependencies.group(1).split(",")],
                              ["changes", *ci_scope.NATIVE_JOBS])
        # A missing workflow job must not silently remove validation while the
        # Python gate still expects it, including docs-only routing.
        jobs = set(re.findall(r"^  ([a-z][a-z0-9-]*):$", workflow, re.MULTILINE))
        self.assertTrue(set(ci_scope.NATIVE_JOBS) <= jobs)

    def test_workflow_defers_exactly_the_integration_and_nightly_jobs(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        self.assertNotIn("'pull-request'", workflow)
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$", workflow, re.MULTILINE))
        for name in ci_scope.NATIVE_JOBS:
            condition = conditions.get(name, "")
            self.assertEqual("validation_tier == 'nightly'" in condition, name in ci_scope.NIGHTLY_JOBS)
            self.assertEqual("validation_tier != 'integration'" in condition or "validation_tier == 'nightly'" in condition,
                             name in ci_scope.INTEGRATION_JOBS, name)

    def test_advisory_analyzers_run_nightly_and_required_inventory_runs_in_every_batch(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        nightly = {
            "ci.yml": "if: needs.changes.outputs.validation_tier == 'nightly'",
            "webgpu-probe.yml": "if: inputs.batch_sha == '' && github.event_name != 'pull_request'",
        }
        for name, condition in nightly.items():
            jobs = re.findall(r"(?ms)^  ([a-z][a-z0-9-]*):\n(.*?)(?=^  [a-z][a-z0-9-]*:\n|\Z)",
                              root.joinpath(name).read_text().split("\njobs:\n", 1)[1])
            analyzer_steps = 0
            for job, body in jobs:
                header = body.split("\n    steps:", 1)[0]
                for step in re.findall(r"(?ms)^      - name: [^\n]*\n((?:        [^\n]*\n)+)", body):
                    if "--analyzer" not in step:
                        continue
                    analyzer_steps += 1
                    with self.subTest(workflow=name, job=job):
                        # The step itself or its whole job must be nightly-only.
                        self.assertTrue(condition in step or condition.replace("if: ", "") in header)
                        self.assertIn("continue-on-error: true", step)
            with self.subTest(workflow=name):
                self.assertTrue(analyzer_steps)
        self.assertRegex(root.joinpath("webgpu-probe.yml").read_text(), r"(?m)^  schedule:\n    - cron: '[^']+'$")
        workflow = root.joinpath("ci.yml").read_text()
        macos_compile = workflow.split("\n  macos-compile:\n", 1)[1].split("\n  macos-sanitizers:\n", 1)[0]
        inventory = macos_compile.split("--inventory", 1)
        self.assertEqual(len(inventory), 2)
        self.assertNotIn("continue-on-error", inventory[0].rsplit("- name:", 1)[1])
        self.assertNotIn("--inventory", workflow.split("\n  macos-analysis:\n", 1)[1].split("\n  formatting:\n", 1)[0])

    def test_builds_and_tidy_use_the_runner_core_counts(self):
        # Hosted runner CPUs for public repositories; a runner label not listed
        # here must be added with its core count before a job can use it.
        cores = {"ubuntu-24.04": "4", "macos-15": "3"}
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        workflow = root.joinpath("ci.yml").read_text()
        self.assertIn('\n  CMAKE_BUILD_PARALLEL_LEVEL: "4"\n', workflow.split("\njobs:\n", 1)[0])
        jobs = dict(re.findall(r"(?ms)^  ([a-z][a-z0-9-]*):\n(.*?)^    steps:", workflow.split("\njobs:\n", 1)[1]))
        for name, header in jobs.items():
            runner = re.search(r"(?m)^    runs-on: (\S+)$", header)
            if runner is None:
                continue
            level = re.search(r'(?m)^      CMAKE_BUILD_PARALLEL_LEVEL: "(\d+)"$', header)
            tidy = re.search(r'(?m)^      LUDUS_TIDY_JOBS: "(\d+)"$', header)
            with self.subTest(job=name, runner=runner.group(1)):
                self.assertIn(runner.group(1), cores)
                expected = cores[runner.group(1)]
                if name == "build-budget":
                    self.assertEqual(level.group(1), "2")
                elif expected == "4":
                    # The workflow default already matches Linux runners.
                    self.assertIsNone(level)
                else:
                    self.assertEqual(level.group(1) if level else None, expected)
                if tidy:
                    self.assertEqual(tidy.group(1), expected)
        for name in ("ci.yml", "webgpu-probe.yml"):
            with self.subTest(workflow=name):
                self.assertNotRegex(root.joinpath(name).read_text(), r"--parallel \d")

    def test_full_validation_is_owned_by_the_batch_scheduler(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        for path in root.glob("*.yml"):
            with self.subTest(workflow=path.name):
                # Promotion alone updates main; no merge queue or push validates it.
                workflow = path.read_text()
                self.assertNotRegex(workflow.split("\npermissions:", 1)[0], r"(?m)^  (push|merge_group):")
                self.assertNotIn("merge_group", workflow)
        for name in ("ci.yml", "wiki.yml", "project-sdk.yml"):
            triggers = root.joinpath(name).read_text().split("\npermissions:", 1)[0]
            self.assertNotIn("labeled", triggers)
            self.assertIn("  workflow_call:", triggers)
        batch = root.joinpath("batch-ci.yml").read_text()
        self.assertIn("cron: '13,43 * * * *'", batch)
        self.assertIn("  cancel-in-progress: false", batch)

    def test_integration_prs_run_only_the_source_and_tooling_native_job(self):
        event = {"pull_request": {"base": {"ref": "develop"}, "labels": [{"name": "ci:full"}]}}
        self.assertEqual(ci_scope.validation_tier("pull_request", event), ci_scope.INTEGRATION_TIER)
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$", workflow, re.MULTILINE))
        for name in ci_scope.NATIVE_JOBS:
            condition = conditions.get(name, "")
            self.assertEqual("validation_tier" in condition, name != "formatting", name)
        results = self.results(tier=ci_scope.INTEGRATION_TIER)
        self.assertEqual(ci_scope.gate_failures(results), [])
        for name in ci_scope.INTEGRATION_JOBS:
            self.assertEqual(results[name]["result"], "skipped", name)
            results[name]["result"] = "success"
            self.assertTrue(ci_scope.gate_failures(results), name)
            results[name]["result"] = "skipped"
        # Source/tooling validation remains required before accepting engine changes.
        for status in ("skipped", "failure", "cancelled"):
            results["formatting"]["result"] = status
            self.assertTrue(ci_scope.gate_failures(results), status)
        results["formatting"]["result"] = "success"
        self.assertEqual(ci_scope.gate_failures(results), [])
        self.assertEqual(ci_scope.gate_failures(self.results(False, ci_scope.INTEGRATION_TIER)), [])

    def test_pull_requests_leave_macos_tooling_jobs_to_the_batch(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        wiki = root.joinpath("wiki.yml").read_text().split("\n  macos-documentation-tools:\n", 1)[1]
        self.assertIn("    if: github.event_name != 'pull_request'\n", wiki.split("\n    steps:", 1)[0])
        butler = root.joinpath("project-sdk.yml").read_text().split("\n  butler-install:\n", 1)[1]
        runners = re.search(r"runner: \$\{\{ fromJSON\(github.event_name == 'pull_request' && '(\[.*?\])' \|\| '(\[.*?\])'\) \}\}",
                            butler)
        self.assertIsNotNone(runners)
        self.assertEqual(json.loads(runners.group(1)), ["ubuntu-24.04"])
        self.assertEqual(json.loads(runners.group(2)),
                         ["ubuntu-24.04", "macos-15", "macos-15-intel"])

    def results(self, native_required=True, tier=ci_scope.FULL_TIER):
        def expected(name):
            deferred = ((tier == ci_scope.INTEGRATION_TIER and name in ci_scope.INTEGRATION_JOBS)
                        or (tier != ci_scope.NIGHTLY_TIER and name in ci_scope.NIGHTLY_JOBS))
            return "success" if native_required and not deferred else "skipped"
        return {
            "changes": {"result": "success", "outputs": {
                "native_required": "true" if native_required else "false", "validation_tier": tier}},
            **{name: {"result": expected(name)} for name in ci_scope.NATIVE_JOBS},
        }

    def test_all_native_successes_or_intentional_docs_skips_pass(self):
        self.assertEqual(ci_scope.gate_failures(self.results()), [])
        self.assertEqual(ci_scope.gate_failures(self.results(False)), [])

    def test_failed_cancelled_or_skipped_native_jobs_fail_full_validation(self):
        for name in ci_scope.NATIVE_JOBS:
            statuses = (("failure", "cancelled", "success")
                        if name in ci_scope.NIGHTLY_JOBS
                        else ("failure", "cancelled", "skipped"))
            for status in statuses:
                with self.subTest(name=name, status=status):
                    results = self.results()
                    results[name]["result"] = status
                    self.assertTrue(ci_scope.gate_failures(results))

    def test_nightly_requires_every_native_job_and_retargeting_runs_cheap_checks(self):
        results = self.results(tier=ci_scope.NIGHTLY_TIER)
        self.assertEqual(ci_scope.gate_failures(results), [])
        for name in ci_scope.NATIVE_JOBS:
            results[name]["result"] = "skipped"
            self.assertTrue(ci_scope.gate_failures(results), name)
            results[name]["result"] = "success"
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        for name in ("ci.yml", "wiki.yml", "project-sdk.yml"):
            triggers = root.joinpath(name).read_text().split("\npermissions:", 1)[0]
            self.assertRegex(triggers, r"types: \[[^\]\n]*edited")

    def test_missing_invalid_or_retired_tier_cannot_skip_batch_jobs(self):
        for tier in (None, "", "Full", "fast", "pull-request"):
            with self.subTest(tier=tier):
                results = self.results(tier=ci_scope.INTEGRATION_TIER)
                results["changes"]["outputs"]["validation_tier"] = tier
                self.assertTrue(ci_scope.gate_failures(results))

    def test_docs_exemption_does_not_hide_failed_cancelled_or_executed_jobs(self):
        for status in ("failure", "cancelled", "success"):
            results = self.results(False)
            results["formatting"]["result"] = status
            self.assertTrue(ci_scope.gate_failures(results))

    def test_failed_or_missing_classifier_cannot_bypass_required_check(self):
        for status in ("failure", "cancelled", "skipped", None):
            results = self.results(False)
            results["changes"]["result"] = status
            self.assertTrue(ci_scope.gate_failures(results))
        results = self.results(False)
        del results["changes"]
        self.assertTrue(ci_scope.gate_failures(results))
        for output in (None, "", "False", "invalid"):
            results = self.results(False)
            results["changes"]["outputs"]["native_required"] = output
            self.assertTrue(ci_scope.gate_failures(results))

    def test_batch_editor_retains_all_deferred_authoring_coverage(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        editor = workflow.split("\n  editor:\n", 1)[1].split("\n  # Debug runtime", 1)[0]
        self.assertIn("-DLUDUS_EDITOR_RENDER_PREVIEW=ON", editor)
        self.assertIn("-DLUDUS_BUILD_SHADER_WORKER=ON", editor)
        # A target-limited Editor build misses worker/client/protocol targets.
        build = next(line.strip() for line in editor.splitlines() if line.strip().startswith("run:")
                     and "cmake --build" in line)
        self.assertEqual(build, "run: out/host-tools/venv/bin/cmake --build --preset linux-clang-development")
        self.assertIn("--label-regex '^(unit|editor)$' --no-tests=error", editor)
        self.assertIn("--tests-regex '^ludus_target_detection$' --output-on-failure --no-tests=error", editor)
        self.assertEqual(editor.count('--label-regex'), 1, 'Tests carrying both labels must run once.')
        self.assertIn('QT_QPA_PLATFORM: offscreen', editor)
        self.assertIn("'[.script-journey]'", editor)

    def test_batch_identity_overrides_pr_routing_and_requires_compiled_jobs(self):
        event = {"pull_request": {"base": {"ref": "develop"}, "labels": []}}
        tier = ci_scope.validation_tier("pull_request", event, is_batch=True)
        self.assertEqual(tier, ci_scope.FULL_TIER)
        results = self.results(tier=tier)
        for name in ci_scope.INTEGRATION_JOBS:
            if name in ci_scope.NIGHTLY_JOBS:
                continue
            with self.subTest(job=name):
                results[name]["result"] = "skipped"
                self.assertTrue(ci_scope.gate_failures(results))
                results[name]["result"] = "success"
        self.assertEqual(ci_scope.gate_failures(results), [])

    def test_missing_or_unexpected_native_jobs_fail(self):
        results = self.results()
        del results["sanitizers"]
        self.assertTrue(ci_scope.gate_failures(results))
        results = self.results(False)
        results["unknown"] = {"result": "skipped"}
        self.assertTrue(ci_scope.gate_failures(results))
        # The retired PR-only compile is no longer an accepted job.
        results = self.results()
        results["integration-compile"] = {"result": "skipped"}
        self.assertTrue(ci_scope.gate_failures(results))



class CancelledRunTests(unittest.TestCase):
    """A superseded duplicate must not block a PR; an unreplaced cancellation still fails."""

    SHA = "b062a93c" + "0" * 32
    CURRENT = {"id": 487, "workflow_id": 7, "event": "pull_request", "head_sha": SHA, "conclusion": None}

    def cancelled_results(self):
        results = RequiredGateTests().results(tier=ci_scope.INTEGRATION_TIER)
        results["changes"] = {"result": "cancelled", "outputs": {}}
        for name in ci_scope.NATIVE_JOBS:
            results[name]["result"] = "cancelled"
        return results

    def test_only_a_newer_uncancelled_run_of_the_same_commit_supersedes(self):
        newer = dict(self.CURRENT, id=825, conclusion="success")
        running = dict(self.CURRENT, id=826, conclusion=None)
        self.assertEqual(ci_scope.superseding_run(self.CURRENT, [newer]), newer)
        self.assertEqual(ci_scope.superseding_run(self.CURRENT, [running]), running)
        for other in (dict(newer, id=400), dict(newer, id=487), dict(newer, conclusion="cancelled"),
                      dict(newer, head_sha="1" * 40), dict(newer, workflow_id=8), dict(newer, event="merge_group"),
                      dict(newer, id="825"), "825", None):
            with self.subTest(run=other):
                self.assertIsNone(ci_scope.superseding_run(self.CURRENT, [other]))
        self.assertIsNone(ci_scope.superseding_run(self.CURRENT, []))

    def test_superseded_cancelled_run_passes_and_names_the_owning_run(self):
        with mock.patch.object(ci_scope, "find_superseding_run", return_value={"id": 825}), \
                mock.patch("sys.stdout") as stdout:
            self.assertEqual(ci_scope.run_gate(self.cancelled_results()), 0)
        self.assertIn("superseded by run 825", "".join(call.args[0] for call in stdout.write.call_args_list))

    def test_unreplaced_cancellation_still_fails(self):
        with mock.patch.object(ci_scope, "find_superseding_run", return_value=None), \
                mock.patch("sys.stderr") as stderr:
            self.assertEqual(ci_scope.run_gate(self.cancelled_results()), 1)
        output = "".join(call.args[0] for call in stderr.write.call_args_list)
        self.assertIn("Change classification did not succeed.", output)
        self.assertIn("no newer run of this workflow covers this commit", output)

    def test_genuine_failures_never_consult_replacement_runs(self):
        failed_classifier = RequiredGateTests().results(False)
        failed_classifier["changes"]["result"] = "failure"
        failed_job = RequiredGateTests().results()
        failed_job["formatting"]["result"] = "failure"
        unexpected_skip = RequiredGateTests().results()
        unexpected_skip["development"]["result"] = "skipped"
        for results in (failed_classifier, failed_job, unexpected_skip):
            with self.subTest(results=results), \
                    mock.patch.object(ci_scope, "find_superseding_run", return_value={"id": 825}) as lookup, \
                    mock.patch("sys.stderr"):
                self.assertEqual(ci_scope.run_gate(results), 1)
                lookup.assert_not_called()

    def test_successful_gate_never_queries_github(self):
        with mock.patch.object(ci_scope, "find_superseding_run") as lookup, mock.patch("sys.stdout"):
            self.assertEqual(ci_scope.run_gate(RequiredGateTests().results()), 0)
        lookup.assert_not_called()

    def test_lookup_is_limited_to_pull_requests_and_fails_closed(self):
        environment = {"GITHUB_EVENT_NAME": "pull_request", "GITHUB_RUN_ID": "487",
                       "GITHUB_REPOSITORY": "The-Ludus-Project/Ludus", "GITHUB_TOKEN": "token"}
        listing = {"workflow_runs": [dict(self.CURRENT, id=825, conclusion="success"), self.CURRENT]}

        def read(path):
            return dict(self.CURRENT) if path == "actions/runs/487" else listing

        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(ci_scope, "read_github", side_effect=read) as reader:
            self.assertEqual(ci_scope.find_superseding_run()["id"], 825)
            self.assertIn(f"head_sha={self.SHA}&event=pull_request", reader.call_args_list[1].args[0])
        for overrides in ({"GITHUB_EVENT_NAME": "merge_group"}, {"GITHUB_EVENT_NAME": "schedule"},
                          {"LUDUS_CI_BATCH_SHA": self.SHA}, {"GITHUB_RUN_ID": "not-a-run"}):
            with self.subTest(overrides=overrides), mock.patch.dict(os.environ, {**environment, **overrides}, clear=True), \
                    mock.patch.object(ci_scope, "read_github", side_effect=read):
                self.assertIsNone(ci_scope.find_superseding_run())
        for current in (dict(self.CURRENT, event="merge_group"), dict(self.CURRENT, head_sha="main"),
                        dict(self.CURRENT, workflow_id=None)):
            with self.subTest(current=current), mock.patch.dict(os.environ, environment, clear=True), \
                    mock.patch.object(ci_scope, "read_github", side_effect=[current, listing]):
                self.assertIsNone(ci_scope.find_superseding_run())
        for error in (urllib.error.URLError("offline"), ValueError("bounded"), KeyError("GITHUB_TOKEN")):
            with self.subTest(error=error), mock.patch.dict(os.environ, environment, clear=True), \
                    mock.patch.object(ci_scope, "read_github", side_effect=error):
                self.assertIsNone(ci_scope.find_superseding_run())

    def test_gate_job_can_read_runs_and_receives_a_token(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        gate = workflow.split("\n  linux:\n", 1)[1]
        header = gate.split("\n    steps:", 1)[0]
        self.assertIn("    if: always()\n", header)
        self.assertIn("    permissions:\n      contents: read\n      actions: read", header)
        self.assertIn("          GITHUB_TOKEN: ${{ github.token }}\n        run: python3 scripts/python/ci_scope.py gate",
                      gate)

    def test_only_pushes_cancel_required_pull_request_workflows(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        for name, prefix in (("ci.yml", "ci"), ("wiki.yml", "wiki"), ("project-sdk.yml", "project-sdk")):
            workflow = root.joinpath(name).read_text()
            with self.subTest(workflow=name):
                # Same-commit activity keeps full validation in its own non-cancelling group.
                self.assertRegex(workflow.split("\npermissions:", 1)[0],
                                 r"types: \[[^\]\n]*synchronize[^\]\n]*ready_for_review")
                self.assertIn(
                    "\nconcurrency:\n"
                    f"  group: {prefix}-${{{{ inputs.batch_sha || github.ref }}}}"
                    "${{ github.event_name == 'pull_request' && github.event.action != 'synchronize' && "
                    "format('-head-{0}', github.event.pull_request.head.sha) || '' }}\n"
                    "  cancel-in-progress: ${{ github.event_name == 'pull_request' && "
                    "github.event.action == 'synchronize' }}\n",
                    workflow)


if __name__ == "__main__":
    unittest.main()
