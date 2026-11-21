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
import pull_request_tests


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

    def test_manual_full_qualification_matches_the_batch_without_weakening_other_events(self):
        event = {"inputs": {"validation_tier": "full"}}
        self.assertEqual(ci_scope.validation_tier("workflow_dispatch", event), ci_scope.FULL_TIER)
        self.assertEqual(ci_scope.validation_tier("workflow_dispatch", event, is_batch=True), ci_scope.FULL_TIER)
        self.assertEqual(ci_scope.validation_tier("schedule", event), ci_scope.NIGHTLY_TIER)
        event["pull_request"] = {"base": {"ref": "develop"}}
        self.assertEqual(ci_scope.validation_tier("pull_request", event), ci_scope.INTEGRATION_TIER)
        for inputs in (None, [], "full", {"validation_tier": "integration"}, {"validation_tier": True}):
            with self.subTest(inputs=inputs):
                self.assertEqual(ci_scope.validation_tier("workflow_dispatch", {"inputs": inputs}),
                                 ci_scope.NIGHTLY_TIER)

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

    def test_camera_pilot_routes_header_only_and_unrelated_documentation_diffs(self):
        self.write("modules/graphics/renderer/include/ludus/graphics/renderer/render_graph.hpp", "camera")
        header = self.commit()
        self.write("docs/index.md", "Unrelated documentation")
        docs = self.commit()
        self.assertTrue(ci_scope.classify("pull_request", self.pull_request(header), self.root)[2])
        self.assertFalse(ci_scope.classify("pull_request", self.pull_request(docs, header), self.root)[2])

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

    def test_objective_cxx_routing_counts_present_mm_sources_and_editor_ones(self):
        self.write("modules/platform/tests/cocoa_tests.mm", "Original Cocoa")
        self.write("apps/editor/src/crash_capture_macos.mm", "Original Editor")
        self.base = self.commit()
        self.assertEqual(ci_scope.classify_objcxx("pull_request", self.pull_request(self.commit_engine()), self.root),
                         (False, False))
        self.write("modules/platform/tests/cocoa_tests.mm", "Changed Cocoa")
        engine_only = self.commit()
        self.assertEqual(ci_scope.classify_objcxx("pull_request", self.pull_request(engine_only), self.root),
                         (True, False))
        self.write("apps/editor/src/crash_capture_macos.mm", "Changed Editor")
        editor = self.commit()
        self.assertEqual(ci_scope.classify_objcxx("pull_request", self.pull_request(editor), self.root), (True, True))
        # Deleting a source leaves nothing to analyze.
        self.git("rm", "-q", "modules/platform/tests/cocoa_tests.mm", "apps/editor/src/crash_capture_macos.mm")
        deleted = self.commit()
        self.assertEqual(ci_scope.classify_objcxx("pull_request", self.pull_request(deleted, editor), self.root),
                         (False, False))
        # Only pull requests route the job; an unavailable comparison runs it rather than skip it.
        self.assertEqual(ci_scope.classify_objcxx("workflow_dispatch", {}, self.root), (False, False))
        self.assertEqual(ci_scope.classify_objcxx("pull_request", self.pull_request(editor, "a" * 40), self.root),
                         (True, True))

    def commit_engine(self):
        self.write("modules/engine.cpp", "Changed engine only")
        return self.commit()

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
            ("workflow_dispatch", {"inputs": {"validation_tier": "full"}}, "", "true", ci_scope.FULL_TIER),
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
                self.assertEqual(output.read_text(),
                                 f"camera_pilot=false\nnative_required={required}\nvalidation_tier={tier}\n"
                                 "objcxx_changed=false\nobjcxx_editor=false\n")
                self.assertIn(f"Native validation required: {required}. Validation tier: {tier}.", summary.read_text())
        # An integration PR changing Objective-C++ selects the macOS job; a full-tier PR leaves it to the batch.
        self.write("modules/platform/src/window_cocoa.mm", "Changed Cocoa")
        head = self.commit()
        for base_ref, selected in (("develop", "true"), ("main", "false")):
            with self.subTest(base_ref=base_ref):
                event.write_text(json.dumps(self.pull_request(head, base_ref=base_ref)), encoding="utf-8")
                output.unlink(missing_ok=True)
                subprocess.run(
                    [sys.executable, ci_scope.__file__, "classify"], cwd=self.root, check=True, capture_output=True,
                    env={**os.environ, "GITHUB_EVENT_NAME": "pull_request", "GITHUB_EVENT_PATH": str(event),
                         "GITHUB_OUTPUT": str(output), "GITHUB_STEP_SUMMARY": str(summary), "LUDUS_CI_BATCH_SHA": ""},
                )
                self.assertIn(f"objcxx_changed={selected}\nobjcxx_editor=false\n", output.read_text())


