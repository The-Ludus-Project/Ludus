"""Regression tests for tools/check_naming.py."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_naming", ROOT / "tools" / "check_naming.py")
check_naming = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_naming)


class CheckNamingTests(unittest.TestCase):
    def findings(self, source: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "a.cpp"
            path.write_text(source)
            return [finding.split("] ")[0].split("[")[1] for finding in check_naming.check_file(root, path)]

    def test_nullable_members_locals_and_move_parameters(self) -> None:
        self.assertEqual(self.findings("struct A { Thing* mValue{}; }; Thing* value = nullptr;"), ["N4", "N4"])
        self.assertEqual(self.findings("void Move(Thing&& inoutOther);"), ["N2"])
        self.assertEqual(self.findings("void Move(Thing&& other); Thing* valueOrNull = nullptr;"), [])

    def test_reports_each_rule(self) -> None:
        source = "bool F(Foo& foo, int* p = nullptr, bool ok);\nvoid G() { if (!x) {} }\n"
        self.assertEqual(sorted(self.findings(source)), ["N1", "N2", "N3", "N4"])

    def test_accepts_house_style(self) -> None:
        source = (
            "bool F(Foo& inoutFoo, const Bar& bar, Baz& outBaz, int* pOrNull = nullptr, bool isOk);\n"
            "void G() { if (x == false && a != b) {} bool hasItems = true; for (Q& q : r) {} int z = a & b; }\n"
            "// !comment and Foo& foo, in a comment\nconst char* s = \"!text\";\n"
        )
        self.assertEqual(self.findings(source), [])

    def test_negation_is_checked_outside_adopted_scopes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "modules" / "legacy" / "src" / "a.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                "#if !defined(X)\n#endif\nvoid G() { if (!x || not y) {} if (a != b) {} }\n"
                "// !comment\nconst char* s = \"!text\";\n"
            )
            (root / "third_party" / "lib").mkdir(parents=True)
            (root / "third_party" / "lib" / "b.cpp").write_text("bool b = !a;\n")
            (root / "tests" / "support").mkdir(parents=True)
            (root / "tests" / "support" / "catch_stream.hpp").write_text("bool b = !a;\n")
            self.assertEqual(check_naming.negation_files(root), [source])
            findings = check_naming.check_negation(root, source)
            self.assertEqual(len(findings), 2)
            self.assertTrue(all(":3: [N1]" in finding for finding in findings))

    def test_negation_exemptions_keep_line_numbers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "a.cpp"
            source.write_text(
                "EM_JS(int, Probe, (), {\n  return !Module.ready;\n});\n"
                "bool operator!(const A& a);\n"
                "char c = '\\n'; int z = 1'000;\n"
                "bool b = !a;\n"
            )
            findings = check_naming.check_negation(root, source)
            self.assertEqual(len(findings), 1)
            self.assertIn("a.cpp:6: [N1]", findings[0])


if __name__ == "__main__":
    unittest.main()
