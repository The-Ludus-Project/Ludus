"""Regressions for changed-line clang-tidy planning."""

import json
import contextlib
import io
import os
import shlex
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import engine
import subprocess

from authored_lines import parse_authored_lines, to_ranges
from changed_tidy import (changed_lines, changed_translation_units, line_filter, parse_changed_lines, plan,
                          uncompiled_sources)

DIFF = """diff --git a/modules/a/src/a.cpp b/modules/a/src/a.cpp
index 1..2 100644
--- a/modules/a/src/a.cpp
+++ b/modules/a/src/a.cpp
@@ -3,0 +4,2 @@ void F()
+int x = 42;
+int y = 7;
@@ -10 +12 @@ void G()
-old
+new
@@ -20,3 +22,0 @@ void H()
-gone
diff --git a/modules/a/tests/a_tests.cpp b/modules/a/tests/a_tests.cpp
--- a/modules/a/tests/a_tests.cpp
+++ b/modules/a/tests/a_tests.cpp
@@ -1 +1 @@
-a
+b
diff --git a/modules/a/include/a.h b/modules/a/include/a.h
--- a/modules/a/include/a.h
+++ b/modules/a/include/a.h
@@ -5,0 +6 @@
+int Z();
"""


class ChangedTidyTests(unittest.TestCase):
    def test_parses_added_ranges_and_ignores_deletions(self):
        self.assertEqual(
            parse_changed_lines(DIFF),
            {"modules/a/src/a.cpp": [[4, 5], [12, 12]], "modules/a/tests/a_tests.cpp": [[1, 1]],
             "modules/a/include/a.h": [[6, 6]]},
        )

    def test_moved_lines_keep_their_origin_and_written_lines_count(self):
        # The batch compares main with a candidate holding many pull requests; a block moved
        # past an untouched line must not make that line "changed" (authored_lines.py).
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*arguments):
                return subprocess.run(
                    ["git", *arguments], cwd=root, check=True, text=True, capture_output=True
                ).stdout.strip()

            git("init", "-q")
            git("config", "user.name", "Tidy test")
            git("config", "user.email", "tidy@example.invalid")
            (root / "modules").mkdir()
            source = root / "modules/a.cpp"
            legacy = "const auto* imported = internal::NativeDeviceRequest();\n"
            block = "".join(f"Status initialized{i} = InitializeOwnerSlot({i});\n" for i in range(4))
            source.write_text(legacy + block)
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            source.write_text(block + legacy)
            git("commit", "-qam", "move")
            self.assertEqual(changed_lines(root, base), {})
            source.write_text(block + legacy + "int added = 42;\n")
            self.assertEqual(changed_lines(root, base), {"modules/a.cpp": [[6, 6]]})

    def test_blame_boundaries_are_not_authored(self):
        old = "a" * 40
        new = "b" * 40
        porcelain = (
            f"{old} 1 1 2\nauthor x\nboundary\nfilename a.cpp\n\tline\n"
            f"{old} 2 2\n\tline\n"
            f"{new} 3 3 1\nauthor y\nfilename a.cpp\n\tline\n"
        )
        self.assertEqual(parse_authored_lines(porcelain), {3})
        self.assertEqual(to_ranges({1, 2, 3, 7, 9, 10}), [[1, 3], [7, 7], [9, 10]])

    def test_plans_changed_sources_with_test_policy_and_header_filter(self):
        root = Path("/repo")
        changed = parse_changed_lines(DIFF)
        entries = [{"file": "/repo/modules/a/src/a.cpp"}, {"file": "/repo/modules/a/src/a.cpp"},
                   {"file": "/repo/modules/a/tests/a_tests.cpp"}, {"file": "/repo/modules/a/src/other.cpp"}]
        selected = plan(root, entries, changed)
        self.assertEqual([(entry["file"], policy.name) for entry, policy in selected], [
            ("/repo/modules/a/src/a.cpp", "clang-tidy-changed.yml"),
            ("/repo/modules/a/tests/a_tests.cpp", "clang-tidy-changed-tests.yml"),
        ])
        names = [entry["name"] for entry in json.loads(line_filter(root, changed))]
        self.assertIn("/repo/modules/a/include/a.h", names)

    def test_policies_exist_and_tests_omit_magic_numbers(self):
        root = Path(__file__).resolve().parents[2]
        production = (root / "config/clang-tidy-changed.yml").read_text()
        tests = (root / "config/clang-tidy-changed-tests.yml").read_text()
        self.assertIn("readability-magic-numbers,", production)
        self.assertNotIn("readability-magic-numbers,", tests)
        for text in (production, tests):
            self.assertIn("readability-identifier-naming", text)
            self.assertIn("PrivateMethodCase", text)


class LargeBatchExecutionTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "bounded quality analysis requires POSIX")
    def test_large_filter_still_reports_changed_source_and_header_diagnostics(self):
        tool = shutil.which(engine.pinned_llvm_tool(engine.repo_root(), "clang-tidy"))
        if tool is None:
            self.skipTest("pinned clang-tidy is unavailable")
        with tempfile.TemporaryDirectory(prefix="ludus tidy 'quoted ü ") as temporary:
            root = Path(temporary).resolve()
            source = root / "modules/example/src/example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text('#include "changed.h"\nint GetSourceValue() { return 43; }\n'
                              'int GetUnchangedValue() { return 47; }\n')
            header = source.with_name("changed.h")
            header.write_text("inline int GetHeaderValue() { return 41; }\n")
            build = root / "out/build/test"
            build.mkdir(parents=True)
            entry = {"directory": str(root), "file": str(source),
                     "arguments": ["clang++", "-std=c++23", "-c", str(source)]}
            (build / "compile_commands.json").write_text(json.dumps([entry]))
            policy = root / "config/clang-tidy-changed.yml"
            policy.parent.mkdir()
            policy.write_text("Checks: '-*,readability-magic-numbers'\nWarningsAsErrors: '*'\n")
            changed = {f"modules/example/include/uncompiled_{index}.h": [[1, 1]] for index in range(1500)}
            changed.update({source.relative_to(root).as_posix(): [[2, 2]],
                            header.relative_to(root).as_posix(): [[1, 1]]})
            captured = io.StringIO()
            with mock.patch.object(engine, "load_tool_versions", return_value={}), \
                    mock.patch.object(engine, "find_system_tool", return_value=tool), \
                    mock.patch("changed_tidy.changed_lines", return_value=changed), \
                    mock.patch.dict(os.environ, {"LUDUS_TIDY_SHARD_COUNT": "1", "LUDUS_TIDY_SHARD_INDEX": "0",
                                                 "LUDUS_TIDY_JOBS": "1"}), \
                    contextlib.redirect_stdout(captured), \
                    self.assertRaisesRegex(engine.EngineError, "1 translation unit"):
                engine.run_tidy_changed(root, "test", "base")
                self.fail("Expected fixture diagnostics:\n" + captured.getvalue())
            report_path = next(root.glob("out/quality/analysis/run-*/results.json"))
            result = json.loads(report_path.read_text())["results"][0]
            self.assertEqual(result["state"], "fail", "the tool must launch and diagnose the actual fixture")
            self.assertTrue(result["complete"])
            self.assertIn("GetSourceValue", captured.getvalue())
            self.assertIn("GetHeaderValue", captured.getvalue())
            self.assertNotIn("GetUnchangedValue", captured.getvalue())
            response = Path(next(argument[1:] for argument in result["argv"] if argument.startswith("@")))
            self.assertGreater(response.stat().st_size, 128 * 1024)
            argument = shlex.split(response.read_text())[0]
            self.assertEqual(len(json.loads(argument.removeprefix("--line-filter="))), len(changed))
            self.assertLess(max(len(argument.encode()) for argument in result["argv"]), 8192)


def make_repository(root: Path):
    def git(*arguments):
        return subprocess.run(
            ["git", *arguments], cwd=root, check=True, text=True, capture_output=True
        ).stdout.strip()

    git("init", "-q")
    git("config", "user.name", "Tidy test")
    git("config", "user.email", "tidy@example.invalid")
    return git


class UncompiledSourceTests(unittest.TestCase):
    """Changed translation units outside the selected compile database are reported, not dropped."""

    CHANGED = {"modules/platform/src/window_cocoa.mm": [[3, 3]], "modules/a/src/a.cpp": [[1, 1]],
               "modules/rhi/src/rhi_d3d12.cpp": [[2, 2]], "modules/a/include/a.h": [[6, 6]]}

    def test_names_changed_translation_units_missing_from_the_database(self):
        entries = [{"file": "/repo/modules/a/src/a.cpp"}]
        self.assertEqual(uncompiled_sources(Path("/repo"), entries, self.CHANGED),
                         ["modules/platform/src/window_cocoa.mm", "modules/rhi/src/rhi_d3d12.cpp"])
        # Headers are analyzed through includers and never reported as uncompiled.
        self.assertEqual(uncompiled_sources(Path("/repo"), entries, ["modules/a/include/a.h"]), [])

    def report(self, names, *, actions):
        captured = io.StringIO()
        with mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "true" if actions else ""}), \
                contextlib.redirect_stdout(captured):
            engine.report_uncompiled_sources(names, "linux-clang-development")
        return captured.getvalue()

    def test_github_actions_warns_once_per_file_and_names_the_analyzing_job(self):
        output = self.report(["modules/platform/tests/cocoa_tests.mm", "modules/rhi/src/rhi_d3d12.cpp"], actions=True)
        warnings = [line for line in output.splitlines() if line.startswith("::warning ")]
        self.assertEqual(len(warnings), 2)
        self.assertTrue(warnings[0].startswith("::warning file=modules/platform/tests/cocoa_tests.mm,"))
        self.assertIn(engine.OBJECTIVE_CXX_JOB_NAME, warnings[0])
        self.assertIn("macOS batch jobs", warnings[0])
        self.assertTrue(warnings[1].startswith("::warning file=modules/rhi/src/rhi_d3d12.cpp,"))
        self.assertIn("batch job for its platform", warnings[1])
        self.assertIn("not compiled by linux-clang-development", output)

    def test_local_runs_print_without_workflow_commands(self):
        output = self.report(["modules/platform/tests/cocoa_tests.mm"], actions=False)
        self.assertIn("modules/platform/tests/cocoa_tests.mm is not compiled by preset linux-clang-development", output)
        self.assertNotIn("::warning", output)
        self.assertEqual(self.report([], actions=True), "")

    def test_tidy_changed_reports_uncompiled_sources_even_when_nothing_is_analyzed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / "out/build/test"
            build.mkdir(parents=True)
            (build / "compile_commands.json").write_text(json.dumps(
                [{"directory": str(root), "file": str(root / "modules/a/src/other.cpp"), "arguments": ["c++"]}]))
            captured = io.StringIO()
            with mock.patch.object(engine, "load_tool_versions", return_value={}), \
                    mock.patch.object(engine, "find_system_tool", return_value="clang-tidy"), \
                    mock.patch("changed_tidy.changed_lines",
                               return_value={"modules/platform/tests/cocoa_tests.mm": [[4, 4]]}), \
                    mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}), \
                    mock.patch.object(engine, "run_analysis_commands") as analysis, \
                    contextlib.redirect_stdout(captured):
                engine.run_tidy_changed(root, "test", "base")
            analysis.assert_not_called()
            self.assertIn("::warning file=modules/platform/tests/cocoa_tests.mm,", captured.getvalue())
            self.assertIn("no changed source is compiled by this preset", captured.getvalue())


