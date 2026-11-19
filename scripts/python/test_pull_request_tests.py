"""Regressions for the quick pull-request test list and its checker."""

import unittest

import pull_request_tests
from pull_request_tests import LIST, MAX_TIMEOUT_SECONDS, list_failures, read_names, test_failures


def record(name, labels=("unit",), **properties):
    entries = [{"name": "LABELS", "value": list(labels)}]
    entries += [{"name": key, "value": value} for key, value in properties.items()]
    return {"name": name, "properties": entries}


class PullRequestTestListTests(unittest.TestCase):
    def test_repository_list_is_sorted_unique_names(self):
        names = read_names(LIST.read_text(encoding="utf-8"))
        self.assertTrue(names)
        self.assertEqual(list_failures(names), [])
        for name in names:
            with self.subTest(name=name):
                # Exact CTest names: --tests-from-file does not read patterns.
                self.assertRegex(name, r"^[a-z0-9_]+$")

    def test_comments_and_blank_lines_are_not_names(self):
        self.assertEqual(read_names("# header\n\na_tests\n  b_tests  \n"), ["a_tests", "b_tests"])
        self.assertTrue(list_failures(["b_tests", "a_tests"]))
        self.assertTrue(list_failures(["a_tests", "a_tests"]))

    def test_accepts_quick_unit_and_editor_tests(self):
        tests = [record("a_tests"), record("b_tests", ("editor", "layout"), TIMEOUT=MAX_TIMEOUT_SECONDS)]
        self.assertEqual(test_failures(["a_tests", "b_tests"], tests), [])

    def test_reports_each_entry_that_cannot_run_quickly_and_once(self):
        tests = [
            record("gpu_tests", ("unit", "gpu")),
            record("fixture_tests", FIXTURES_REQUIRED=["cooked"]),
            record("slow_tests", TIMEOUT=MAX_TIMEOUT_SECONDS + 1),
            record("tool_tests", ("tooling",)),
        ]
        names = ["gpu_tests", "fixture_tests", "slow_tests", "tool_tests", "renamed_tests"]
        failures = test_failures(names, tests)
        self.assertEqual(len(failures), len(names))
        for name, failure in zip(names, failures):
            with self.subTest(name=name):
                self.assertTrue(failure.startswith(f"{name}: "), failure)
        self.assertIn("no such CTest entry", failures[-1])

    def test_module_names_the_checked_in_list(self):
        self.assertEqual(LIST.name, "pull_request_tests.txt")
        self.assertEqual(LIST.parent.name, "config")
        self.assertTrue(pull_request_tests.REQUIRED_LABELS <= {"unit", "editor"})


if __name__ == "__main__":
    unittest.main()
