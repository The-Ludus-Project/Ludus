"""Real CMake regressions for stale/missing host dependencies, including SDK use."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(sys.argv[1]).resolve()
CMAKE = sys.argv[2]
NINJA = sys.argv[3]
del sys.argv[1:]


class DependencyDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "consumer with spaces"
        self.root.mkdir()
        self.cmake = self.root / "installed SDK/cmake"
        self.cmake.mkdir(parents=True)
        for name in ("LudusDependencies", "LudusShaders", "LudusRendererShaders", "LudusReflection",
                     "EngineSetupPreflight", "EngineDependencyPreflight", "LudusBehavior", "LuauProfile"):
            shutil.copy2(SOURCE / f"cmake/{name}.cmake", self.cmake)
        self.shader = self.root / "sample.slang"
        (self.root / "dummy.cpp").write_text("int fixture() { return 0; }\n")
        self.shader.write_text("// fixture\n")
        self.tool = self.root / "tool with spaces"
        self.tool.write_text("#!/bin/sh\necho fixture-tool\n")
        self.tool.chmod(0o755)
        (self.cmake / "reflection").mkdir()
        (self.cmake / "reflection/generate.py").write_text("# fixture\n")

    def configure(self, body, *settings):
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.29)\nproject(Fixture CXX)\n'
            f'include("{self.cmake}/LudusDependencies.cmake")\n' + body)
        return subprocess.run(
            [CMAKE, "-S", str(self.root), "-B", str(self.root / "build"),
             "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={NINJA}", *settings],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
            env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ.get("PATH", "")})

    def shader_body(self, system="Darwin", extra=""):
        return (f'set(CMAKE_SYSTEM_NAME "{system}")\n{extra}\n'
                f'include("{self.cmake}/LudusShaders.cmake")\n'
                'add_library(sample STATIC dummy.cpp)\n'
                f'ludus_compile_shader(TARGET sample NAME sample SOURCE "{self.shader}" '
                'VERTEX vertexMain FRAGMENT fragmentMain)\n')

    def assert_failure(self, result, *messages):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Ludus setup problem", result.stdout)
        self.assertIn("How to fix", result.stdout)
        self.assertIn("CMake Configure", result.stdout)
        for message in messages:
            self.assertIn(message, result.stdout)
        self.assertFalse((self.root / "out").exists())

    def test_unset_missing_directory_and_non_executable_shader_compiler(self):
        for value in ("", str(self.root / "removed/slangc"), str(self.root)):
            with self.subTest(value=value):
                result = self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={value}")
                self.assert_failure(result, "LUDUS_SLANG_COMPILER", "./scripts/shader-probe bootstrap")
        self.tool.chmod(0o644)
        self.assert_failure(self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={self.tool}"),
                            "non-executable")

    def test_existing_but_unrunnable_tool(self):
        self.tool.write_text("#!/bin/sh\necho missing-library >&2\nexit 7\n")
        self.assert_failure(self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={self.tool}"),
                            "Cannot run", "missing-library", "permissions and shared libraries")

    def test_valid_metal_setup_and_repeated_configure_need_no_spirv_tools(self):
        for _ in range(2):
            result = self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={self.tool}")
            self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.root / "out").exists())
        # Removed tools in an existing cache must fail on reconfigure.
        self.tool.unlink()
        self.assert_failure(self.configure(self.shader_body()), "LUDUS_SLANG_COMPILER")

    def test_command_name_is_resolved_to_absolute_dependency(self):
        # CMake uses --version, whereas Slang uses -version. Test the shared helper.
        result = self.configure('set(HOST_TOOL "tool with spaces")\n'
                                'ludus_require_tool(HOST_TOOL "fixture" "restore CMake" --version)\n'
                                'if(NOT IS_ABSOLUTE "${HOST_TOOL}")\nmessage(FATAL_ERROR "relative tool")\nendif()\n')
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_missing_spirv_validator(self):
        self.assert_failure(self.configure(self.shader_body("Linux"),
                            f"-DLUDUS_SLANG_COMPILER={self.tool}",
                            f"-DLUDUS_SPIRV_VALIDATOR={self.root}/gone-spirv-val"),
                            "LUDUS_SPIRV_VALIDATOR", "./scripts/shader-probe bootstrap")

    def test_unsupported_native_probe_fails_before_dependency_discovery(self):
        for system in ("Darwin", "Emscripten"):
            with self.subTest(system=system):
                body = (f'set(CMAKE_SYSTEM_NAME "{system}")\n'
                        'set(LUDUS_BUILD_SHADER_PROBE ON)\n'
                        f'include("{self.cmake}/EngineDependencyPreflight.cmake")\n')
                result = self.configure(body)
                self.assert_failure(result, "Shader feasibility probe",
                                    "Linux Vulkan", "./init.sh --no-shader-probe", "Cornell box")
                self.assertNotIn("volkConfig.cmake", result.stdout)

    def test_browser_cross_tool_and_provenance(self):
        args = (f"-DLUDUS_SLANG_COMPILER={self.tool}", f"-DLUDUS_SPIRV_VALIDATOR={self.tool}")
        self.assert_failure(self.configure(self.shader_body("Emscripten", "set(EMSCRIPTEN TRUE)"), *args),
                            "LUDUS_SPIRV_CROSS", "./scripts/bootstrap-spirv-cross")
        self.assert_failure(self.configure(self.shader_body("Emscripten", "set(EMSCRIPTEN TRUE)"),
                            *args, f"-DLUDUS_SPIRV_CROSS={self.tool}"), "provenance", ".build.json")
        Path(str(self.tool) + ".build.json").write_text("{}")
        result = self.configure(self.shader_body("Emscripten", "set(EMSCRIPTEN TRUE)"),
                                *args, f"-DLUDUS_SPIRV_CROSS={self.tool}")
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_lusl_source_needs_a_host_luslc_outside_the_source_tree(self):
        # Without a luslc target or an SDK bin/luslc (as in an Emscripten build), the helper
        # requires the host compiler that scripts/bootstrap-luslc builds.
        self.shader = self.root / "sample.lusl"
        self.shader.write_text("// fixture\n")
        args = (f"-DLUDUS_SLANG_COMPILER={self.tool}", f"-DLUDUS_SPIRV_VALIDATOR={self.tool}",
                f"-DLUDUS_SPIRV_CROSS={self.tool}")
        Path(str(self.tool) + ".build.json").write_text("{}")
        body = self.shader_body("Emscripten", "set(EMSCRIPTEN TRUE)\nset(CMAKE_CROSSCOMPILING TRUE)")
        for value in ("", str(self.root / "removed/luslc")):
            with self.subTest(value=value):
                self.assert_failure(self.configure(body, *args, f"-DLUDUS_LUSLC={value}"),
                                    "LUDUS_LUSLC", "./scripts/bootstrap-luslc", "sample.lusl")
        result = self.configure(body, *args, f"-DLUDUS_LUSLC={self.tool}")
        self.assertEqual(result.returncode, 0, result.stdout)
        # A native consumer of an SDK without bin/luslc takes the same setting.
        native = self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={self.tool}",
                                f"-DLUDUS_LUSLC={self.tool}")
        self.assertEqual(native.returncode, 0, native.stdout)

    def test_cross_sdk_supplies_its_host_luslc_on_the_same_host(self):
        # A browser SDK installs the host luslc it was built with and the host it runs on, so its
        # consumers configure without LUDUS_LUSLC (batch 45839408533); another host still needs it.
        package = self.root / "web sdk/lib/cmake/Ludus"
        package.mkdir(parents=True)
        for path in self.cmake.iterdir():
            if path.is_file():
                shutil.copy2(path, package)
        (self.root / "web sdk/bin").mkdir()
        shutil.copy2(self.tool, self.root / "web sdk/bin/luslc")
        self.shader = self.root / "sample.lusl"
        self.shader.write_text("// fixture\n")
        Path(str(self.tool) + ".build.json").write_text("{}")
        args = (f"-DLUDUS_SLANG_COMPILER={self.tool}", f"-DLUDUS_SPIRV_VALIDATOR={self.tool}",
                f"-DLUDUS_SPIRV_CROSS={self.tool}")
        self.cmake = package
        body = self.shader_body("Emscripten", "set(EMSCRIPTEN TRUE)\nset(CMAKE_CROSSCOMPILING TRUE)")
        record = package / "LudusHostLuslc.cmake"
        with self.subTest(record="absent"):
            self.assert_failure(self.configure(body, *args), "LUDUS_LUSLC", "./scripts/bootstrap-luslc")
        record.write_text('set(LUDUS_SDK_LUSLC_HOST "OtherSystem-other")\n')
        with self.subTest(record="other host"):
            self.assert_failure(self.configure(body, *args), "LUDUS_LUSLC", "./scripts/bootstrap-luslc")
        record.write_text('set(LUDUS_SDK_LUSLC_HOST "${CMAKE_HOST_SYSTEM_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR}")\n')
        with self.subTest(record="this host"):
            result = self.configure(body, *args)
            self.assertEqual(result.returncode, 0, result.stdout)
            commands = (self.root / "build/build.ninja").read_text()
            self.assertIn(str(self.root / "web sdk/bin/luslc"), commands)

    def test_missing_shader_source(self):
        self.shader.unlink()
        self.assert_failure(self.configure(self.shader_body(), f"-DLUDUS_SLANG_COMPILER={self.tool}"),
                            "Restore the shader source")

    def test_missing_python_has_recovery(self):
        self.assert_failure(self.configure('ludus_require_python("fixture generation")\n',
                            f"-DPython3_EXECUTABLE={self.root}/gone-python"), "Python 3.10", "Python3_EXECUTABLE")

    def test_explicit_stale_build_tool_paths(self):
        for variable in ("CMAKE_MAKE_PROGRAM", "CMAKE_CXX_COMPILER", "CMAKE_C_COMPILER"):
            with self.subTest(variable=variable):
                # Include before project(), just as the engine does.
                (self.root / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.29)\n'
                    f'set({variable} "{self.root}/removed-tool")\n'
                    f'include("{self.cmake}/EngineSetupPreflight.cmake")\nproject(Fixture NONE)\n')
                result = subprocess.run([CMAKE, "-S", str(self.root), "-B", str(self.root / "build")],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
                self.assert_failure(result, variable, "./init.sh")

    def test_optional_features_do_not_require_tools_or_packages(self):
        result = self.configure(f'include("{self.cmake}/LudusShaders.cmake")\n'
                                f'include("{self.cmake}/LudusReflection.cmake")\n'
                                'set(CMAKE_SYSTEM_NAME Emscripten)\nset(EMSCRIPTEN TRUE)\nset(LUDUS_BUILD_TESTS OFF)\n'
                                f'include("{self.cmake}/EngineDependencyPreflight.cmake")\n')
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_missing_test_package_has_setup_and_custom_prefix_recovery(self):
        self.assert_failure(self.configure('set(CMAKE_SYSTEM_NAME Darwin)\nset(LUDUS_BUILD_TESTS ON)\n'
                            f'include("{self.cmake}/EngineDependencyPreflight.cmake")\n',
                            "-DCMAKE_DISABLE_FIND_PACKAGE_Catch2=TRUE"), "Catch2 3", "--with-tests", "Catch2_DIR")

    def test_missing_vulkan_package_has_setup_recovery(self):
        self.assert_failure(self.configure('set(CMAKE_SYSTEM_NAME Linux)\nset(LUDUS_BUILD_TESTS OFF)\n'
                            f'include("{self.cmake}/EngineDependencyPreflight.cmake")\n',
                            "-DCMAKE_DISABLE_FIND_PACKAGE_volk=TRUE"), "volk", "./init.sh", "volk_DIR")

    def test_missing_text_packages_have_recovery(self):
        package_dir = self.root / "packages"
        package_dir.mkdir()
        (package_dir / "freetypeConfig.cmake").write_text("set(freetype_FOUND TRUE)\n")
        for package in ("freetype", "harfbuzz"):
            with self.subTest(package=package):
                result = self.configure('set(CMAKE_SYSTEM_NAME Darwin)\nset(LUDUS_BUILD_TESTS OFF)\n'
                                        f'include("{self.cmake}/EngineDependencyPreflight.cmake")\n',
                                        f"-DCMAKE_DISABLE_FIND_PACKAGE_{package}=TRUE",
                                        f"-Dfreetype_DIR={package_dir}")
                self.assert_failure(result, "Text rendering", package, f"{package}_DIR")
                # Don't retain the first subcase's disabled package.
                self.configure(' ', f"-DCMAKE_DISABLE_FIND_PACKAGE_{package}=FALSE")

    def test_missing_behavior_tools_and_inputs(self):
        body = (f'include("{self.cmake}/LudusBehavior.cmake")\n'
                f'ludus_cook_behaviors(NAME fixture CONTRACT "{self.root}/contract.json" '
                f'PACKAGE "{self.root}/package.json" PROFILE "{self.root}/profile.json" '
                f'COMPILER "{self.root}/removed-compiler" ANALYZER "{self.tool}")\n')
        self.assert_failure(self.configure(body), "BEHAVIOR_COMPILER", "matching SDK host tools")
        self.assert_failure(self.configure(body.replace(str(self.root / "removed-compiler"), str(self.tool))),
                            "contract.json", "correct CONTRACT")

    def test_missing_editor_qt_is_actionable(self):
        editor = (SOURCE / "apps/editor/CMakeLists.txt").read_text()
        # Stop after discovery so this negative fixture needs no editor sources.
        discovery = editor.split("# QT_NO_KEYWORDS", 1)[0]
        self.assert_failure(self.configure('set(CMAKE_SYSTEM_NAME Linux)\n'
                            'set(CMAKE_SYSTEM_PROCESSOR x86_64)\n' + discovery,
                            "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=TRUE"), "Qt 6.4+", "--with-editor", "Qt6_DIR")

    def test_missing_luau_profile_has_bootstrap_recovery(self):
        self.assert_failure(self.configure(f'include("{self.cmake}/LuauProfile.cmake")\n'
                            'ludus_prepare_luau()\n'), "Luau scripting", "./scripts/luau-probe bootstrap")

    def test_missing_reflection_generator_header_and_baseline(self):
        schema = self.root / "schema.json"
        schema.write_text('{"header": "native.hpp"}')
        body = (f'include("{self.cmake}/LudusReflection.cmake")\n'
                f'ludus_generate_reflection(TARGET schema SCHEMA "{schema}")\n')
        self.assert_failure(self.configure(body), "native.hpp", "native header")
        (self.root / "native.hpp").write_text("// fixture\n")
        self.assert_failure(self.configure(body.replace('SCHEMA ', f'BASELINE "{self.root}/missing-baseline.json" SCHEMA ')),
                            "Restore the baseline", "missing-baseline.json")
        (self.cmake / "reflection/generate.py").unlink()
        self.assert_failure(self.configure(body), "reinstall the SDK", "generate.py")

    def test_missing_reflection_schema_is_actionable(self):
        self.assert_failure(self.configure(f'include("{self.cmake}/LudusReflection.cmake")\n'
                            f'ludus_generate_reflection(TARGET schema SCHEMA "{self.root}/missing.json")\n'),
                            "Restore the schema", "missing.json")


if __name__ == "__main__":
    unittest.main()
