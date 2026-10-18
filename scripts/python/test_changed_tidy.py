"""Regressions for changed-line clang-tidy planning."""

import json
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
