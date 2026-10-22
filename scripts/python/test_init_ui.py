"""Setup routing tests without downloads, package installation or a desktop."""
from __future__ import annotations

import contextlib
import io
import os
import platform
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import engine
import init_editor
import init_ui
import init_options

BACKGROUND_START_TIMEOUT_SECONDS = 10
BACKGROUND_POLL_SECONDS = 0.01


class InitTests(unittest.TestCase):
    def args(self, *options):
        return engine.make_parser().parse_args(["init", *options])

    def test_old_macos_tk_is_rejected_before_creating_window(self):
        tk = types.ModuleType("tkinter")
        tk.TkVersion = 8.5
        tk.Tk = Mock()
        tk.messagebox = Mock()
        tk.ttk = Mock()
        with patch.dict("sys.modules", {"tkinter": tk}), patch("sys.platform", "darwin"):
            with self.assertRaisesRegex(init_ui.GuiUnavailable, "Tk 8.6 or later"):
                init_ui.select_options(init_ui.apply_defaults(self.args()), engine)
        tk.Tk.assert_not_called()

    def test_persona_defaults_and_explicit_overrides(self):
        for persona, preset, single, validation in (
            ("contributor", engine.DEFAULT_PRESET, False, False),
            ("application", engine.DEFAULT_PRESET, True, False),
            ("browser", "web-emscripten-development", True, False),
            ("validation", engine.DEFAULT_PRESET, False, True),
        ):
            with self.subTest(persona=persona):
                args = init_ui.apply_defaults(self.args("--persona", persona))
                self.assertEqual((args.preset, args.preset_only, args.validate), (preset, single, validation))
        args = init_ui.apply_defaults(self.args("--persona", "application", "linux-clang-debug", "--all-presets"))
        self.assertEqual(args.preset, "linux-clang-debug")
        self.assertFalse(args.preset_only)
        self.assertTrue(args.all_presets)

    def test_browser_defaults_do_not_prepare_native_dependencies(self):
        args = init_ui.prepare_init(self.args("--cli", "web-emscripten-release"), engine)
        self.assertTrue(args.preset_only)
        self.assertFalse(args.all_presets)
        with patch("web_build.command", return_value=0) as browser, patch.object(engine, "command_init") as native:
            self.assertEqual(engine.main(["init", "--cli", "--persona", "browser"]), 0)
        browser.assert_called_once()
        native.assert_not_called()

    def test_invalid_combinations_fail_before_window_or_installation(self):
        for options in (
            ("--all-presets", "--preset-only"),
            ("--persona", "browser", "--validate"),
            ("--persona", "browser", "--ci"),
            ("--persona", "browser", "--all-presets"),
            ("--persona", "browser", "--with-rad-debugger"),
            ("--persona", "browser", "--with-editor"),
            ("linux-clang-release", "--with-editor"),
            ("invalid-preset",),
        ):
            with self.subTest(options=options), patch.object(init_ui, "select_options") as gui:
                with self.assertRaises(engine.EngineError):
                    init_ui.prepare_init(self.args("--gui", *options), engine)
                gui.assert_not_called()

    def test_macos_shader_probe_fails_before_gui_or_installation(self):
        with patch.object(init_ui.platform, "system", return_value="Darwin"), \
                patch.object(init_ui, "select_options") as gui, \
                patch.object(engine, "command_init") as install:
            with self.assertRaisesRegex(engine.EngineError, "Linux Vulkan.*--no-shader-probe"):
                init_ui.prepare_init(self.args("--gui", "macos-clang-development", "--with-shader-probe"), engine)
            gui.assert_not_called()
            install.assert_not_called()
        with patch.object(init_ui.platform, "system", return_value="Linux"):
            self.assertTrue(init_ui.shader_probe_supported("linux-clang-development"))
            self.assertFalse(init_ui.shader_probe_supported("macos-clang-development"))
            self.assertFalse(init_ui.shader_probe_supported("web-emscripten-development"))

    def test_default_gui_and_noninteractive_routing(self):
        with patch.dict(os.environ, {"DISPLAY": ":test"}, clear=True), \
                patch("sys.stdin.isatty", return_value=True), patch("sys.stdout.isatty", return_value=True):
            self.assertTrue(init_ui.wants_gui(self.args()))
            self.assertFalse(init_ui.wants_gui(self.args("--cli")))
            self.assertFalse(init_ui.wants_gui(self.args("--ci")))
            with patch.dict(os.environ, {"CI": "true"}):
                self.assertFalse(init_ui.wants_gui(self.args()))
            with patch("sys.stdin.isatty", return_value=False):
                self.assertFalse(init_ui.wants_gui(self.args()))
                self.assertTrue(init_ui.wants_gui(self.args("--gui")))
        with patch.dict(os.environ, {}, clear=True), patch("sys.platform", "linux"):
            self.assertFalse(init_ui.wants_gui(self.args()))

    def test_cancel_does_not_install(self):
        with patch.object(init_ui, "select_options", return_value=None), \
                patch.object(engine, "command_init") as install, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(engine.main(["init", "--gui"]), 0)
        install.assert_not_called()

    def test_explicit_gui_failure_does_not_install(self):
        with patch.object(init_ui, "select_options", side_effect=init_ui.GuiUnavailable("no display")), \
                patch.object(engine, "command_init") as install, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(engine.main(["init", "--gui"]), 1)
        install.assert_not_called()

    def test_default_gui_failure_falls_back_to_same_cli_options(self):
        with patch.object(init_ui, "wants_gui", return_value=True), \
                patch.object(init_ui, "select_options", side_effect=init_ui.GuiUnavailable("no Tk")), \
                contextlib.redirect_stderr(io.StringIO()) as output:
            args = init_ui.prepare_init(self.args("--no-system-install", "--with-rad-debugger"), engine)
        self.assertTrue(args.no_system_install)
        self.assertTrue(args.with_rad_debugger)
        self.assertIn("Continuing with CLI", output.getvalue())

    def test_cli_never_loads_window_and_passes_existing_flags(self):
        with patch.object(init_ui, "select_options") as gui, patch.object(engine, "command_init", return_value=0) as install:
            self.assertEqual(engine.main(["init", "--cli", "--validate", "--skip-sdk", "--no-system-install"]), 0)
        gui.assert_not_called()
        args = install.call_args.args[0]
        self.assertEqual(args.preset, engine.DEFAULT_PRESET)
        self.assertTrue(args.validate and args.skip_sdk and args.no_system_install)

    def test_editor_is_optional_and_explicitly_selectable(self):
        self.assertFalse(self.args().with_editor)
        self.assertFalse(self.args("--no-editor").with_editor)
        self.assertTrue(self.args("--with-editor").with_editor)
        with patch.object(init_editor.platform, "system", return_value="Windows"):
            with self.assertRaisesRegex(engine.EngineError, "Linux x64"):
                init_ui.prepare_init(self.args("--cli", "--with-editor"), engine)


