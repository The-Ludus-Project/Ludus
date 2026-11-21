"""Regressions for CTest shard selection from one listing every shard shares."""

import contextlib
import io
import json
import stat
import sys
import tempfile
import unittest
from pathlib import Path

import ctest_shard
from ctest_shard import ShardError, compute_digest, read_names, select_shard

# Shape matches ctest 3.29 --show-only=json-v1 output.
LISTING = {
    "kind": "ctestInfo",
    "version": {"major": 1, "minor": 0},
    "tests": [{"name": f"test_{index:02d}", "properties": []} for index in range(1, 12)]
    + [{"name": "ludus_render_graph_ray: shadow ray, two lights", "properties": []}],
}

FAKE_CTEST = """#!{python}
import json, sys
with open({record!r}, "w", encoding="utf-8") as record:
    json.dump(sys.argv[1:], record)
if {fail!r}:
    sys.exit(8)
print(json.dumps({listing!r}))
"""


class SelectionTests(unittest.TestCase):
    def test_shards_are_disjoint_and_complete(self):
        names = read_names(LISTING)
        for shards in range(1, len(names) + 1):
            with self.subTest(shards=shards):
                selections = [select_shard(names, shard, shards) for shard in range(1, shards + 1)]
                selected = [name for selection in selections for name in selection]
                self.assertEqual(sorted(selected), sorted(names))
                self.assertEqual(len(selected), len(set(selected)))
                # Round-robin keeps the shards within one test of each other.
                sizes = [len(selection) for selection in selections]
                self.assertLessEqual(max(sizes) - min(sizes), 1)

    def test_selection_follows_listing_order_like_tests_information(self):
        names = read_names(LISTING)
        # --tests-information 2,,4 runs tests #2, #6, #10 of one numbering.
        self.assertEqual(select_shard(names, 2, 4), [names[1], names[5], names[9]])

    def test_a_per_shard_listing_reproduces_the_overlap_one_listing_prevents(self):
        # Shard 1's probes registered tests ahead of the others' numbering. A
        # stride over each shard's own tree then overlaps and skips tests.
        shared = read_names(LISTING)
        shifted = ["probe_a", "probe_b", *shared]
        per_tree = [select_shard(shifted if shard == 1 else shared, shard, 4) for shard in range(1, 5)]
        covered = [name for selection in per_tree for name in selection if name in shared]
        self.assertNotEqual(sorted(set(covered)), sorted(shared))
        self.assertNotEqual(len(covered), len(set(covered)))
        one_listing = [name for shard in range(1, 5) for name in select_shard(shared, shard, 4)]
        self.assertEqual(sorted(one_listing), sorted(shared))

    def test_invalid_requests_and_listings_are_errors(self):
        names = read_names(LISTING)
        for shard, shards in ((0, 4), (5, 4), (1, 0), (-1, 4)):
            with self.subTest(shard=shard, shards=shards), self.assertRaises(ShardError):
                select_shard(names, shard, shards)
        for tests in ([{"name": "a"}, {"name": "a"}], [{"name": ""}], [{}], [{"name": "a\nb"}]):
            with self.subTest(tests=tests), self.assertRaises(ShardError):
                read_names({"tests": tests})
        self.assertEqual(read_names({}), [])

    def test_digest_identifies_the_listing(self):
        names = read_names(LISTING)
        self.assertEqual(compute_digest(names), compute_digest(list(names)))
        self.assertNotEqual(compute_digest(names), compute_digest(names[1:]))
        self.assertNotEqual(compute_digest(names), compute_digest([*names[1:], names[0]]))


class MainTests(unittest.TestCase):
    def run_main(self, *argv: str) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = ctest_shard.main(list(argv))
        return status, output.getvalue()

    def write_fake_ctest(self, directory: Path, listing: dict, should_fail: bool = False) -> tuple[Path, Path]:
        record = directory / "arguments.json"
        ctest = directory / "ctest"
        ctest.write_text(
            FAKE_CTEST.format(python=sys.executable, record=str(record), fail=should_fail, listing=listing),
            encoding="utf-8",
        )
        ctest.chmod(ctest.stat().st_mode | stat.S_IXUSR)
        return ctest, record

    def test_live_listing_writes_one_exact_name_per_line_and_records_the_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            ctest, record = self.write_fake_ctest(directory, LISTING)
            output = directory / "nested" / "shard.txt"
            summary = directory / "summary.md"
            status, printed = self.run_main(
                str(directory / "build"), "--ctest", str(ctest), "--shard", "4", "--shards", "4",
                "--exclude-regex", "^ludus_game_host_reload_tests$", "--output", str(output),
                "--summary", str(summary),
            )
            self.assertEqual(status, 0, printed)
            names = read_names(LISTING)
            self.assertEqual(output.read_text(encoding="utf-8").splitlines(), select_shard(names, 4, 4))
            self.assertEqual(output.read_text(encoding="utf-8").splitlines()[-1], names[-1])
            self.assertEqual(
                json.loads(record.read_text(encoding="utf-8")),
                ["--test-dir", str(directory / "build"), "--show-only=json-v1",
                 "--exclude-regex", "^ludus_game_host_reload_tests$"],
            )
            self.assertIn(f"3 of {len(names)} tests", summary.read_text(encoding="utf-8"))
            self.assertIn(compute_digest(names), summary.read_text(encoding="utf-8"))

    def test_saved_listing_selects_without_ctest(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "tests.json").write_text(json.dumps(LISTING), encoding="utf-8")
            status, printed = self.run_main(
                str(directory), "--tests-json", str(directory / "tests.json"), "--shard", "1", "--shards", "4",
                "--output", str(directory / "shard.txt"), "--summary", str(directory / "summary.md"),
            )
            self.assertEqual(status, 0, printed)
            self.assertEqual((directory / "shard.txt").read_text(encoding="utf-8"), "test_01\ntest_05\ntest_09\n")

    def test_failures_exit_non_zero_and_write_no_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            failing, _ = self.write_fake_ctest(directory, LISTING, should_fail=True)
            (directory / "empty.json").write_text(json.dumps({"tests": []}), encoding="utf-8")
            (directory / "two.json").write_text(json.dumps({"tests": [{"name": "a"}, {"name": "b"}]}), encoding="utf-8")
            (directory / "tests.json").write_text(json.dumps(LISTING), encoding="utf-8")
            cases = {
                "ctest fails": ["--ctest", str(failing)],
                "ctest missing": ["--ctest", str(directory / "absent")],
                "unreadable listing": ["--tests-json", str(directory / "absent.json")],
                "empty listing": ["--tests-json", str(directory / "empty.json")],
                "empty shard": ["--tests-json", str(directory / "two.json")],
                "exclusion needs ctest": ["--tests-json", str(directory / "tests.json"), "--exclude-regex", "x"],
            }
            for case, argv in cases.items():
                with self.subTest(case=case):
                    output = directory / f"{case}.txt"
                    status, printed = self.run_main(
                        str(directory), *argv, "--shard", "3", "--shards", "4", "--output", str(output),
                        "--summary", str(directory / "summary.md"),
                    )
                    self.assertEqual(status, 1)
                    self.assertIn("::error title=CTest shard::", printed)
                    self.assertFalse(output.exists())
            self.assertFalse((directory / "summary.md").exists())


if __name__ == "__main__":
    unittest.main()
