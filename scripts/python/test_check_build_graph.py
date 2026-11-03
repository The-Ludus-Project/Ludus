"""Regression tests for tools/check_build_graph.py."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_build_graph", ROOT / "tools" / "check_build_graph.py")
build_graph = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_graph)


class BuildGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.source_root = Path(self.directory.name).resolve()
        self.build_root = self.source_root / "out" / "build" / "preset"
        self.build_root.mkdir(parents=True)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def entry(self, source: str, target: str) -> dict[str, str]:
        path = self.source_root / source
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int value = 0;\nint other = 1;\n", encoding="utf-8")
        name = path.name
        return {
            "directory": str(self.build_root),
            "file": str(path),
            "output": f"apps/editor/CMakeFiles/{target}.dir/src/{name}.o",
        }

    def run_check(self, entries: list[dict[str, str]], rules: list[dict[str, object]]) -> tuple[int, dict]:
        (self.build_root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
        config = self.source_root / "build_graph.json"
        config.write_text(json.dumps({"rules": rules}), encoding="utf-8")
        report_path = self.source_root / "report.json"
        status = build_graph.main([str(self.build_root), "--config", str(config), "--source-root",
                                   str(self.source_root), "--report", str(report_path)])
        report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else {}
        return status, report

    def test_rejects_a_source_two_targets_compile(self) -> None:
        status, report = self.run_check(
            [self.entry("apps/editor/src/canvas.cpp", "ludus_editor"),
             self.entry("apps/editor/src/canvas.cpp", "ludus_editor_tests"),
             self.entry("apps/editor/src/main.cpp", "ludus_editor")], [])
        self.assertEqual(status, 1)
        self.assertEqual([record["source"] for record in report["unexpected"]], ["apps/editor/src/canvas.cpp"])
        self.assertEqual(report["unexpected"][0]["targets"], ["ludus_editor", "ludus_editor_tests"])
        self.assertEqual(report["unexpected"][0]["lines"], 2)
        self.assertEqual(report["compilations"], 3)
        self.assertEqual(report["sources"], 2)

    def test_accepts_a_variant_with_its_reason(self) -> None:
        status, report = self.run_check(
            [self.entry("modules/foundation/filesystem/src/path.cpp", "ludus_foundation_filesystem"),
             self.entry("modules/foundation/filesystem/src/path.cpp", "ludus_filesystem_fault_backend")],
            [{"kind": "variant", "sources": ["modules/foundation/filesystem/src/*"], "reason": "fault hooks"}])
        self.assertEqual(status, 0)
        self.assertEqual(len(report["variant"]), 1)
        self.assertEqual(report["unexpected"], [])

    def test_names_generated_sources_relative_to_the_build_tree(self) -> None:
        generated = self.build_root / "generated" / "table.cpp"
        generated.parent.mkdir(parents=True)
        generated.write_text("const int TABLE = 0;\n", encoding="utf-8")
        entries = [{"directory": str(self.build_root), "file": str(generated),
                    "output": f"CMakeFiles/{target}.dir/generated/table.cpp.o"} for target in ("a", "b")]
        status, report = self.run_check(entries, [])
        self.assertEqual(status, 1)
        self.assertEqual(report["unexpected"][0]["source"], "<build>/generated/table.cpp")

    def test_resolved_baseline_must_be_removed(self) -> None:
        rule = {"kind": "baseline", "sources": ["apps/smoke/lifecycle.cpp"],
                "targets": ["ludus_smoke", "ludus_smoke_lifecycle_tests"], "reason": "tests compile the app source"}
        status, report = self.run_check(
            [self.entry("apps/smoke/lifecycle.cpp", "ludus_smoke"),
             self.entry("apps/smoke/lifecycle_tests.cpp", "ludus_smoke_lifecycle_tests")], [rule])
        self.assertEqual(status, 1)
        self.assertEqual(report["stale_baseline"][0]["sources"], ["apps/smoke/lifecycle.cpp"])

    def test_baseline_is_not_resolved_by_a_tree_without_its_targets(self) -> None:
        rule = {"kind": "baseline", "sources": ["apps/smoke/lifecycle.cpp"],
                "targets": ["ludus_smoke", "ludus_smoke_lifecycle_tests"], "reason": "tests compile the app source"}
        status, report = self.run_check([self.entry("apps/smoke/lifecycle.cpp", "ludus_smoke")], [rule])
        self.assertEqual(status, 0)
        self.assertEqual(report["stale_baseline"], [])

    def test_baseline_rules_name_their_targets(self) -> None:
        status, report = self.run_check(
            [self.entry("apps/smoke/lifecycle.cpp", "ludus_smoke")],
            [{"kind": "baseline", "sources": ["apps/smoke/lifecycle.cpp"], "reason": "tests compile it"}])
        self.assertEqual(status, 2)
        self.assertEqual(report, {})

    def test_repository_rules_are_well_formed(self) -> None:
        rules, problems = build_graph.load_rules(build_graph.DEFAULT_CONFIG)
        self.assertEqual(problems, [])
        self.assertGreater(len(rules), 0)


if __name__ == "__main__":
    unittest.main()
