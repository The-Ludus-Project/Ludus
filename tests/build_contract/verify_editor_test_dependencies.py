"""Build the real editor test dependency helper with optional suites and excluded targets."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    source, cmake, compiler, ninja, build_directory = sys.argv[1:]
    helper = Path(source) / "apps/editor/cmake/EditorTestDependencies.cmake"
    with tempfile.TemporaryDirectory(prefix="editor-test-dependencies-", dir=build_directory) as temporary:
        root = Path(temporary)
        fixture = root / "source"
        fixture.mkdir()
        (fixture / "main.cpp").write_text("int main() { return 0; }\n")
        (fixture / "excluded.cpp").write_text("#error This target must not enter the editor test closure\n")
        (fixture / "nested").mkdir()
        (fixture / "nested/CMakeLists.txt").write_text(
            "add_executable(ludus_foreign_tests EXCLUDE_FROM_ALL ../excluded.cpp)\n")
        (fixture / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.29)\n"
            "set(CMAKE_CXX_SCAN_FOR_MODULES OFF)\n"
            "set(CMAKE_CXX_STANDARD 23)\nset(CMAKE_CXX_EXTENSIONS OFF)\n"
            "project(EditorTestDependencies LANGUAGES CXX)\n"
            "option(SHOULD_ENABLE_OPTIONAL_SUITE \"Build another editor suite\" OFF)\n"
            "option(SHOULD_ENABLE_AGGREGATE \"Build the editor test entry point\" ON)\n"
            "if(SHOULD_ENABLE_AGGREGATE)\n"
            "  add_executable(ludus_editor_tests main.cpp)\n"
            "endif()\n"
            "add_executable(ludus_existing_tests EXCLUDE_FROM_ALL main.cpp)\n"
            "if(SHOULD_ENABLE_OPTIONAL_SUITE)\n"
            "  add_executable(ludus_new_preview_setup_tests EXCLUDE_FROM_ALL main.cpp)\n"
            "endif()\n"
            "add_executable(ludus_editor_benchmark EXCLUDE_FROM_ALL excluded.cpp)\n"
            "add_library(ludus_helper_tests OBJECT EXCLUDE_FROM_ALL excluded.cpp)\n"
            "add_subdirectory(nested)\n"
            f'include("{helper.as_posix()}")\n'
            "ludus_complete_editor_test_target(ludus_editor_tests)\n"
            "foreach(target IN ITEMS ludus_existing_tests ludus_new_preview_setup_tests)\n"
            "  if(TARGET ${target})\n"
            "    file(GENERATE OUTPUT \"${CMAKE_CURRENT_BINARY_DIR}/${target}.path\" "
            "CONTENT \"$<TARGET_FILE:${target}>\")\n"
            "  endif()\n"
            "endforeach()\n")
        for optional, aggregate in [(False, True), (True, True), (True, False)]:
            build = root / f"build-{optional}-{aggregate}"
            subprocess.run([cmake, "-S", str(fixture), "-B", str(build), "-G", "Ninja",
                            f"-DCMAKE_CXX_COMPILER={compiler}", f"-DCMAKE_MAKE_PROGRAM={ninja}",
                            f"-DSHOULD_ENABLE_OPTIONAL_SUITE={'ON' if optional else 'OFF'}",
                            f"-DSHOULD_ENABLE_AGGREGATE={'ON' if aggregate else 'OFF'}"], check=True)
            if not aggregate:
                continue
            subprocess.run([cmake, "--build", str(build), "--target", "ludus_editor_tests", "--parallel", "2"],
                           check=True)
            targets = ["ludus_existing_tests"] + (["ludus_new_preview_setup_tests"] if optional else [])
            for target in targets:
                executable = Path((build / f"{target}.path").read_text())
                if not executable.is_file():
                    raise RuntimeError(f"{target} was not built; attach every editor test executable "
                                       "to ludus_editor_tests through EditorTestDependencies.cmake")
    print("Verified editor test build closure with optional suites enabled, disabled and no aggregate")


if __name__ == "__main__":
    main()