class ObjectiveCxxAnalysisTests(unittest.TestCase):
    def test_changed_translation_units_include_moved_files_and_exclude_deletions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            git = make_repository(root)
            (root / "modules/platform").mkdir(parents=True)
            (root / "modules/platform/kept.mm").write_text("int A();\nint B();\n")
            (root / "modules/platform/gone.mm").write_text("int C();\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            # A pure line move still changes the file a whole-file analysis diagnoses.
            (root / "modules/platform/kept.mm").write_text("int B();\nint A();\n")
            (root / "modules/platform/gone.mm").unlink()
            (root / "modules/platform/new.cpp").write_text("int D();\n")
            (root / "docs").mkdir()
            (root / "docs/outside.mm").write_text("int E();\n")
            git("add", "-A")
            git("commit", "-qm", "change")
            self.assertEqual(changed_translation_units(root, base, frozenset({".mm"})), ["modules/platform/kept.mm"])
            self.assertEqual(changed_translation_units(root, base, frozenset({".mm", ".cpp"})),
                             ["modules/platform/kept.mm", "modules/platform/new.cpp"])

    def run_objcxx(self, root, changed):
        captured = io.StringIO()
        with mock.patch("changed_tidy.changed_translation_units", return_value=changed), \
                mock.patch.object(engine, "cmake_configure") as configure, \
                mock.patch.object(engine, "cmake_build") as build, \
                mock.patch.object(engine, "run_tidy_sources") as tidy, \
                mock.patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}), \
                contextlib.redirect_stdout(captured):
            engine.run_tidy_changed_objcxx(root, "test", "base")
        return configure, build, tidy, captured.getvalue()

    def test_builds_then_analyzes_only_compiled_changed_objective_cxx(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build_dir = root / "out/build/test"
            build_dir.mkdir(parents=True)
            compiled = root / "modules/platform/src/window_cocoa.mm"
            (build_dir / "compile_commands.json").write_text(json.dumps(
                [{"directory": str(root), "file": str(compiled), "arguments": ["clang++"]}]))
            changed = ["modules/platform/src/window_cocoa.mm", "tests/sdk_consumer/native_interop_metal.mm"]
            configure, build, tidy, output = self.run_objcxx(root, changed)
            configure.assert_not_called()
            build.assert_called_once_with(root, "test", ["--target", f"{compiled}^"])
            tidy.assert_called_once_with(root, "test", ["modules/platform/src/window_cocoa.mm"])
            self.assertIn("::warning file=tests/sdk_consumer/native_interop_metal.mm,", output)
            # The macOS job does not refer an unconfigured source to itself.
            self.assertNotIn(engine.OBJECTIVE_CXX_JOB_NAME, output)
            self.assertIn("only the build that configures it", output)

    def test_no_changed_objective_cxx_needs_no_build(self):
        configure, build, tidy, output = self.run_objcxx(Path("/repo"), [])
        for step in (configure, build, tidy):
            step.assert_not_called()
        self.assertIn("no Objective-C++ source changed", output)

    def test_check_command_routes_the_mode(self):
        args = engine.make_parser().parse_args(["check", "macos-clang-development", "--tidy-changed-objcxx", "--base", "origin/develop"])
        self.assertTrue(args.tidy_changed_objcxx)
        with mock.patch.object(engine, "ensure_bootstrap_for_preset"), \
                mock.patch.object(engine, "run_tidy_changed_objcxx") as objcxx, \
                mock.patch.object(engine, "run_format_check") as format_check:
            engine.command_check(args)
        objcxx.assert_called_once_with(engine.repo_root(), "macos-clang-development", "origin/develop")
        format_check.assert_not_called()


if __name__ == "__main__":
    unittest.main()
