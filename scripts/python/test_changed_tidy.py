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
from changed_tidy import line_filter, parse_changed_lines, plan

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


if __name__ == "__main__":
    unittest.main()
