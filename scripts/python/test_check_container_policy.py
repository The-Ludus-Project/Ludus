"""Regression tests for tools/check_container_policy.py."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_container_policy", ROOT / "tools" / "check_container_policy.py"
)
policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(policy)

BENCHMARK = "modules/foundation/containers/benchmarks/array_bench.cpp"


def write(root: Path, files: dict[str, str]) -> None:
    # Every approved boundary must exist in a synthetic tree, otherwise the
    # stale-exception gate correctly reports the fixture itself as incomplete.
    sources = {name: "std::vector<int> upstreamBoundary;\n" for name in policy.EXCEPTIONS}
    sources.update(files)
    for name, text in sources.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


class ContainerPolicyTests(unittest.TestCase):
    def test_reports_includes_and_uses_in_every_first_party_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                {
                    BENCHMARK: "#include <vector>\nstd::vector<int> baseline;\n",
                    "modules/a/src/a.cpp": "#include <vector>\nstd::vector<int> values;\n",
                    "modules/a/include/a.h": "void F(const std :: vector<int>& values);\n",
                    "tests/b.cpp": "std::pmr::vector<int> values;\n",
                    "apps/c.mm": "  #  include <vector>\n",
                    "tools/d.cpp": "using Values = std::vector<int>;\n",
                    "examples/e.hpp": "std::vector<float> samples;\n",
                },
            )
            problems = policy.violations(root)
            self.assertEqual(len(problems), 7, problems)
            self.assertTrue(
                any(problem.startswith("modules/a/src/a.cpp:1: #include <vector>") for problem in problems),
                problems,
            )
            self.assertTrue(all(BENCHMARK not in problem for problem in problems), problems)

    def test_ignores_comments_literals_vendor_and_similar_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                {
                    BENCHMARK: "std::vector<int> baseline;\n",
                    "modules/a/src/a.cpp": (
                        "// std::vector and <vector> are banned\n"
                        "/* #include <vector>\n std::vector */\n"
                        'const char* note = "std::vector";\n'
                        'const char* raw = R"x(std::vector)x";\n'
                        "Vector3 position; std::vectorish other; my::vector third;\n"
                        "#include <vector_helpers.h>\n"
                    ),
                    "third_party/lib/x.cpp": "#include <vector>\n",
                    "modules/a/out/generated.cpp": "std::vector<int> values;\n",
                    "modules/a/notes.md": "std::vector\n",
                },
            )
            self.assertEqual(policy.violations(root), [])

    def test_reports_line_numbers_after_multiline_comments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                {
                    BENCHMARK: "std::vector<int> baseline;\n",
                    "modules/a/src/a.cpp": "/*\n\n*/\nstd::vector<int> values;\n",
                },
            )
            self.assertEqual(
                [problem.split(": ")[0] for problem in policy.violations(root)],
                ["modules/a/src/a.cpp:4"],
            )

    def test_stale_exception_is_reported(self) -> None:
        for exception in policy.EXCEPTIONS:
            with self.subTest(exception=exception), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                write(root, {exception: "ludus::foundation::Array<int> values;\n"})
                problems = policy.violations(root)
                self.assertEqual(len(problems), 1, problems)
                self.assertTrue(problems[0].startswith(exception + ": stale container-policy exception"))

    def test_crashpad_exception_does_not_allow_an_adjacent_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            neighbor = "apps/editor/src/crash_capture_other.mm"
            write(root, {neighbor: "std::vector<int> values;\n"})
            problems = policy.violations(root)
            self.assertEqual(len(problems), 1, problems)
            self.assertTrue(problems[0].startswith(neighbor + ":1: std::vector is prohibited"))

    def test_repository(self) -> None:
        self.assertEqual(policy.violations(ROOT), [])


if __name__ == "__main__":
    unittest.main()
