"""Regression tests for tools/check_target_macros.py."""

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_target_macros", ROOT / "tools" / "check_target_macros.py")
target_macros = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(target_macros)


def write(root: Path, files: dict[str, str]) -> None:
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


class TargetMacroTests(unittest.TestCase):
    def test_reports_each_raw_directive_with_its_line(self) -> None:
        source = (
            "#include <ludus/foundation/base/config.h>\n"
            "\n"
            "#if defined(__linux__) || defined(__APPLE__)\n"
            "#    include <unistd.h>\n"
            "#endif\n"
            "  #  ifdef _MSC_VER\n"
            "#endif\n"
            "#elif defined(__EMSCRIPTEN__)\n"
        )
        self.assertEqual(
            target_macros.raw_identity_lines(source),
            [(3, "__linux__"), (6, "_MSC_VER"), (8, "__EMSCRIPTEN__")],
        )

    def test_finds_a_directive_split_across_continuations_on_its_first_line(self) -> None:
        source = "int value = 0;\n#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || \\\n    defined(__APPLE__)\n#endif\n#ifdef _WIN32\n#endif\n"
        self.assertEqual(target_macros.raw_identity_lines(source), [(2, "__APPLE__"), (5, "_WIN32")])

    def test_accepts_foundation_detection_and_non_directive_mentions(self) -> None:
        source = (
            "#if LUDUS_TARGET_OS == LUDUS_OS_LINUX || LUDUS_TARGET_OS == LUDUS_OS_MACOS\n"
            "#endif\n"
            "#if LUDUS_TARGET_COMPILER == LUDUS_CXX_MSVC\n"
            "#endif\n"
            "// Mirrors __linux__ socket behaviour.\n"
            "const char* name = \"__APPLE__\";\n"
            "#define LUDUS_PRIVATE_MSC_VERSION 0\n"
            "#if defined(__linux_like__)\n"
            "#endif\n"
        )
        self.assertEqual(target_macros.raw_identity_lines(source), [])

    def test_scans_engine_and_editor_sources_but_not_detectors_or_tests(self) -> None:
        raw = "#if defined(__linux__)\n#endif\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                {
                    "modules/tooling/shader_compiler/src/client.cpp": "\n" + raw,
                    "apps/editor/src/window.mm": raw,
                    "apps/editor/src/internal/view.hpp": raw,
                    "modules/foundation/base/include/ludus/foundation/base/config.h": raw,
                    "modules/foundation/base/include/ludus/foundation/base/compiler.h": raw,
                    "modules/a/tests/a_tests.cpp": raw,
                    "modules/a/src/notes.txt": raw,
                    "tools/shader-worker/main.cpp": raw,
                },
            )
            failures = target_macros.violations(root)
        self.assertEqual(
            [failure.split(": ", 1)[0] for failure in failures],
            [
                "modules/tooling/shader_compiler/src/client.cpp:2",
                "apps/editor/src/internal/view.hpp:1",
                "apps/editor/src/window.mm:1",
            ],
        )
        self.assertIn("tests __linux__; use FoundationBase detection", failures[0])

    def test_repository_is_clean(self) -> None:
        self.assertEqual(target_macros.violations(ROOT), [])


if __name__ == "__main__":
    unittest.main()
