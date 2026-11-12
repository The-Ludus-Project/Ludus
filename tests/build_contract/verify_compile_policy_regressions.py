"""Probe exception classification with real compiler macros, including test-only MOC objects."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler, build_directory = sys.argv[1:]
    verifier = Path(__file__).with_name("verify_compile_policy.py")
    cases = [
        ("ludus_policy_tests", "tests/probe.cpp", True, True, []),
        ("ludus_editor_test_main", "apps/editor/tests/main.cpp", True, True, []),
        ("ludus_editor_test_main", "apps/editor/ludus_editor_test_main_autogen/mocs_compilation.cpp", True, True, []),
        ("ludus_engine", "modules/probe.cpp", False, True, []),
        ("ludus_assert_child", "tests/probe.cpp", False, True, []),
        ("ludus_engine", "modules/probe.cpp", True, False, []),
        ("ludus_editor_test_main", "modules/probe.cpp", True, False, []),
        ("ludus_engine", "modules/probe.cpp", True, False, ["-ICMakeFiles/ludus_decoy_tests.dir/include"]),
        ("ludus_engine", "modules/probe.cpp", True, False, ["-DDECOY=CMakeFiles/ludus_decoy_tests.dir/"]),
        ("ludus_policy_tests", "tests/probe.cpp", True, False, ["-fno-exceptions"]),
    ]
    with tempfile.TemporaryDirectory(prefix="compile-policy-", dir=build_directory) as temporary:
        root = Path(temporary)
        for index, (target, source, has_exceptions, should_pass, extra) in enumerate(cases):
            output = f"CMakeFiles/{target}.dir/probe.cpp.o"
            arguments = [compiler, "-std=c++23", "-fno-exceptions"]
            if has_exceptions:
                arguments.append("-fexceptions")
            arguments += [*extra, "-o", output, "-c", source]
            entry = {"directory": str(root), "file": str(root / source), "arguments": arguments}
            database = root / "compile_commands.json"
            database.write_text(json.dumps([entry]))
            result = subprocess.run([sys.executable, str(verifier), str(database)],
                                    text=True, capture_output=True, check=False)
            if (result.returncode == 0) != should_pass:
                raise RuntimeError(f"Exception policy case {index} ({target}, {source}) failed:\n"
                                   f"{result.stdout}{result.stderr}")
            if not should_pass and "Wrong exception policy" not in result.stderr:
                raise RuntimeError(f"Policy case {index} rejected for the wrong reason: {result.stderr}")
        shared_arguments = [compiler, "-std=c++23", "-fexceptions"]
        entries = [
            {
                "directory": str(root),
                "file": str(root / f"tests/{name}.cpp"),
                "arguments": [*shared_arguments, "-o", f"CMakeFiles/ludus_policy_tests.dir/{name}.cpp.o",
                              "-c", f"tests/{name}.cpp"],
            }
            for name in ("first", "second")
        ]
        database.write_text(json.dumps(entries))
        result = subprocess.run([sys.executable, str(verifier), str(database)],
                                text=True, capture_output=True, check=True)
        if "2 compilation commands (compiler probes: 1)" not in result.stdout:
            raise RuntimeError(f"Identical commands did not share a compiler probe: {result.stdout}")

        entries[1]["output"] = "CMakeFiles/ludus_engine.dir/second.cpp.o"
        database.write_text(json.dumps(entries))
        result = subprocess.run([sys.executable, str(verifier), str(database)],
                                text=True, capture_output=True, check=False)
        if result.returncode == 0 or f"Wrong exception policy: {entries[1]['file']}" not in result.stderr:
            raise RuntimeError(f"Shared compiler probe skipped a source policy check: {result.stderr}")
    print(f"Verified {len(cases) + 2} exception-policy compiler regressions")


if __name__ == "__main__":
    main()