class EditorSetupTests(unittest.TestCase):
    def setUp(self):
        for name, value in (("system", "Linux"), ("machine", "x86_64")):
            host = patch.object(init_editor.platform, name, return_value=value)
            host.start()
            self.addCleanup(host.stop)

    def args(self, *options):
        return init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli", "--with-editor", "--no-editor-preview", "linux-clang-development", *options]))

    def test_preview_tools_follow_default_opt_out_and_host(self):
        root = engine.repo_root()
        for host, tool_count in (("Linux", 2), ("Darwin", 2)):
            with self.subTest(host=host), patch.object(init_editor.platform, "system", return_value=host):
                preset = "macos-clang-development" if host == "Darwin" else "linux-clang-development"
                for options, calls in (((), 1), (("--no-editor-preview",), 0), (("--no-editor",), 0)):
                    args = init_ui.apply_defaults(engine.make_parser().parse_args(
                        ["init", "--cli", preset, *([] if "--no-editor" in options else ["--with-editor"]), *options]))
                    with patch.object(init_editor.BackgroundCommands, "start", autospec=True,
                                      side_effect=lambda job: job) as start:
                        job = init_editor.prepare_editor_preview(args, engine)
                    self.assertEqual(start.call_count, calls)
                    if calls:
                        expected = [[engine.venv_python(root), root / "scripts/shader-probe", "bootstrap"]]
                        if host == "Darwin":
                            expected.append([engine.venv_python(root), root / "scripts/bootstrap-spirv-cross"])
                        self.assertEqual(job.commands, expected)
                    else:
                        self.assertIsNone(job)
                tools = init_editor.preview_tool_args(root)
                self.assertEqual(len(tools), tool_count)
                self.assertIn("LUDUS_SLANG_COMPILER", tools[0])

    def test_background_preview_tools_replay_output_and_raise_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            succeed = [sys.executable, "-c", "print('verified slang')"]
            fail = [sys.executable, "-c", "import sys; print('bad digest'); sys.exit(3)"]
            never = [sys.executable, "-c", "print('not reached')"]
            output = io.StringIO()
            job = init_editor.BackgroundCommands([succeed, fail, never], cwd=Path(temporary), engine=engine).start()
            with contextlib.redirect_stdout(output), self.assertRaisesRegex(engine.EngineError, "exit code 3"):
                job.wait()
            self.assertIn("verified slang", output.getvalue())
            self.assertIn("bad digest", output.getvalue())
            self.assertNotIn("not reached", output.getvalue())

    def test_cancel_stops_a_running_background_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            job = init_editor.BackgroundCommands([[sys.executable, "-c", "import time; time.sleep(60)"]],
                                                 cwd=Path(temporary), engine=engine).start()
            started = time.monotonic()
            while job.process is None and time.monotonic() - started < BACKGROUND_START_TIMEOUT_SECONDS:
                time.sleep(BACKGROUND_POLL_SECONDS)
            job.cancel()
            self.assertFalse(job.thread.is_alive())
            self.assertNotEqual(job.process.returncode, 0)

    def test_editor_waits_for_started_preview_tools_before_configuring(self):
        args = init_ui.apply_defaults(engine.make_parser().parse_args(
            ["init", "--cli", "linux-clang-development", "--with-editor", "--no-system-install"]))
        calls = Mock()
        preparation = Mock()
        calls.attach_mock(preparation.wait, "wait")
        with patch.object(init_editor.cmake_targets, "query_codemodel"), \
                patch.object(engine, "cmake_configure") as configure, patch.object(engine, "cmake_build"), \
                patch.object(init_editor, "prepare_editor_preview") as prepare:
            calls.attach_mock(configure, "cmake_configure")
            init_editor.build_editor(args, engine, preparation=preparation)
        prepare.assert_not_called()
        self.assertEqual([call[0] for call in calls.mock_calls], ["wait", "cmake_configure"])

    def test_compiler_cache_is_chosen_only_for_new_build_trees(self):
        root = engine.repo_root()
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            with patch.object(engine, "build_dir_for_preset", return_value=build), \
                    patch.object(init_editor.shutil, "which", return_value="/usr/bin/ccache"), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(init_editor.compiler_cache_args(root, "linux-clang-development", engine, {}),
                                 ["-DLUDUS_ENABLE_CCACHE=ON"])
                (build / "CMakeCache.txt").write_text("LUDUS_ENABLE_CCACHE:BOOL=OFF\n")
                self.assertEqual(init_editor.compiler_cache_args(root, "linux-clang-development", engine, {}), [])
                self.assertEqual(init_editor.compiler_cache_args(
                    root, "linux-clang-development", engine, {"LUDUS_ENABLE_CCACHE": "1"}), ["-DLUDUS_ENABLE_CCACHE=ON"])
                self.assertEqual(init_editor.compiler_cache_args(
                    root, "linux-clang-development", engine, {"LUDUS_ENABLE_CCACHE": "0"}), ["-DLUDUS_ENABLE_CCACHE=OFF"])
            with patch.object(engine, "build_dir_for_preset", return_value=build / "new"), \
                    patch.object(init_editor.shutil, "which", return_value=None):
                self.assertEqual(init_editor.compiler_cache_args(root, "linux-clang-development", engine, {}), [])

    def test_default_editor_configures_rendering_and_pinned_tools(self):
        args = init_ui.apply_defaults(engine.make_parser().parse_args(
            ["init", "--cli", "linux-clang-development", "--with-editor", "--no-system-install"]))
        with patch.object(engine, "run"), patch.object(init_editor.cmake_targets, "query_codemodel"), \
                patch.object(engine, "cmake_configure") as configure, patch.object(engine, "cmake_build"):
            init_editor.setup_editor(args, engine)
        self.assertEqual(configure.call_args.args[2],
                         ["-DLUDUS_BUILD_EDITOR=ON", "-DLUDUS_EDITOR_RENDER_PREVIEW=ON",
                          *init_editor.preview_tool_args(engine.repo_root())])

    def test_existing_packages_are_not_installed_again(self):
        with patch.object(engine, "host_supports_apt_install", return_value=True), \
                patch.object(engine, "capture_command_quiet", return_value=(0, "install ok installed")), \
                patch.object(engine, "run") as run, patch.object(init_editor.cmake_targets, "query_codemodel") as query, \
                patch.object(engine, "cmake_configure") as configure, patch.object(engine, "cmake_build") as build:
            init_editor.setup_editor(self.args(), engine)
        run.assert_not_called()
        self.assertEqual(query.call_args.args[1], "ludus-editor")
        self.assertEqual(configure.call_args.args[2], ["-DLUDUS_BUILD_EDITOR=ON", "-DLUDUS_EDITOR_RENDER_PREVIEW=OFF"])
        self.assertEqual(build.call_args.args[2], ["--target", "ludus_editor"])

    def test_installs_only_missing_opted_in_packages(self):
        with patch.object(engine, "host_supports_apt_install", return_value=True), \
                patch.object(engine, "capture_command_quiet", side_effect=[(0, "install ok installed"), (1, "missing")]), \
                patch.object(engine, "sudo_command_prefix", return_value=["sudo"]), \
                patch.object(engine, "run") as run, patch.object(init_editor.cmake_targets, "query_codemodel"), \
                patch.object(engine, "cmake_configure"), patch.object(engine, "cmake_build"):
            init_editor.setup_editor(self.args(), engine)
        self.assertEqual([call.args[0] for call in run.call_args_list],
                         [["sudo", "apt-get", "update"], ["sudo", "apt-get", "install", "-y", "qt6-wayland"]])

    def test_sanitizer_editor_setup_keeps_the_selected_profile_and_existing_qt(self):
        args = init_ui.prepare_init(engine.make_parser().parse_args([
            "init", "--cli", "linux-clang-asan-ubsan", "--preset-only", "--with-editor",
            "--with-tests", "--no-system-install", "--no-editor-preview"]), engine)
        with patch.object(engine, "run") as run, \
                patch.object(init_editor.cmake_targets, "query_codemodel"), \
                patch.object(engine, "cmake_configure") as configure, \
                patch.object(engine, "cmake_build") as build:
            init_editor.setup_editor(args, engine)
        run.assert_not_called()
        self.assertTrue(args.with_tests)
        configure.assert_called_once_with(engine.repo_root(), "linux-clang-asan-ubsan", ["-DLUDUS_BUILD_EDITOR=ON", "-DLUDUS_EDITOR_RENDER_PREVIEW=OFF"],
                                          reuse_current_cache=True)
        build.assert_called_once_with(engine.repo_root(), "linux-clang-asan-ubsan", ["--target", "ludus_editor"])

    def test_no_system_install_uses_existing_qt_and_propagates_configure_failure(self):
        with patch.object(engine, "capture_command_quiet") as query, patch.object(engine, "run") as run, \
                patch.object(init_editor.cmake_targets, "query_codemodel"), \
                patch.object(engine, "cmake_configure", side_effect=engine.EngineError("Qt not found")), \
                patch.object(engine, "cmake_build") as build:
            with self.assertRaisesRegex(engine.EngineError, "Qt not found"):
                init_editor.setup_editor(self.args("--no-system-install"), engine)
        query.assert_not_called()
        run.assert_not_called()
        build.assert_not_called()

    def test_init_prepares_preview_after_host_tools_and_before_cache_repair(self):
        args = init_ui.apply_defaults(engine.make_parser().parse_args([
            "init", "--cli", "linux-clang-development", "--with-editor", "--no-system-install"]))
        calls = Mock()

        def prepare_conan(*_arguments, after_host_tools, **_options):
            calls.host_tools()
            after_host_tools()
            calls.conan()

        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(engine, "prepare_conan_artifacts", side_effect=prepare_conan))
            for module, name in ((init_editor, "prepare_editor_preview"),
                                 (engine, "repair_existing_cmake_caches"), (init_editor, "setup_editor")):
                calls.attach_mock(stack.enter_context(patch.object(module, name)), name)
            for name in ("check_current_python", "configure_system_tool_shims", "command_install_hooks"):
                stack.enter_context(patch.object(engine, name))
            stack.enter_context(patch.object(engine, "load_tool_versions", return_value={"minimum": {"python": "3.10"}}))
            stack.enter_context(patch.object(engine, "command_doctor", return_value=0))
            stack.enter_context(patch.object(init_options, "save_options"))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            engine.command_init(args)
        names = [call[0] for call in calls.mock_calls if "." not in call[0]]
        # Shader tools start once the managed Python exists and download while Conan runs.
        self.assertEqual(names, ["host_tools", "prepare_editor_preview", "conan", "repair_existing_cmake_caches",
                                 "setup_editor"])
        self.assertIs(calls.setup_editor.call_args.kwargs["preparation"], calls.prepare_editor_preview.return_value)
        calls.prepare_editor_preview.return_value.cancel.assert_called_once_with()

    def test_init_calls_editor_setup_only_when_selected(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), contextlib.ExitStack() as stack:
                args = init_ui.apply_defaults(engine.make_parser().parse_args([
                    "init", "--cli", "--no-system-install", "--with-editor" if enabled else "--no-editor"]))
                for name in ("check_current_python", "configure_system_tool_shims",
                             "repair_existing_cmake_caches", "command_install_hooks"):
                    stack.enter_context(patch.object(engine, name))
                stack.enter_context(patch.object(
                    engine, "prepare_conan_artifacts",
                    side_effect=lambda *_arguments, after_host_tools, **_options: after_host_tools()))
                stack.enter_context(patch.object(engine, "load_tool_versions", return_value={"minimum": {"python": "3.10"}}))
                stack.enter_context(patch.object(engine, "command_doctor", return_value=0))
                stack.enter_context(patch.object(init_options, "save_options"))
                preview = stack.enter_context(patch.object(init_editor, "prepare_editor_preview"))
                setup = stack.enter_context(patch.object(init_editor, "setup_editor"))
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                self.assertEqual(engine.command_init(args), 0)
                self.assertEqual(setup.call_count, int(enabled))
                preview.assert_called_once_with(args, engine)


