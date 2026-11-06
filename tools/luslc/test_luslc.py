"""Tests of the luslc command-line compiler (docs/modules/tooling/shader-language.md#luslc)."""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import unittest

LUSLC = None
REPOSITORY = None


def run(*arguments, root):
    return subprocess.run([LUSLC, *arguments, "--root", str(root)], capture_output=True, text=True, check=False)


class LuslcTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def test_lower_writes_slang_with_line_directives(self):
        self.write("main/common.luslh", "static const uint32 COUNT = 4;\n")
        self.write(
            "main/shade.lusl",
            '#include "common.luslh"\n'
            "float32 Scale(in float32 value, in float32 factor = 2.0)\n{\n    return value * factor;\n}\n",
        )
        result = run("lower", "main/shade.lusl", root=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#line 2 "main/shade.lusl"', result.stdout)
        self.assertIn("static const uint COUNT = 4u;", result.stdout)
        self.assertIn("float Scale(float value, float factor)", result.stdout)

    def test_output_file_and_check(self):
        self.write("shade.lusl", "static const uint32 COUNT = 4;\n")
        output = self.root / "out" / "shade.lusl.slang"
        output.parent.mkdir()
        self.assertEqual(run("lower", "shade.lusl", "--output", str(output), root=self.root).returncode, 0)
        self.assertIn("COUNT", output.read_text(encoding="utf-8"))
        self.assertEqual(run("check", "shade.lusl", root=self.root).returncode, 0)

    def test_diagnostics_name_file_line_and_code(self):
        self.write("bad.lusl", "void Main()\n{\n    float32 x = 1;\n    uint32 y = x;\n}\n")
        result = run("check", "bad.lusl", root=self.root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("bad.lusl:4:16: error LUSL0504 type-mismatch:", result.stderr)

    def test_style_setting(self):
        self.write("style.lusl", "static const uint32 count = 4;\n")
        self.assertEqual(run("check", "style.lusl", root=self.root).returncode, 1)
        warned = run("check", "style.lusl", "--style", "warn", root=self.root)
        self.assertEqual(warned.returncode, 0)
        self.assertIn("warning LUSL0601", warned.stderr)
        quiet = run("check", "style.lusl", "--style", "off", root=self.root)
        self.assertEqual((quiet.returncode, quiet.stderr), (0, ""))

    def test_define_selects_configuration(self):
        self.write("mode.lusl", "#if MODE == 2\nstatic const uint32 SELECTED = 2;\n#endif\n")
        result = run("lower", "mode.lusl", "--define", "MODE=2", root=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SELECTED", result.stdout)

    def test_include_directory(self):
        self.write("_sdk/ray.luslh", '#pragma once\nextern slang "ray.slang"\n{\n    struct Ray;\n};\n')
        self.write("_sdk/ray.slang", "struct Ray { float3 DirectionWorld; };\n")
        self.write("main/miss.lusl", '#include "ray.luslh"\nvoid Use(in Ray ray)\n{\n}\n')
        self.assertEqual(run("check", "main/miss.lusl", root=self.root).returncode, 1)
        result = run("lower", "main/miss.lusl", "--include-directory", "_sdk", root=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#include "../_sdk/ray.slang"', result.stdout)

    def test_specialization_budget_and_report(self):
        self.write(
            "shade.lusl",
            "interface Material\n{\n    static const uint32 LOBE_COUNT;\n};\n"
            "concept Opaque<T> = (T : Material) && (T::LOBE_COUNT == 1);\n"
            "struct Plastic : Material\n{\n    static const uint32 LOBE_COUNT = 1;\n};\n"
            "struct Stone : Material\n{\n    static const uint32 LOBE_COUNT = 1;\n};\n"
            "template <Opaque M>\nuint32 Count(in M material)\n{\n    return M::LOBE_COUNT;\n}\n"
            "uint32 Use(in Plastic plastic, in Stone stone)\n{\n    return Count(plastic) + Count(stone);\n}\n",
        )
        report = self.root / "report.txt"
        result = run("lower", "shade.lusl", "--report", str(report), "--max-specializations", "2", root=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            report.read_text(encoding="utf-8").splitlines(),
            [
                "specialization\tshade.lusl:15\tCount<Plastic>\t_luslSpecialization0_Count",
                "specialization\tshade.lusl:15\tCount<Stone>\t_luslSpecialization1_Count",
            ],
        )
        over = run("check", "shade.lusl", "--max-specializations", "1", root=self.root)
        self.assertEqual(over.returncode, 1)
        self.assertIn("error LUSL0550 specialization-budget:", over.stderr)
        for budget in ("0", "257", "two"):
            invalid = run("check", "shade.lusl", "--max-specializations", budget, root=self.root)
            self.assertEqual(invalid.returncode, 2)
            self.assertIn("from 1 to 256", invalid.stderr)

    def test_format_check_and_write(self):
        self.write("format.lusl", "static const uint32 COUNT=4;\n")
        self.assertEqual(run("format", "--check", "format.lusl", root=self.root).returncode, 1)
        self.assertEqual(run("format", "--write", "format.lusl", root=self.root).returncode, 0)
        self.assertEqual((self.root / "format.lusl").read_text(encoding="utf-8"), "static const uint32 COUNT = 4;\n")
        self.assertEqual(run("format", "--check", "format.lusl", root=self.root).returncode, 0)

    def test_repository_sources_are_formatted(self):
        """Every .lusl and .luslh file in the repository keeps the formatter's layout."""
        if REPOSITORY is None:
            self.skipTest("pass --repository to check the repository's LUSL sources")
        root = pathlib.Path(REPOSITORY)
        sources = sorted(path.relative_to(root).as_posix() for suffix in ("*.lusl", "*.luslh")
                         for path in root.rglob(suffix)
                         if "out" not in path.relative_to(root).parts and ".git" not in path.relative_to(root).parts)
        self.assertTrue(sources)
        result = subprocess.run([LUSLC, "format", "--check", "--root", str(root), *sources],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_usage(self):
        result = subprocess.run([LUSLC], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage: luslc", result.stderr)


def main():
    global LUSLC, REPOSITORY
    parser = argparse.ArgumentParser()
    parser.add_argument("--luslc", required=True)
    parser.add_argument("--repository")
    arguments, remaining = parser.parse_known_args()
    LUSLC = arguments.luslc
    REPOSITORY = arguments.repository
    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
