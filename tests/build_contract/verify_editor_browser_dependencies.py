"""Compile the editor's browser dependency block on its actual object-library owner."""

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    source, cmake, compiler, ninja, build_directory = sys.argv[1:]
    source = Path(source)
    editor = (source / "apps/editor/CMakeLists.txt").read_text()
    match = re.search(r"^if\(EMSCRIPTEN\)\n\s+find_package\(Qt6 [^\n]*GuiPrivate\)[\s\S]*?^endif\(\)",
                      editor, re.MULTILINE)
    if match is None:
        raise RuntimeError("Cannot locate the editor browser dependency block")
    browser_block = match.group()
    version = json.loads((source / "config/editor_web_toolchain.json").read_text())["qt"]
    with tempfile.TemporaryDirectory(prefix="editor-browser-dependencies-", dir=build_directory) as temporary:
        root = Path(temporary)
        fixture = root / "source"
        fixture.mkdir()
        (fixture / "browser").mkdir()
        (fixture / "browser/files.js").write_text("// Link dependency fixture.\n")
        private_include = fixture / f"include/QtCore/{version}"
        header = private_include / "QtCore/private/qstdweb_p.h"
        header.parent.mkdir(parents=True)
        header.write_text("#pragma once\n#define QT_PRIVATE_HEADER_AVAILABLE 1\n")
        (fixture / "content.cpp").write_text(
            "#ifdef EXPECT_QT_PRIVATE_HEADER\n#include <QtCore/private/qstdweb_p.h>\n"
            "static_assert(QT_PRIVATE_HEADER_AVAILABLE == 1);\n#endif\n"
            "int ReadBrowserFile() noexcept { return 0; }\n")
        (fixture / "main.cpp").write_text(
            "int ReadBrowserFile() noexcept;\nint main() { return ReadBrowserFile(); }\n")
        package = fixture / "qt"
        package.mkdir()
        (package / "Qt6ConfigVersion.cmake").write_text(
            f'set(PACKAGE_VERSION "{version}")\n'
            'if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n'
            '  set(PACKAGE_VERSION_EXACT TRUE)\n  set(PACKAGE_VERSION_COMPATIBLE TRUE)\nendif()\n')
        (package / "Qt6Config.cmake").write_text(
            "add_library(Qt6::CorePrivate INTERFACE IMPORTED)\n"
            f'set_target_properties(Qt6::CorePrivate PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "{private_include.as_posix()}")\n'
            "add_library(Qt6::GuiPrivate INTERFACE IMPORTED)\n"
            'set_target_properties(Qt6::GuiPrivate PROPERTIES INTERFACE_LINK_LIBRARIES Qt6::CorePrivate)\n'
            "foreach(component IN LISTS Qt6_FIND_COMPONENTS)\n"
            "  set(Qt6_${component}_FOUND TRUE)\nendforeach()\n")
        legacy_block = re.sub(r"^.*target_link_libraries\(ludus_editor_content PRIVATE Qt6::CorePrivate\).*\n",
                              "", browser_block, flags=re.MULTILINE)
        for name, is_browser, block, should_build in [
                ("legacy", True, legacy_block, False),
                ("browser", True, browser_block, True),
                ("native", False, browser_block, True)]:
            (fixture / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.29)\n"
                "set(CMAKE_CXX_SCAN_FOR_MODULES OFF)\n"
                "set(CMAKE_CXX_STANDARD 23)\nset(CMAKE_CXX_EXTENSIONS OFF)\n"
                "project(EditorBrowserDependencies LANGUAGES CXX)\n"
                f"set(EMSCRIPTEN {'TRUE' if is_browser else 'FALSE'})\n"
                "add_library(ludus_editor_content OBJECT content.cpp)\n"
                "add_executable(ludus_editor main.cpp)\n"
                "target_link_libraries(ludus_editor PRIVATE ludus_editor_content)\n"
                # Keep the real dependency operations; a native linker cannot consume Emscripten's JS flags.
                "function(target_link_options)\nendfunction()\n"
                + ("target_compile_definitions(ludus_editor_content PRIVATE EXPECT_QT_PRIVATE_HEADER)\n"
                   if is_browser else "") + block + "\n")
            build = root / name
            configure = [cmake, "-S", str(fixture), "-B", str(build), "-G", "Ninja",
                         f"-DCMAKE_CXX_COMPILER={compiler}", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
            configure += ([f"-DQt6_DIR={package}"] if is_browser else ["-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=TRUE"])
            subprocess.run(configure, check=True)
            result = subprocess.run([cmake, "--build", str(build), "--target", "ludus_editor", "--parallel", "2"],
                                    capture_output=True, text=True)
            if (result.returncode == 0) != should_build:
                raise RuntimeError(f"{name}: unexpected compile result\n{result.stdout}\n{result.stderr}")
            if not should_build and "qstdweb_p.h" not in result.stdout + result.stderr:
                raise RuntimeError(f"{name}: negative control failed for an unrelated reason\n{result.stdout}")
            print(f"Verified {name}: {'compiled and linked' if should_build else 'missing private header rejected'}")


if __name__ == "__main__":
    main()