@unittest.skipUnless(os.environ.get("LUDUS_TEST_GUI") == "1", "requires a desktop display")
class WindowTests(unittest.TestCase):
    def select(self, action, *options):
        import tkinter as tk
        real_tk = tk.Tk
        errors = []

        def create_window(*arguments, **options):
            window = real_tk(*arguments, **options)

            def on_error(*error):
                errors.append(error)
                window.destroy()

            window.report_callback_exception = on_error

            def drive():
                def descendants(widget):
                    children = widget.winfo_children()
                    return children + [child for widget in children for child in descendants(widget)]
                action(window, descendants(window))

            window.after(100, drive)
            return window

        args = init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--gui", *options]))
        with patch.object(tk, "Tk", side_effect=create_window):
            result = init_ui.select_options(args, engine)
        self.assertEqual(errors, [])
        return result

    def test_initialize_preserves_supplied_options(self):
        def accept(window, widgets):
            button = next(widget for widget in widgets if widget.winfo_class() == "TButton"
                          and widget.cget("text") == "Initialize")
            self.assertLessEqual(button.winfo_rootx() + button.winfo_width(),
                                 window.winfo_rootx() + window.winfo_width())
            self.assertLessEqual(button.winfo_rooty() + button.winfo_height(),
                                 window.winfo_rooty() + window.winfo_height())
            button.invoke()

        linux = platform.system() == "Linux"
        preset = engine.NATIVE_PRESET_PREFIX + "-debug"
        options = [preset, "--preset-only", "--no-system-install", "--validate", "--skip-sdk"]
        if linux:
            options.extend(["--with-rad-debugger", "--with-editor"])
        result = self.select(accept, *options)
        self.assertEqual(result.preset, preset)
        self.assertTrue(result.preset_only and result.no_system_install)
        self.assertTrue(result.validate and result.skip_sdk)
        self.assertEqual(result.with_rad_debugger, linux)
        self.assertEqual(result.with_editor, linux)

    def test_native_editor_checkbox_remains_selectable(self):
        def accept(window, widgets):
            editor = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                          and widget.cget("text").startswith("Build Ludus editor"))
            self.assertFalse(editor.instate(["disabled"]))
            editor.invoke()
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Initialize").invoke()
        result = self.select(accept, engine.NATIVE_PRESET_PREFIX + "-development")
        self.assertTrue(result.with_editor)

    def test_advanced_setup_can_disable_default_editor_preview(self):
        def accept(window, widgets):
            editor = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                          and widget.cget("text").startswith("Build Ludus editor"))
            editor.invoke()
            preview = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                           and widget.cget("text").startswith("Include 3D preview"))
            self.assertTrue(window.getvar(preview.cget("variable")))
            self.assertFalse(preview.instate(["disabled"]))
            self.assertFalse(preview.winfo_ismapped())
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Advanced setup").invoke()
            window.update_idletasks()
            self.assertTrue(preview.winfo_ismapped())
            preview.invoke()
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Initialize").invoke()
        result = self.select(accept, engine.NATIVE_PRESET_PREFIX + "-development")
        self.assertTrue(result.with_editor)
        self.assertFalse(result.with_editor_preview)

    def test_shader_probe_checkbox_follows_host_and_selected_preset(self):
        def accept(window, widgets):
            probe = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                         and widget.cget("text").startswith("Include shader feasibility probe"))
            supported = platform.system() == "Linux"
            self.assertEqual(probe.instate(["disabled"]), not supported)
            probe.invoke()
            self.assertEqual(bool(window.getvar(probe.cget("variable"))), supported)
            presets = [widget for widget in widgets if widget.winfo_class() == "TCombobox"][1]
            presets.set("web-emscripten-development")
            presets.event_generate("<<ComboboxSelected>>")
            window.update_idletasks()
            self.assertTrue(probe.instate(["disabled"]))
            self.assertFalse(window.getvar(probe.cget("variable")))
            presets.set(engine.NATIVE_PRESET_PREFIX + "-development")
            presets.event_generate("<<ComboboxSelected>>")
            window.update_idletasks()
            self.assertEqual(probe.instate(["disabled"]), not supported)
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Initialize").invoke()
        result = self.select(accept, engine.NATIVE_PRESET_PREFIX + "-development")
        self.assertFalse(result.with_shader_probe)

    def test_browser_workflow_disables_native_options(self):
        def accept(window, widgets):
            workflow = next(widget for widget in widgets if widget.winfo_class() == "TCombobox")
            workflow.set("Browser developer")
            workflow.event_generate("<<ComboboxSelected>>")
            window.update_idletasks()
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Initialize").invoke()

        result = self.select(accept, "--with-rad-debugger", "--with-editor", "--validate")
        self.assertEqual(result.preset, "web-emscripten-development")
        self.assertTrue(result.preset_only)
        self.assertFalse(result.all_presets or result.with_rad_debugger or result.with_editor or result.validate or result.ci)

    def test_tests_are_opt_in_and_running_implies_building(self):
        def accept(window, widgets):
            tests = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                         and widget.cget("text").startswith("Include native test"))
            self.assertFalse(window.getvar(tests.cget("variable")))
            run = next(widget for widget in widgets if widget.winfo_class() == "TCheckbutton"
                       and widget.cget("text") == "Build and run native tests after setup")
            run.invoke()
            self.assertTrue(window.getvar(tests.cget("variable")))
            next(widget for widget in widgets if widget.winfo_class() == "TButton"
                 and widget.cget("text") == "Initialize").invoke()
        result = self.select(accept, "--persona", "application")
        self.assertTrue(result.with_tests and result.run_tests)
        self.assertFalse(result.with_smoke_app or result.with_editor or result.with_shader_probe)

    def test_enter_initializes_and_escape_cancels(self):
        result = self.select(lambda window, _widgets: window.event_generate("<Return>"),
                             engine.NATIVE_PRESET_PREFIX + "-development")
        self.assertIsNotNone(result)
        self.assertIsNone(self.select(lambda window, _widgets: window.event_generate("<Escape>")))

    def test_close_cancels(self):
        self.assertIsNone(self.select(lambda window, _widgets: window.destroy()))


if __name__ == "__main__":
    unittest.main()
