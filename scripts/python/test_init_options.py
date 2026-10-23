"""Check saved target choices, dependency selection and opt-in test execution."""
from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import engine
import init_options
import init_ui


class OptionsTests(unittest.TestCase):
    def args(self, *options):
        return init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli", *options]))

    def test_personas_and_explicit_target_choices(self):
        for persona, tests, samples in (("contributor", False, True), ("application", False, False),
                                       ("browser", False, True), ("validation", True, True)):
            args = self.args("--persona", persona)
            self.assertEqual((args.with_tests, args.with_smoke_app, args.with_web_probes), (tests, samples, False))
        args = self.args("--persona", "application", "--with-tests", "--with-smoke-app")
        self.assertTrue(args.with_tests and args.with_smoke_app)
        self.assertFalse(args.run_tests)
        self.assertTrue(self.args("--run-tests").with_tests)
        self.assertTrue(self.args("--ci").with_tests)
        for options in (("--no-tests", "--run-tests"), ("--no-tests", "--validate"),
                        ("--persona", "browser", "--with-tests"), ("--with-web-probes",),
                        ("--persona", "browser", "--with-shader-probe")):
            with self.subTest(options=options), self.assertRaises(engine.EngineError):
                init_ui.validate_options(self.args(*options), engine)

    def test_saved_choices_are_scoped_and_reinitialization_replaces_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--with-tests", "--with-editor"))
            init_options.save_options(root, ("web-emscripten-development",), self.args("--persona", "browser"))
            self.assertTrue(init_options.read_options(root, engine.DEFAULT_PRESET)["LUDUS_BUILD_TESTS"])
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--persona", "application"))
            self.assertFalse(any(init_options.read_options(root, engine.DEFAULT_PRESET).values()))
            self.assertTrue(init_options.read_options(root, "web-emscripten-development")["LUDUS_BUILD_SMOKE_APP"])
            self.assertEqual(init_options.read_options(root, "untouched"), {})

    def test_invalid_saved_choices_fail_before_configuring(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = init_options.options_path(root)
            path.parent.mkdir(parents=True)
            for text in ('{', '[]', '{"linux-clang-development": {"LUDUS_BUILD_TESTS": "false"}}'):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(init_options.OptionsError):
                    init_options.read_options(root, engine.DEFAULT_PRESET)
                with patch.object(engine, "repo_root", return_value=root), patch.object(engine, "cmake_configure") as configure, \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(engine.main(["test"]), 1)
                    configure.assert_not_called()

    def test_editor_is_enabled_only_for_selected_preset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init_options.save_options(root, tuple(engine.PRESET_BUILD_TYPES), self.args("--with-editor"))
            for preset in engine.PRESET_BUILD_TYPES:
                self.assertEqual(init_options.read_options(root, preset)["LUDUS_BUILD_EDITOR"], preset == engine.DEFAULT_PRESET)
                self.assertEqual(init_options.read_options(root, preset)["LUDUS_EDITOR_RENDER_PREVIEW"],
                                 preset == engine.DEFAULT_PRESET)
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--with-editor", "--no-editor-preview"))
            self.assertTrue(init_options.read_options(root, engine.DEFAULT_PRESET)["LUDUS_BUILD_EDITOR"])
            self.assertFalse(init_options.read_options(root, engine.DEFAULT_PRESET)["LUDUS_EDITOR_RENDER_PREVIEW"])

    def test_test_dependencies_and_bootstrap_fingerprint_follow_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for enabled in (False, True):
                init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--with-tests" if enabled else "--no-tests"))
                with patch.object(engine, "run") as run, patch.object(engine, "tool_env", return_value={}):
                    engine.conan_install_for_preset(root, root / "profile", engine.DEFAULT_PRESET)
                self.assertIn(f"user.ludus:build_tests={enabled}", run.call_args.args[0])
                self.assertEqual(engine.preset_bootstrap_fingerprint(root, engine.DEFAULT_PRESET)["build_tests"], enabled)
            self.assertTrue(engine.preset_bootstrap_fingerprint(root, "untouched")["build_tests"])

    def test_rendered_editor_requires_worker_across_saved_and_direct_reconfiguration(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake unavailable")
        modules = Path(__file__).resolve().parents[2] / "cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "out/build" / engine.DEFAULT_PRESET
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.19)\nproject(EditorOptions NONE)\n'
                'option(LUDUS_BUILD_EDITOR "editor" OFF)\n'
                'option(LUDUS_BUILD_SHADER_WORKER "worker" OFF)\n'
                f'include("{modules / "EngineInitOptions.cmake"}")\n'
                f'include("{modules / "EngineEditorOptions.cmake"}")\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/worker.txt" "${LUDUS_BUILD_SHADER_WORKER}")\n',
                encoding="utf-8")

            def configure(*options):
                subprocess.run([cmake, "-S", str(root), "-B", str(build), *options],
                               check=True, capture_output=True)
                return (build / "worker.txt").read_text(encoding="utf-8")

            # Old setup files have no worker selection. A cached OFF must not
            # disable the compiler the rendered editor needs.
            for options, expected in ((("--with-editor",), "ON"),
                                      (("--with-editor", "--no-editor-preview"), "OFF"),
                                      (("--with-editor",), "ON"), (("--no-editor",), "OFF")):
                with self.subTest(saved=options):
                    init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args(*options))
                    self.assertEqual(configure("-DLUDUS_BUILD_SHADER_WORKER=OFF"), expected)

            # Direct CMake users get the same dependency without onboarding.
            self.assertEqual(configure("--fresh", "-DLUDUS_USE_INIT_OPTIONS=OFF", "-DLUDUS_BUILD_EDITOR=ON"), "ON")
            self.assertEqual(configure("-DLUDUS_EDITOR_RENDER_PREVIEW=OFF"), "OFF")
            self.assertEqual(configure("-DLUDUS_BUILD_SHADER_WORKER=ON"), "ON")
            self.assertEqual(configure("-DLUDUS_BUILD_EDITOR=OFF", "-DLUDUS_BUILD_SHADER_WORKER=OFF"), "OFF")
            # Browser previews cannot launch a native worker, even when enabled.
            self.assertEqual(configure("-DEMSCRIPTEN=ON", "-DLUDUS_BUILD_EDITOR=ON",
                                       "-DLUDUS_EDITOR_RENDER_PREVIEW=ON"), "OFF")
            self.assertEqual(configure("--fresh", "-DLUDUS_USE_INIT_OPTIONS=OFF", "-DEMSCRIPTEN=ON",
                                       "-DLUDUS_BUILD_EDITOR=ON"), "OFF")
            self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_EDITOR_RENDER_PREVIEW"), "OFF")

    def test_tests_disabled_error_precedes_build_or_configure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args())
            with patch.object(engine, "repo_root", return_value=root), patch.object(engine, "cmake_configure") as configure:
                with self.assertRaisesRegex(engine.EngineError, "--with-tests"):
                    engine.command_test(argparse.Namespace(preset=engine.DEFAULT_PRESET, label=None))
                configure.assert_not_called()

    def test_only_explicit_run_tests_executes_tests_during_setup(self):
        for options, runs in (((), 0), (("--with-tests",), 0), (("--run-tests",), 1)):
            with self.subTest(options=options), contextlib.ExitStack() as stack:
                args = self.args("--no-system-install", *options)
                for name in ("check_current_python", "run_init_preflight", "configure_system_tool_shims", "prepare_conan_artifacts",
                             "repair_existing_cmake_caches", "command_install_hooks"):
                    stack.enter_context(patch.object(engine, name))
                stack.enter_context(patch.object(engine, "load_tool_versions", return_value={"minimum": {"python": "3.10"}}))
                stack.enter_context(patch.object(engine, "command_doctor", return_value=0))
                stack.enter_context(patch.object(init_options, "save_options"))
                test = stack.enter_context(patch.object(engine, "command_test"))
                build = stack.enter_context(patch.object(engine, "command_build"))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                engine.command_init(args)
                self.assertEqual(test.call_count, runs)
                build.assert_not_called()

    def test_configure_resolves_preview_tools_after_a_fresh_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--with-editor"))
            with patch.object(engine, "request_codemodel"), \
                    patch.object(engine, "configure_needs_fresh_cache", return_value=True), \
                    patch.object(engine, "cmake", return_value=Path("cmake")), \
                    patch.object(engine, "tool_env", return_value={}), patch.object(engine, "run") as run, \
                    patch.object(engine, "macos_toolchain_identity", return_value={}):
                engine.build_dir_for_preset(root, engine.DEFAULT_PRESET).mkdir(parents=True)
                engine.cmake_configure(root, engine.DEFAULT_PRESET)
            command = run.call_args.args[0]
            self.assertIn("--fresh", command)
            self.assertIn(f"-DLUDUS_SLANG_COMPILER={root / 'out/shader-tools/slang/bin/slangc'}", command)

    def test_direct_cmake_configuration_keeps_choices_across_reconfigure(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake unavailable")
        module = Path(__file__).resolve().parents[2] / "cmake/EngineInitOptions.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args("--persona", "application"))
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.19)\nproject(Options NONE)\n'
                'option(LUDUS_BUILD_TESTS "tests" ON)\noption(LUDUS_BUILD_SMOKE_APP "samples" ON)\n'
                'option(LUDUS_EDITOR_RENDER_PREVIEW "preview" ON)\n'
                f'include("{module}")\n', encoding="utf-8")
            build = root / "out/build" / engine.DEFAULT_PRESET
            for extra in ([], ["-DLUDUS_BUILD_TESTS=ON", "-DLUDUS_BUILD_SMOKE_APP=ON", "-DLUDUS_EDITOR_RENDER_PREVIEW=ON"]):
                subprocess.run([cmake, "-S", str(root), "-B", str(build), *extra], check=True, capture_output=True)
                self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_BUILD_TESTS"), "OFF")
                self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_BUILD_SMOKE_APP"), "OFF")
                self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_EDITOR_RENDER_PREVIEW"), "OFF")
            subprocess.run([cmake, "-S", str(root), "-B", str(build), "-DLUDUS_USE_INIT_OPTIONS=OFF",
                            "-DLUDUS_BUILD_TESTS=ON"], check=True, capture_output=True)
            self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_BUILD_TESTS"), "ON")

    def test_scripted_configuration_restores_saved_choices_after_manual_opt_out(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake unavailable")
        module = Path(__file__).resolve().parents[2] / "cmake/EngineInitOptions.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = engine.build_dir_for_preset(root, engine.DEFAULT_PRESET)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.19)\nproject(Options NONE)\n'
                'option(LUDUS_BUILD_TESTS "tests" ON)\n'
                f'include("{module}")\n', encoding="utf-8")
            (root / "CMakePresets.json").write_text(json.dumps({
                "version": 3,
                "configurePresets": [{"name": engine.DEFAULT_PRESET, "binaryDir": str(build)}],
            }), encoding="utf-8")
            subprocess.run([cmake, "--preset", engine.DEFAULT_PRESET,
                            "-DLUDUS_USE_INIT_OPTIONS=OFF", "-DLUDUS_BUILD_TESTS=ON"],
                           cwd=root, check=True, capture_output=True)
            for options, expected in (((), "OFF"), (("--with-tests",), "ON"), ((), "OFF")):
                with self.subTest(options=options):
                    init_options.save_options(root, (engine.DEFAULT_PRESET,), self.args(*options))
                    with patch.object(engine, "configure_needs_fresh_cache", return_value=False), \
                            patch.object(engine, "cmake", return_value=Path(cmake)), \
                            patch.object(engine, "tool_env", return_value={}):
                        engine.cmake_configure(root, engine.DEFAULT_PRESET)
                    self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_USE_INIT_OPTIONS"), "ON")
                    self.assertEqual(engine.cmake_cache_value(build / "CMakeCache.txt", "LUDUS_BUILD_TESTS"), expected)


if __name__ == "__main__":
    unittest.main()