class RequiredGateTests(unittest.TestCase):
    def test_workflow_gate_requires_every_classified_native_job(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        gate = workflow.split("\n  linux:", 1)[1]
        dependencies = re.search(r"needs: \[([^\]]+)\]", gate)
        self.assertIsNotNone(dependencies)
        self.assertCountEqual([name.strip() for name in dependencies.group(1).split(",")],
                              ["changes", *ci_scope.NATIVE_JOBS, ci_scope.OBJCXX_JOB])
        # A missing workflow job must not silently remove validation while the
        # Python gate still expects it, including docs-only routing.
        jobs = set(re.findall(r"^  ([a-z][a-z0-9-]*):$", workflow, re.MULTILINE))
        self.assertTrue(set(ci_scope.NATIVE_JOBS) <= jobs)
        self.assertIn(ci_scope.OBJCXX_JOB, jobs)

    def test_objective_cxx_job_runs_only_for_integration_prs_that_change_mm_sources(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        job = workflow.split(f"\n  {ci_scope.OBJCXX_JOB}:\n", 1)[1].split("\n  macos-analysis:\n", 1)[0]
        self.assertIn("    needs: changes\n", job)
        self.assertIn("    if: needs.changes.outputs.native_required == 'true' && needs.changes.outputs.validation_tier"
                      " == 'integration' && needs.changes.outputs.objcxx_changed == 'true'\n", job)
        self.assertIn("    runs-on: macos-15\n", job)
        self.assertIn("editor: ${{ needs.changes.outputs.objcxx_editor }}", job)
        self.assertIn("./scripts/check macos-clang-development --tidy-changed-objcxx --base", job)
        self.assertNotIn("actions/cache/save", job)
        for output in ("objcxx_changed", "objcxx_editor"):
            self.assertIn(f"      {output}: ${{{{ steps.scope.outputs.{output} }}}}\n", workflow)

    def test_main_workflow_without_the_objective_cxx_job_still_gates_the_batch(self):
        # Batch 44859003421 ran main's ci.yml, which predates the Objective-C++ job: every suite
        # passed, but the candidate's gate rejected the missing objcxx_changed output.
        for tier in (ci_scope.FULL_TIER, ci_scope.NIGHTLY_TIER):
            with self.subTest(tier=tier):
                results = self.results(True, tier)
                del results[ci_scope.OBJCXX_JOB]
                del results["changes"]["outputs"]["objcxx_changed"]
                self.assertEqual(ci_scope.gate_failures(results), [])
                results["macos-compile"]["result"] = "failure"
                self.assertIn("macos-compile: expected success", " ".join(ci_scope.gate_failures(results)))
                del results["macos-compile"]
                self.assertIn("inventory", " ".join(ci_scope.gate_failures(results)))

    def test_objective_cxx_job_is_required_exactly_when_selected(self):
        for tier in sorted(ci_scope.TIERS):
            for required in (True, False):
                for objcxx in ("true", "false"):
                    with self.subTest(tier=tier, required=required, objcxx=objcxx):
                        results = self.results(required, tier, objcxx=objcxx)
                        self.assertEqual(ci_scope.gate_failures(results), [])
                        expected = results[ci_scope.OBJCXX_JOB]["result"]
                        self.assertEqual(expected == "success",
                                         required and tier == ci_scope.INTEGRATION_TIER and objcxx == "true")
                        for status in ("success", "failure", "cancelled", "skipped"):
                            if status != expected:
                                results[ci_scope.OBJCXX_JOB]["result"] = status
                                self.assertIn(f"{ci_scope.OBJCXX_JOB}: expected {expected}",
                                              " ".join(ci_scope.gate_failures(results)))
        for value in (None, "", "True", "yes"):
            results = self.results(tier=ci_scope.INTEGRATION_TIER)
            results["changes"]["outputs"]["objcxx_changed"] = value
            self.assertTrue(ci_scope.gate_failures(results))
        results = self.results(tier=ci_scope.INTEGRATION_TIER)
        del results[ci_scope.OBJCXX_JOB]
        self.assertTrue(ci_scope.gate_failures(results))

    def test_native_builds_do_not_wait_for_source_policy(self):
        # A source-policy failure must not skip every compile in a batch: the
        # builds report their own results while the gate still requires the
        # source/tooling job to succeed before promotion.
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        dependencies = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    needs: (.*)$", workflow, re.MULTILINE))
        # Native acceptance runs beside the sanitizers; the nightly analyzer waits for
        # the sanitizer lane so at most two ARM runners are held, without depending on
        # formatting or inheriting the previous lane's success requirement.
        serial = {"macos-analysis": "[changes, macos-sanitizers]"}
        for name in ci_scope.NATIVE_JOBS:
            with self.subTest(job=name):
                self.assertEqual(dependencies.get(name),
                                 "[changes, sanitizer-build]" if name == "sanitizers" else serial.get(name, "changes"))
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$", workflow, re.MULTILINE))
        for name in serial:
            self.assertTrue(conditions[name].startswith("${{ !cancelled() && "), name)
        results = self.results(tier=ci_scope.FULL_TIER)
        results["formatting"]["result"] = "failure"
        self.assertIn("formatting: expected success, received failure.", ci_scope.gate_failures(results))

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
        # The macOS native job runs its check plan; a failed check fails the job.
        plan = (root.parent.parent / "scripts/ci/macos_native.py").read_text()
        self.assertEqual(plan.count("--inventory"), 1)
        self.assertIn("scripts/python/ci_checks.py scripts/ci/macos_native.py", macos_compile)
        self.assertNotIn("continue-on-error", macos_compile)
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

    def test_linux_sanitizer_suite_is_sharded_across_runners(self):
        # The serial sanitizer suite took 95-100 min of a 2 h job. Shards split it
        # across runners; each case still runs alone on its runner, so lavapipe
        # cold-shader deadlines keep the CPU they were calibrated against.
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        sanitizers = workflow.split("\n  sanitizers:\n", 1)[1].split("\n  scripting-qualification:\n", 1)[0]
        header = sanitizers.split("\n    steps:\n", 1)[0]
        shards = re.search(r"(?m)^        shard: \[([^\]]+)\]$", header)
        self.assertIsNotNone(shards, header)
        count = len(shards.group(1).split(", "))
        self.assertGreater(count, 1)
        self.assertEqual(shards.group(1).split(", "), [str(index) for index in range(1, count + 1)])
        self.assertIn("      fail-fast: false\n", header)
        self.assertIn("      SANITIZER_SHARD: ${{ matrix.shard }}\n", header)
        self.assertTrue(header.endswith("      SANITIZER_SHARDS: ${{ strategy.job-total }}"), header)
        # native-setup selects the Mesa correctness profile by GITHUB_JOB, the job ID
        # every shard shares.
        self.assertIn("Linux:sanitizers:linux-clang-asan-ubsan",
                      (Path(__file__).resolve().parents[2] / ".github/actions/native-setup/action.yml").read_text())

        steps = re.split(r"(?m)^      - ", sanitizers.split("\n    steps:\n", 1)[1])
        suite = [step for step in steps if step.startswith("name: Test with sanitizers\n")]
        self.assertEqual(len(suite), 1)
        command = " ".join(suite[0].split())
        self.assertIn("ctest --preset linux-clang-asan-ubsan", command)
        self.assertIn('--tests-from-file "$RUNNER_TEMP/sanitizer-shard-tests.txt"', command)
        self.assertNotIn("--parallel", command)
        self.assertNotIn("if:", suite[0])
        # ctest numbers the tests of the tree it runs in. Shard 1's probe steps
        # reconfigure that tree and register more tests, so a --tests-information
        # stride there ran 87 tests twice and skipped as many. Every shard selects
        # its names before any step that differs between shards could change the tree.
        self.assertNotIn("--tests-information", sanitizers)
        self.assertNotRegex(sanitizers, r"ctest[^\n]* -I ")
        selection = [index for index, step in enumerate(steps) if step.startswith("name: Select this shard's tests\n")]
        self.assertEqual(len(selection), 1)
        select = " ".join(steps[selection[0]].split())
        self.assertNotIn("if:", steps[selection[0]])
        self.assertIn("scripts/python/ctest_shard.py out/build/linux-clang-asan-ubsan", select)
        self.assertIn('--shard "$SANITIZER_SHARD" --shards "$SANITIZER_SHARDS"', select)
        self.assertIn('--output "$RUNNER_TEMP/sanitizer-shard-tests.txt"', select)
        self.assertLess(selection[0], steps.index(suite[0]))
        for step in steps[1 : selection[0]]:
            with self.subTest(before_selection=step.splitlines()[0]):
                condition = re.search(r"(?m)^        if: (.*)$", step)
                if condition is not None and "matrix.shard" in condition.group(1):
                    # Uploads read the tree; anything else could change the listing.
                    self.assertIn("uses: actions/upload-artifact@", step)
        first_shard_specific = next(
            index for index, step in enumerate(steps)
            if re.search(r"(?m)^        if: .*matrix\.shard", step) and "actions/upload-artifact@" not in step
        )
        self.assertLess(selection[0], first_shard_specific)
        # Required reload evidence comes from one fixed shard, never from the round-robin.
        self.assertIn("--exclude-regex '^ludus_game_host_reload_tests$'", select)
        reload = [step for step in steps if "LUDUS_RELOAD_METRICS" in step]
        self.assertEqual(len(reload), 1)
        self.assertIn("--tests-regex '^ludus_game_host_reload_tests$'", reload[0])
        self.assertIn("if: matrix.shard == 1\n", reload[0])
        # Artifact names are unique per workflow run, and one shard writes each cache.
        for step in steps:
            if "actions/upload-artifact@" in step or "actions/cache@" in step:
                with self.subTest(step=step.splitlines()[0]):
                    self.assertRegex(step, r"(?m)^        if: (always\(\) && )?matrix\.shard == 1$")
        for writer in ("conan-cache-writer", "compiler-cache-writer"):
            self.assertIn(f"          {writer}: ${{{{ matrix.shard == 1 && 'true' || 'false' }}}}\n", sanitizers)

    def test_shared_sanitizer_build_is_manual_and_reuses_the_required_shard_steps(self):
        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/ci.yml").read_text()
        build, shards = workflow.split("\n  sanitizer-build:\n", 1)[1].split("\n  sanitizers:\n", 1)
        shards = shards.split("\n  scripting-qualification:\n", 1)[0]
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.sanitizer_experiment == 'shared'", build)
        self.assertIn("run: ./scripts/build linux-clang-asan-ubsan", build)
        self.assertIn("id: bundle\n        shell: bash", build)
        self.assertIn("if-no-files-found: error", build)
        self.assertIn("needs.sanitizer-build.result == 'success'", shards)
        self.assertIn("name: ${{ needs.sanitizer-build.outputs.artifact }}", shards)
        self.assertIn("prerequisites-only: 'true'", shards)
        self.assertIn("git diff --exit-code", shards)
        self.assertIn("if: inputs.sanitizer_experiment != 'shared'\n"
                      "        run: ./scripts/build linux-clang-asan-ubsan", shards)
        self.assertNotIn("if:", shards.split("- name: Prove ASan detection", 1)[1].split("- name:", 1)[0])
        self.assertIn("sanitizers", ci_scope.NATIVE_JOBS)

    def test_full_validation_is_owned_by_the_batch_scheduler(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        for path in root.glob("*.yml"):
            with self.subTest(workflow=path.name):
                # Promotion alone updates main; no merge queue or push validates it.
                workflow = path.read_text()
                triggers = workflow.split("\npermissions:", 1)[0]
                self.assertNotIn("merge_group", workflow)
                if path.name == "develop-cache.yml":
                    # Only the cache warmer reacts to develop pushes.
                    self.assertIn("\n  push:\n    branches: [develop]\n", triggers)
                    self.assertNotRegex(triggers, r"(?m)^  (pull_request|pull_request_target|merge_group|workflow_call):")
                    continue
                self.assertNotRegex(triggers, r"(?m)^  (push|merge_group):")
        for name in ("ci.yml", "wiki.yml", "project-sdk.yml"):
            triggers = root.joinpath(name).read_text().split("\npermissions:", 1)[0]
            self.assertNotIn("labeled", triggers)
            self.assertIn("  workflow_call:", triggers)
        batch = root.joinpath("batch-ci.yml").read_text()
        self.assertIn("cron: '13,43 * * * *'", batch)
        self.assertIn("  cancel-in-progress: false", batch)

    def test_batch_admits_arm_workflows_without_hiding_failed_evidence(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/batch-ci.yml").read_text()
        dependencies = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    needs: (.*)$",
                                       workflow, re.MULTILINE))
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$",
                                     workflow, re.MULTILINE))
        # These are all reusable workflows with ARM jobs. Native (at most two ARM runners)
        # starts at capture beside the short chain, which runs one workflow at a time, so
        # the batch asks for at most three ARM runners and no short job waits for native.
        self.assertEqual(dependencies["native"], "snapshot")
        self.assertEqual(dependencies["threading"], "[snapshot]")
        for previous, current in (("threading", "sdk"), ("sdk", "wiki-macos")):
            with self.subTest(workflow=current):
                self.assertEqual(dependencies[current], f"[snapshot, {previous}]")
        for name in ("threading", "sdk", "wiki-macos"):
            with self.subTest(workflow=name):
                self.assertEqual(conditions[name],
                                 "${{ !cancelled() && needs.snapshot.outputs.should_run == 'true' }}")
        # Other platforms retain parallel qualification, and promotion still
        # requires every result, including a failed earlier ARM workflow.
        for name in ("browser", "windows", "recovery", "wiki", "wiki-macos-intel", "threading-windows"):
            self.assertEqual(dependencies[name], "snapshot")
        self.assertEqual(dependencies["promote"],
                         "[snapshot, native, sdk, wiki, wiki-macos, wiki-macos-intel, browser, "
                         "threading, threading-windows, windows, recovery]")
        self.assertIn("needs.threading-windows.result == 'success'", workflow)
        self.assertRegex(workflow, r"(?m)^      batch_platform: macos\n\n  threading-windows:\n")
        threading = (Path(__file__).resolve().parents[2] / ".github/workflows/threading-platforms.yml").read_text()
        self.assertIn("  windows:\n    if: inputs.batch_platform != 'macos'\n", threading)
        self.assertIn("  macos-races:\n    if: inputs.batch_platform != 'windows'\n", threading)

    def test_integration_prs_check_sources_compile_and_analyze(self):
        event = {"pull_request": {"base": {"ref": "develop"}, "labels": [{"name": "ci:full"}]}}
        self.assertEqual(ci_scope.validation_tier("pull_request", event), ci_scope.INTEGRATION_TIER)
        self.assertCountEqual(ci_scope.PULL_REQUEST_JOBS, ("formatting", "analysis", "editor"))
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$", workflow, re.MULTILINE))
        for name in ci_scope.NATIVE_JOBS:
            condition = conditions.get(name, "")
            self.assertEqual("validation_tier" in condition, name not in ci_scope.PULL_REQUEST_JOBS, name)
        results = self.results(tier=ci_scope.INTEGRATION_TIER)
        self.assertEqual(ci_scope.gate_failures(results), [])
        for name in ci_scope.INTEGRATION_JOBS:
            self.assertEqual(results[name]["result"], "skipped", name)
            results[name]["result"] = "success"
            self.assertTrue(ci_scope.gate_failures(results), name)
            results[name]["result"] = "skipped"
        # Source checks, the Editor compile and analysis are required before
        # accepting engine changes, so a build break cannot fail every later batch.
        for name in ci_scope.PULL_REQUEST_JOBS:
            for status in ("skipped", "failure", "cancelled"):
                with self.subTest(job=name, status=status):
                    results[name]["result"] = status
                    self.assertTrue(ci_scope.gate_failures(results))
            results[name]["result"] = "success"
        self.assertEqual(ci_scope.gate_failures(results), [])

    def test_integration_editor_runs_bounded_checks_without_cache_writes(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        editor = workflow.split("\n  editor:\n", 1)[1].split("\n  # Debug runtime", 1)[0]
        self.assertIn("compiler-cache-writer: ${{ needs.changes.outputs.validation_tier == 'integration' "
                      "&& 'false' || 'true' }}", editor)
        self.assertIn("conan-cache-writer: 'false'", editor)
        guard = "        if: needs.changes.outputs.validation_tier != 'integration'\n"
        steps = re.findall(r"(?ms)^      - (?:name|uses): ([^\n]*)\n(.*?)(?=^      - |\Z)", editor)
        self.assertTrue(steps)
        compile_only = ("Configure the shader worker with verified authoring tools", "Check that each source compiles once",
                        "Build every default target with the Editor and shader worker",
                        "Analyze document interactions and regressions", "Report compiler cache")
        # The one test step integration runs: quick generated-file comparisons on the built tree
        # (docs/development/continuous-integration.md#generated-sources).
        generated_sources = "Check checked-in generated sources"
        # And the checked list of sub-second unit and Editor tests the build already produced
        # (docs/development/continuous-integration.md#quick-pull-request-tests).
        quick_tests = "Run quick unit and Editor tests"
        camera_pilot = "Run advisory RenderGraph camera pilot"
        # Reads the batch suite's JUnit report even after a failure; never a test or a gate
        # (docs/development/continuous-integration.md#ctest-timeout-headroom).
        headroom = "Report CTest timeout headroom"
        titles = [title for title, _ in steps]
        self.assertIn(quick_tests, titles)
        self.assertIn(camera_pilot, titles)
        for title, body in steps:
            with self.subTest(step=title):
                runs_tests = "ctest" in body or "ludus_editor_tests" in body or "upload-artifact" in title
                if title == camera_pilot:
                    self.assertIn("validation_tier == 'integration' && needs.changes.outputs.camera_pilot == 'true'", body)
                    self.assertIn("continue-on-error: true", body)
                    self.assertIn("python3 scripts/python/camera_pilot.py", body)
                elif title == headroom:
                    self.assertIn("        if: always() && needs.changes.outputs.validation_tier != 'integration'\n", body)
                    self.assertIn("scripts/python/ctest_headroom.py", body)
                elif title == generated_sources:
                    self.assertNotIn(guard, body)
                    self.assertIn("--label-regex '^generated-source$'", body)
                elif title == quick_tests:
                    self.assertNotIn(guard, body)
                    self.assertIn("scripts/python/pull_request_tests.py out/build/linux-clang-development", body)
                    # CTest reads the list after changing into the build tree, so the path is absolute.
                    self.assertIn('--tests-from-file "$PWD/config/pull_request_tests.txt"', body)
                    self.assertIn(f"--timeout {pull_request_tests.MAX_TIMEOUT_SECONDS} ", body)
                    self.assertIn("QT_QPA_PLATFORM: offscreen", body)
                    # The list is checked before it runs, so a renamed test fails instead of vanishing.
                    self.assertLess(body.index("pull_request_tests.py"), body.index("--tests-from-file"))
                elif title in compile_only or title.startswith(("actions/checkout", "./.github/actions/native-setup")):
                    self.assertNotIn(guard, body)
                else:
                    self.assertTrue(runs_tests, title)
                    self.assertIn(guard, body)
        self.assertEqual(ci_scope.gate_failures(self.results(False, ci_scope.INTEGRATION_TIER)), [])

    def test_pull_requests_leave_macos_tooling_jobs_to_the_batch(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        wiki = root.joinpath("wiki.yml").read_text().split("\n  macos-documentation-tools:\n", 1)[1]
        self.assertIn("    if: github.event_name != 'pull_request' && (inputs.batch_sha == '' || inputs.batch_macos_tools_runner != '')\n",
                      wiki.split("\n    steps:", 1)[0])
        butler = root.joinpath("project-sdk.yml").read_text().split("\n  butler-install:\n", 1)[1]
        runners = re.search(r"runner: \$\{\{ fromJSON\(github.event_name == 'pull_request' && '(\[.*?\])' \|\| '(\[.*?\])'\) \}\}",
                            butler)
        self.assertIsNotNone(runners)
        self.assertEqual(json.loads(runners.group(1)), ["ubuntu-24.04"])
        self.assertEqual(json.loads(runners.group(2)),
                         ["ubuntu-24.04", "macos-15", "macos-15-intel"])

    def results(self, native_required=True, tier=ci_scope.FULL_TIER, objcxx="false"):
        def expected(name):
            deferred = ((tier == ci_scope.INTEGRATION_TIER and name in ci_scope.INTEGRATION_JOBS)
                        or (tier != ci_scope.NIGHTLY_TIER and name in ci_scope.NIGHTLY_JOBS))
            return "success" if native_required and not deferred else "skipped"
        selected = native_required and tier == ci_scope.INTEGRATION_TIER and objcxx == "true"
        return {
            "changes": {"result": "success", "outputs": {
                "native_required": "true" if native_required else "false", "validation_tier": tier,
                "objcxx_changed": objcxx}},
            **{name: {"result": expected(name)} for name in ci_scope.NATIVE_JOBS},
            ci_scope.OBJCXX_JOB: {"result": "success" if selected else "skipped"},
        }

    def test_all_native_successes_or_intentional_docs_skips_pass(self):
        self.assertEqual(ci_scope.gate_failures(self.results()), [])

    def test_scoped_evidence_runs_exactly_the_selected_native_job(self):
        results = self.results()
        for name in ci_scope.NATIVE_JOBS:
            if name not in ("sanitizers",):
                results[name]["result"] = "skipped"
        self.assertEqual(ci_scope.gate_failures(results, "sanitizers"), [])
        self.assertEqual(ci_scope.gate_failures(self.results(), ci_scope.ALL_EVIDENCE_JOBS), [])
        # Another native job that ran, or the selected job skipped, fails the scoped gate.
        extra = dict(results, formatting={"result": "success"})
        self.assertTrue(ci_scope.gate_failures(extra, "sanitizers"))
        missing = dict(results, sanitizers={"result": "skipped"})
        self.assertTrue(ci_scope.gate_failures(missing, "sanitizers"))
        self.assertIn("Unknown evidence job", ci_scope.gate_failures(results, "Ubuntu Clang")[0])
        # A job its tier defers proves nothing, even though every job was skipped.
        nightly_only = self.results()
        for name in ci_scope.NATIVE_JOBS:
            nightly_only[name]["result"] = "skipped"
        self.assertTrue(ci_scope.gate_failures(nightly_only, "macos-analysis"))
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

    def test_nightly_requires_every_native_job(self):
        results = self.results(tier=ci_scope.NIGHTLY_TIER)
        self.assertEqual(ci_scope.gate_failures(results), [])
        for name in ci_scope.NATIVE_JOBS:
            results[name]["result"] = "skipped"
            self.assertTrue(ci_scope.gate_failures(results), name)
            results[name]["result"] = "success"

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
        # Generated-source checks also carry unit; their own step runs them, so the batch skips them here.
        self.assertIn("--label-regex '^(unit|editor)$' --label-exclude '^generated-source$' "
                      '--exclude-from-file "$PWD/config/pull_request_tests.txt" --no-tests=error', editor)
        self.assertIn("--tests-regex '^ludus_target_detection$' --output-on-failure --no-tests=error", editor)
        self.assertEqual(editor.count("--label-regex '^(unit|editor)$'"), 1, 'Tests carrying both labels must run once.')
        # The quick tests every pull request runs are excluded here, so the batch runs each once.
        batch_run = next(line for line in editor.splitlines() if "--label-regex '^(unit|editor)$'" in line)
        self.assertIn('--exclude-from-file "$PWD/config/pull_request_tests.txt"', batch_run)
        self.assertEqual(editor.count('--tests-from-file "$PWD/config/pull_request_tests.txt"'), 1)
        self.assertEqual(editor.count('--label-regex'), 2, 'Only the generated-source step adds a label run.')
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
        self.assertIn("    if: always() && (github.event_name != 'workflow_dispatch' || (inputs.mcmc_vulkan_parity_only != true && inputs.sanitizer_experiment != 'rebuild' && inputs.sanitizer_experiment != 'shared'))\n", header)
        self.assertIn("    permissions:\n      contents: read\n      actions: read", header)
        self.assertIn("          GITHUB_TOKEN: ${{ github.token }}\n        run: python3 scripts/python/ci_scope.py gate",
                      gate)

    def test_focused_vulkan_mode_is_manual_and_cannot_replace_pull_request_or_batch_checks(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text()
        changes = workflow.split("\n  changes:\n", 1)[1].split("\n  mcmc-vulkan-parity:\n", 1)[0]
        self.assertIn("if: github.event_name != 'workflow_dispatch' || (inputs.mcmc_vulkan_parity_only != true", changes)
        focused = workflow.split("\n  mcmc-vulkan-parity:\n", 1)[1].split("\n  macos-compile:\n", 1)[0]
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.mcmc_vulkan_parity_only == true", focused)
        self.assertIn("ref: ${{ inputs.batch_sha || github.sha }}", focused)
        # The shared installer bounds and retries each download (test_ci.BoundedAptInstallTests).
        self.assertIn("bash .github/actions/apt-install/apt-install.sh libvulkan1", focused)
        self.assertIn("VK_INSTANCE_LAYERS: VK_LAYER_KHRONOS_validation", focused)
        self.assertIn("set -euo pipefail", focused)
        self.assertIn("--target ludus_metropolis_tests ludus_renderer_render_tests ludus_metropolis_graph_tests", focused)
        self.assertIn("assert 'skipped' not in text.lower()", focused)
        self.assertIn("'VUID-' not in text", focused)
        gate = workflow.split("\n  linux:\n", 1)[1].split("\n    steps:", 1)[0]
        self.assertIn("&& 'Focused Vulkan dispatch (not integration)' || inputs.evidence_job", gate)
        self.assertIn("|| 'Ubuntu Clang' }}", gate)

    def test_each_pull_request_has_one_run_per_workflow_and_a_push_supersedes_it(self):
        root = Path(__file__).resolve().parents[2] / ".github/workflows"
        for name, prefix in (("ci.yml", "ci"), ("wiki.yml", "wiki"), ("project-sdk.yml", "project-sdk")):
            workflow = root.joinpath(name).read_text()
            with self.subTest(workflow=name):
                self.assertIn("  pull_request:\n    types: [opened, synchronize, reopened]\n", workflow)
                self.assertIn(f"\nconcurrency:\n  group: {prefix}-${{{{ inputs.batch_sha || github.ref }}}}", workflow)
                self.assertNotIn("head.sha", workflow.split("\nconcurrency:\n", 1)[1].split("\n\n", 1)[0])
                self.assertIn("\n  cancel-in-progress: ${{ github.event_name == 'pull_request' }}\n", workflow)

class DevelopCompilerCacheTests(unittest.TestCase):
    """Develop's cache warmer must compile exactly like the pull-request Editor job."""

    ROOT = Path(__file__).resolve().parents[2]

    @staticmethod
    def job(workflow: str, name: str) -> str:
        return workflow.split(f"\n  {name}:\n", 1)[1].split("\n\n  ", 1)[0]

    @staticmethod
    def setup_inputs(job: str) -> dict:
        block = job.split("      - uses: ./.github/actions/native-setup\n        with:\n", 1)[1]
        block = block.split("\n      - ", 1)[0]
        return dict(re.findall(r"(?m)^          ([a-z-]+): (.*)$", block))

    @staticmethod
    def runs(job: str) -> list[str]:
        return [line.strip() for line in job.splitlines()
                if line.strip().startswith(("out/host-tools/venv/bin/cmake ", "./scripts/shader-probe ",
                                            "run: out/host-tools/venv/bin/cmake "))]

    def test_warm_job_compiles_with_the_pull_request_editor_inputs(self):
        ci = (self.ROOT / ".github/workflows/ci.yml").read_text()
        warm = (self.ROOT / ".github/workflows/develop-cache.yml").read_text()
        editor = self.job(ci, "editor")
        cache = self.job(warm, "editor")
        pull_request = self.setup_inputs(editor)
        develop = self.setup_inputs(cache)
        writers = {"conan-cache-writer", "compiler-cache-writer"}
        self.assertEqual({key: value for key, value in pull_request.items() if key not in writers},
                         {key: value for key, value in develop.items() if key not in writers})
        self.assertEqual(develop["compiler-cache-writer"], "'true'")
        self.assertEqual(develop["conan-cache-writer"], "'true'")
        # Configure flags and the build command are identical
        # to the steps a pull request runs before the batch-only tests and journeys.
        compile_steps = editor.split("      - name: Verify unit and Editor tests", 1)[0]
        configure_and_build = self.runs(compile_steps)
        self.assertEqual(self.runs(cache), configure_and_build)
        self.assertEqual(len(configure_and_build), 2)
        self.assertEqual(develop["configure-editor-only"], "'true'")
        self.assertIn('LUDUS_EDITOR_PRECOMPILED_HEADERS: "0"', editor)
        self.assertIn('LUDUS_EDITOR_PRECOMPILED_HEADERS: "0"', cache)
        for name in ("CI", "LUDUS_DIAGNOSTIC_INTERACTIVE", "CMAKE_BUILD_PARALLEL_LEVEL"):
            value = re.search(rf'(?m)^  {name}: (.*)$', ci)
            self.assertIsNotNone(value, name)
            self.assertIn(f"  {name}: {value.group(1)}\n", warm)

    def test_warm_runs_one_at_a_time_on_develop_and_prunes_only_with_cache_privilege(self):
        warm = (self.ROOT / ".github/workflows/develop-cache.yml").read_text()
        self.assertIn("\nconcurrency:\n  group: develop-compiler-cache\n  cancel-in-progress: false\n", warm)
        self.assertIn("\npermissions:\n  contents: read\n\n", warm)
        cache = self.job(warm, "editor")
        self.assertIn("    if: ${{ !cancelled() && github.ref == 'refs/heads/develop' }}\n", cache)
        self.assertNotIn("actions: write", cache)
        # A develop compile failure still saves the objects that compiled.
        self.assertRegex(cache, r"id: build\n        continue-on-error: true\n")
        prune = warm.split("\n  prune:\n", 1)[1]
        self.assertIn("    needs: editor\n", prune)
        self.assertIn("    if: always() && github.ref == 'refs/heads/develop'\n", prune)
        self.assertIn("    permissions:\n      contents: read\n      actions: write\n", prune)
        self.assertIn("run: python3 scripts/python/batch_ci.py prune-caches", prune)
        # It reports no required check name.
        for name in ("Ubuntu Clang", "Develop health", "Editor, shader worker and unit tests"):
            self.assertNotIn(f"name: {name}\n", warm)


class PullRequestRunnerBudgetTests(unittest.TestCase):
    """Pull-request activity that changes no file must not occupy shared runners.

    Every pull-request workflow competes for the organization's hosted runners
    with every other pull request and the batch. Description edits and marking a
    draft ready used to re-run each compile workflow for an unchanged commit.
    """

    ROOT = Path(__file__).resolve().parents[2]
    FILELESS_ACTIONS = ("edited", "ready_for_review", "labeled", "unlabeled", "assigned", "review_requested")

    def test_only_the_single_job_develop_health_gate_runs_for_fileless_activity(self):
        for path in sorted((self.ROOT / ".github/workflows").glob("*.yml")):
            triggers = path.read_text().split("\njobs:\n", 1)[0]
            match = re.search(r"(?m)^  pull_request(?:_target)?:\n((?:    .*\n)*)", triggers)
            if match is None:
                continue
            types = re.search(r"(?m)^    types: \[([^\]]*)\]$", match.group(1))
            # GitHub's default activity types are opened, synchronize and reopened.
            actions = {value.strip() for value in types.group(1).split(",")} if types else set()
            with self.subTest(workflow=path.name):
                if actions & set(self.FILELESS_ACTIONS):
                    self.assertEqual(path.name, "develop-health-gate.yml")
                    jobs = re.findall(r"(?m)^  ([a-z][a-z0-9_-]*):\n", path.read_text().split("\njobs:\n", 1)[1])
                    self.assertEqual(len(jobs), 1, jobs)

    def test_evidence_dispatch_runs_one_native_job_under_its_own_gate_name(self):
        workflow = self.ROOT.joinpath(".github/workflows/ci.yml").read_text()
        dispatch = workflow.split("\n  workflow_dispatch:\n", 1)[1].split("\npermissions:", 1)[0]
        # evidence_job is the last dispatch input; its options are every native job.
        evidence = dispatch.split("      evidence_job:\n", 1)[1]
        self.assertIn("        type: choice\n        default: all\n", evidence)
        self.assertEqual(re.findall(r"(?m)^          - (\S+)$", evidence),
                         [ci_scope.ALL_EVIDENCE_JOBS, *ci_scope.NATIVE_JOBS])
        conditions = dict(re.findall(r"^  ([a-z][a-z0-9-]*):\n(?:    .*\n)*?    if: (.*)$", workflow, re.MULTILINE))
        for name in ci_scope.NATIVE_JOBS:
            with self.subTest(job=name):
                self.assertIn(f"((inputs.evidence_job || 'all') == 'all' || inputs.evidence_job == '{name}')",
                              conditions[name])
        # A scoped gate is not full Ubuntu Clang evidence, and scoped dispatches queue apart.
        self.assertIn("format('Evidence gate ({0})', inputs.evidence_job) || 'Ubuntu Clang' }}", workflow)
        self.assertIn("          EVIDENCE_JOB: ${{ inputs.evidence_job }}\n"
                      "          RESULTS: ${{ toJSON(needs) }}\n", workflow)
        self.assertIn("format('-evidence-{0}', inputs.evidence_job)", workflow.split("\nconcurrency:\n", 1)[1].split("\n\n", 1)[0])

    def test_required_gate_reads_only_job_results_and_classification(self):
        workflow = self.ROOT.joinpath(".github/workflows/ci.yml").read_text()
        self.assertNotIn("validated_by", workflow)
        self.assertNotIn("name: Develop health\n", workflow)
        self.assertIn(f"|| '{ci_scope.GATE_JOB}' }}}}\n", workflow)
        changes = workflow.split("\n  changes:\n", 1)[1].split("\n\n  ", 1)[0]
        self.assertIn("    permissions:\n      contents: read\n      packages: read\n", changes)
        action = self.ROOT.joinpath(".github/actions/change-scope/action.yml").read_text()
        self.assertNotIn("validated_by", action)
        self.assertNotIn("GITHUB_TOKEN", action)

class PendingPromotionJobTests(unittest.TestCase):
    """A native job added on develop may be absent from main's ci.yml until promotion."""

    ROOT = Path(__file__).resolve().parents[2]

    def results(self):
        return RequiredGateTests().results(True, ci_scope.FULL_TIER)

    def test_main_without_a_pending_job_passes_and_a_present_job_is_still_checked(self):
        for name in ci_scope.PENDING_PROMOTION_JOBS:
            with self.subTest(job=name):
                results = self.results()
                self.assertEqual(ci_scope.gate_failures(results), [])
                del results[name]
                self.assertEqual(ci_scope.gate_failures(results), [])
                for status in ("failure", "cancelled", "skipped"):
                    results = self.results()
                    results[name]["result"] = status
                    self.assertIn(f"{name}: expected success", " ".join(ci_scope.gate_failures(results)))

    def test_only_pending_jobs_may_be_absent(self):
        results = self.results()
        del results["macos-compile"]
        self.assertIn("inventory", " ".join(ci_scope.gate_failures(results)))

    def test_pending_jobs_exist_on_develop_and_are_removed_once_main_has_them(self):
        workflow = (self.ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        for name in ci_scope.PENDING_PROMOTION_JOBS:
            self.assertIn(f"\n  {name}:\n", workflow)
        shown = subprocess.run(["git", "show", "origin/main:.github/workflows/ci.yml"], cwd=self.ROOT,
                               capture_output=True, text=True, check=False)
        if shown.returncode != 0:
            self.skipTest("origin/main is not available in this checkout")
        promoted = [name for name in ci_scope.PENDING_PROMOTION_JOBS if f"\n  {name}:\n" in shown.stdout]
        self.assertEqual(promoted, [], "main defines these jobs now; remove them from PENDING_PROMOTION_JOBS")


if __name__ == "__main__":
    unittest.main()
