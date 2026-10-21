"""Setup progress markers, their parser and the progress window."""
from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import textwrap
import time
import unittest
from unittest.mock import patch

import engine
import init_editor
import init_options
import init_progress
import init_ui
import init_window


def resolved(*options):
    return init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli", *options]))


class ParserTests(unittest.TestCase):
    def test_steps_details_notes_and_ninja_status(self):
        progress = init_progress.SetupProgress.from_plan([init_progress.Step(str(i), f"Step {i}") for i in range(4)])
        self.assertFalse(progress.has_started_step)
        self.assertFalse(progress.feed("LUDUS_PROGRESS 1 4 Installing system prerequisites", 10.0))
        self.assertEqual(progress.steps[0].status, init_progress.RUNNING)
        self.assertEqual(progress.steps[0].label, "Installing system prerequisites")
        self.assertEqual(progress.fraction(), 0.0)
        self.assertFalse(progress.feed("LUDUS_PROGRESS_NOTE Waiting for your sudo password", 11.0))
        self.assertEqual(progress.note, "Waiting for your sudo password")
        self.assertTrue(progress.feed("+ sudo apt-get update", 12.0))
        self.assertEqual((progress.note, progress.last_line), ("", "+ sudo apt-get update"))
        self.assertFalse(progress.feed("LUDUS_PROGRESS 3 4 Conan install", 20.0))
        self.assertEqual([step.status for step in progress.steps],
                         [init_progress.DONE, init_progress.DONE, init_progress.RUNNING, init_progress.PENDING])
        self.assertEqual(progress.steps[0].finished_at, 20.0)
        self.assertFalse(progress.feed("LUDUS_PROGRESS_DETAIL 1 4 conan install linux-clang-debug", 21.0))
        self.assertEqual(progress.fraction(), (2 + 0.25) / 4)
        self.assertTrue(progress.feed("[50/200] Building CXX object modules/a.cpp.o", 22.0))
        self.assertEqual((progress.detail_done, progress.detail_total), (50, 200))
        self.assertEqual(progress.detail_text, "Building CXX object modules/a.cpp.o")
        self.assertEqual(progress.fraction(), (2 + 0.25) / 4)
        self.assertFalse(progress.feed("LUDUS_PROGRESS 4 4 Verifying tools", 23.0))
        self.assertEqual(progress.detail_total, 0)

    def test_runner_plan_replaces_a_different_prediction(self):
        progress = init_progress.SetupProgress.from_plan([init_progress.Step("a", "A")])
        progress.feed("LUDUS_PROGRESS 2 3 Second", 1.0)
        self.assertEqual(len(progress.steps), 3)
        self.assertEqual([step.status for step in progress.steps],
                         [init_progress.DONE, init_progress.RUNNING, init_progress.PENDING])
        for malformed in ("LUDUS_PROGRESS 0 3 Zero", "LUDUS_PROGRESS 4 3 Past the end", "LUDUS_PROGRESS x 3 Bad"):
            progress.feed(malformed, 2.0)
        self.assertEqual(progress.current, 1)

    def test_outcomes(self):
        steps = [init_progress.Step("a", "A"), init_progress.Step("b", "B"), init_progress.Step("c", "C")]
        success = init_progress.SetupProgress.from_plan(steps)
        success.feed("LUDUS_PROGRESS 1 3 A", 0.0)
        success.feed("", 0.0)
        success.feed("Ludus initialization complete. No engine targets were built.", 1.0)
        success.feed("Prepared presets: linux-clang-development", 1.0)
        success.finish(0, 2.0)
        self.assertEqual(success.outcome, init_progress.DONE)
        self.assertEqual(success.fraction(), 1.0)
        self.assertTrue(all(step.status == init_progress.DONE for step in success.steps))
        self.assertEqual(success.summary, ["Ludus initialization complete. No engine targets were built.",
                                           "Prepared presets: linux-clang-development"])

        failure = init_progress.SetupProgress.from_plan(steps)
        failure.feed("LUDUS_PROGRESS 2 3 B", 0.0)
        failure.feed("error: command failed with exit code 1: conan install", 1.0)
        failure.finish(1, 2.0)
        self.assertEqual(failure.outcome, init_progress.FAILED)
        self.assertEqual(failure.error, "command failed with exit code 1: conan install")
        self.assertEqual([step.status for step in failure.steps],
                         [init_progress.DONE, init_progress.FAILED, init_progress.PENDING])

        silent = init_progress.SetupProgress.from_plan(steps)
        silent.finish(3, 0.0)
        self.assertIn("exit code 3", silent.error)
        unstarted = init_progress.SetupProgress.from_plan(steps)
        unstarted.finish(None, 0.0)
        self.assertEqual(unstarted.error, "Setup could not start.")

        cancelled = init_progress.SetupProgress.from_plan(steps)
        cancelled.feed("LUDUS_PROGRESS 1 3 A", 0.0)
        cancelled.finish(-15, 1.0, cancelled=True)
        self.assertEqual(cancelled.outcome, init_progress.CANCELLED)
        self.assertEqual(cancelled.steps[0].status, init_progress.CANCELLED)

    def test_clean_line_and_elapsed(self):
        self.assertEqual(init_progress.clean_line("\x1b[32mok\x1b[0m\n"), "ok")
        self.assertEqual(init_progress.clean_line(" 10%\r 50%\r100%"), "100%")
        self.assertEqual(init_progress.format_elapsed(5), "00:05")
        self.assertEqual(init_progress.format_elapsed(3725), "1:02:05")


class ReporterTests(unittest.TestCase):
    def tearDown(self):
        init_progress.stop()

    def test_terminal_and_machine_formats(self):
        steps = [init_progress.Step("a", "Alpha"), init_progress.Step("b", "Beta")]
        for machine, expected in ((False, "==> [2/2] Beta"), (True, "LUDUS_PROGRESS 2 2 Beta")):
            with self.subTest(machine=machine), contextlib.redirect_stdout(io.StringIO()) as output:
                reporter = init_progress.start(steps, environment={"LUDUS_INIT_PROGRESS": "1"} if machine else {})
                init_progress.begin("b")
                init_progress.begin("not-in-plan")
                init_progress.detail(1, 2, "half")
                init_progress.note("notice")
            self.assertEqual(reporter.machine_readable, machine)
            self.assertIn(expected, output.getvalue())
            self.assertEqual("LUDUS_PROGRESS_DETAIL 1 2 half" in output.getvalue(), machine)
            self.assertEqual("LUDUS_PROGRESS_NOTE notice" in output.getvalue(), machine)
        init_progress.stop()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            init_progress.begin("a")
        self.assertEqual(output.getvalue(), "")

    def test_sudo_password_notice(self):
        init_progress.start([], environment={"LUDUS_INIT_PROGRESS": "1"})
        for argv, cached, expected in ((["/usr/bin/sudo", "apt-get"], False, True),
                                       (["/usr/bin/sudo", "apt-get"], True, False),
                                       (["apt-get"], False, False)):
            with self.subTest(argv=argv, cached=cached), contextlib.redirect_stdout(io.StringIO()) as output:
                init_progress.announce_privileged_command(argv, probe=lambda: cached)
            self.assertEqual("LUDUS_PROGRESS_NOTE Waiting for your sudo password" in output.getvalue(), expected)


class PlanTests(unittest.TestCase):
    def run_init(self, args):
        """Run command_init with every action stubbed below its progress announcements."""
        with contextlib.ExitStack() as stack:
            for name in ("check_current_python", "configure_system_tool_shims", "install_ubuntu_system_prerequisites",
                         "validate_required_system_tools", "create_or_update_venv", "install_managed_tools",
                         "validate_managed_tools", "export_local_recipes", "create_conan_lock",
                         "conan_install_for_preset", "repair_existing_cmake_caches", "command_setup_rad_debugger",
                         "command_install_hooks", "command_build", "command_test", "command_check",
                         "command_install_sdk", "run", "cmake_configure", "cmake_build"):
                stack.enter_context(patch.object(engine, name))
            stack.enter_context(patch.object(engine, "install_conan_profile", return_value="profile"))
            stack.enter_context(patch.object(engine, "conan_cache_lock", return_value=contextlib.nullcontext()))
            stack.enter_context(patch.object(engine, "host_supports_apt_install", return_value=True))
            stack.enter_context(patch.object(engine, "capture_command_quiet", return_value=(0, "install ok installed")))
            stack.enter_context(patch.object(engine, "load_tool_versions", return_value={"minimum": {"python": "3.10"}}))
            stack.enter_context(patch.object(engine, "command_doctor", return_value=0))
            stack.enter_context(patch.object(engine.platform, "system", return_value="Linux"))
            stack.enter_context(patch.object(init_editor.platform, "system", return_value="Linux"))
            stack.enter_context(patch.object(init_editor.platform, "machine", return_value="x86_64"))
            stack.enter_context(patch.object(init_editor.cmake_targets, "query_codemodel"))
            stack.enter_context(patch.object(init_editor, "refresh_editor_engine_installs",
                                             side_effect=lambda *_: init_progress.begin("engine_refresh")))
            stack.enter_context(patch.object(init_options, "save_options"))
            stack.enter_context(patch.dict(os.environ, {"LUDUS_INIT_PROGRESS": "1"}))
            os.environ.pop("CI", None)
            output = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            self.assertEqual(engine.command_init(args), 0)
        return [line for line in output.getvalue().splitlines() if line.startswith("LUDUS_PROGRESS ")]

    def test_runner_announces_every_planned_step_in_order(self):
        for options in ((), ("--no-system-install", "--preset-only"), ("--with-editor",),
                        ("--with-editor", "--no-editor-preview", "--no-engine-refresh", "--run-tests"),
                        ("--validate", "--with-rad-debugger", "--with-editor"),
                        ("--validate", "--skip-checks", "--skip-sanitizers", "--skip-sdk")):
            with self.subTest(options=options):
                args = resolved("linux-clang-development", *options)
                with patch.dict(os.environ, {}, clear=False):
                    os.environ.pop("CI", None)
                    expected = init_progress.native_plan(args, system="Linux")
                markers = self.run_init(args)
                total = len(expected)
                self.assertEqual(markers, [f"LUDUS_PROGRESS {index} {total} {step.label}"
                                           for index, step in enumerate(expected, start=1)])

    def test_browser_plan(self):
        args = resolved("web-emscripten-development")
        self.assertEqual([step.key for step in init_progress.plan(args)],
                         ["host_tools", "emsdk", "verify", "shader_tools", "configure"])
        self.assertNotIn("shader_tools", [step.key for step in init_progress.plan(resolved(
            "web-emscripten-development", "--no-smoke-app"))])


class SelectionTests(unittest.TestCase):
    def test_every_init_option_reaches_the_child_runner(self):
        parser = engine.make_parser()
        init = next(action for action in parser._subparsers._group_actions[0].choices.values()
                    if action.prog.endswith(" init"))
        covered = {name for name, _ in init_ui.STORE_TRUE_OPTIONS} | {item[0] for item in init_ui.PAIRED_OPTIONS}
        interface = {"help", "cli", "gui", "preset", "persona", "func"}
        for action in init._actions:
            if action.dest not in interface:
                self.assertIn(action.dest, covered, f"{action.dest} is not forwarded by selection_to_argv")
        for options in ((), ("--persona", "application", "--with-editor", "--no-editor-preview", "--locked"),
                        ("--persona", "validation", "--skip-sdk", "--with-rad-debugger", "--no-system-install"),
                        ("--persona", "browser", "--with-web-probes")):
            with self.subTest(options=options):
                selection = resolved(*options)
                argv = init_ui.selection_to_argv(selection)
                self.assertIn("--cli", argv)
                child = init_ui.apply_defaults(engine.make_parser().parse_args(["init", *argv]))
                for name in covered | {"preset", "persona"}:
                    self.assertEqual(getattr(child, name), getattr(selection, name), name)

    def test_gui_selection_opens_progress_instead_of_running_in_process(self):
        selection = resolved("--no-system-install")
        with patch.object(init_ui, "wants_gui", return_value=True), \
                patch.object(init_ui, "select_options", return_value=selection), \
                patch("init_window.run_setup", return_value=0) as window, \
                patch.object(engine, "command_init") as install:
            self.assertEqual(engine.main(["init"]), 0)
        install.assert_not_called()
        window.assert_called_once()
        self.assertTrue(window.call_args.args[0].no_system_install)


@unittest.skipIf(os.name == "nt", "POSIX process tree")
class ProcessTreeTests(unittest.TestCase):
    def test_cancel_reaches_grandchildren(self):
        process = subprocess.Popen(["sh", "-c", "sleep 60 & wait"])
        try:
            deadline = time.monotonic() + 5
            descendants = []
            while not descendants and time.monotonic() < deadline:
                descendants = init_window.descendant_process_ids(process.pid)
                time.sleep(0.05)
            self.assertTrue(descendants)
            init_window.signal_process_tree(process, force=False)
            process.wait(timeout=5)
            deadline = time.monotonic() + 5
            while any(alive(pid) for pid in descendants) and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual([pid for pid in descendants if alive(pid)], [])
        finally:
            if process.poll() is None:
                process.kill()


def alive(process_id: int) -> bool:
    """True while the process runs; an unreaped zombie has already stopped."""
    state = subprocess.run(["ps", "-o", "stat=", "-p", str(process_id)], capture_output=True, text=True,
                           check=False).stdout.strip()
    return bool(state) and not state.startswith("Z")


FAKE_RUNNER = textwrap.dedent("""
    import sys, time
    outcome = sys.argv[1]
    print("Ludus one-command onboarding", flush=True)
    print("LUDUS_PROGRESS 1 3 Installing system prerequisites", flush=True)
    print("LUDUS_PROGRESS_NOTE Waiting for your sudo password in the terminal that launched setup", flush=True)
    time.sleep(0.2)
    print("+ sudo apt-get update", flush=True)
    print("LUDUS_PROGRESS 2 3 Conan install (all native presets)", flush=True)
    print("LUDUS_PROGRESS_DETAIL 1 4 conan install linux-clang-development", flush=True)
    if outcome == "hang":
        import subprocess
        subprocess.run(["sleep", "60"])
    if outcome == "fail":
        print("error: command failed with exit code 1: conan install", flush=True)
        sys.exit(1)
    time.sleep(0.5)
    print("LUDUS_PROGRESS 3 3 Building the editor", flush=True)
    for i in range(1, 6):
        print(f"[{i}/5] Building CXX object apps/editor/src/file{i}.cpp.o", flush=True)
    print("", flush=True)
    print("Ludus initialization complete. The optional editor and its dependencies were built.", flush=True)
    print("Launch the editor: ./scripts/editor --preset linux-clang-development", flush=True)
""")


@unittest.skipUnless(os.environ.get("LUDUS_TEST_GUI") == "1", "requires a desktop display")
class WindowTests(unittest.TestCase):
    def run_window(self, outcome, action=None):
        import tkinter as tk
        from tkinter import ttk
        steps = [init_progress.Step("system", "Installing system prerequisites"),
                 init_progress.Step("conan", "Conan install (all native presets)"),
                 init_progress.Step("editor_build", "Building the editor")]
        window = init_window.SetupWindow(tk, ttk, steps=steps, command=[sys.executable, "-c", FAKE_RUNNER, outcome],
                                         title="test", log_path=None, cwd=engine.repo_root(),
                                         environment=init_window.child_environment())
        errors = []
        window.window.report_callback_exception = lambda *error: (errors.append(error), window.window.quit())
        observed = {}

        def observe_running():
            observed["initialize"] = str(window.initialize_button.cget("state"))
            observed["mode"] = str(window.bar.cget("mode"))
            if action is not None:
                action(window)
        window.window.after(300, observe_running)
        window.window.after(20000, window.window.quit)

        def close_when_done():
            if window.finished_at is not None:
                observed["status"] = window.status.get()
                observed["outcome"] = window.outcome.get()
                observed["retry"] = (str(window.initialize_button.cget("text")),
                                     str(window.initialize_button.cget("state")))
                observed["log_visible"] = window.log_frame.winfo_ismapped()
                window.window.quit()
            else:
                window.window.after(100, close_when_done)
        window.window.after(100, close_when_done)
        code = window.run()
        self.assertEqual(errors, [])
        return code, window, observed

    def test_success(self):
        code, window, observed = self.run_window("pass")
        self.assertEqual(code, 0)
        self.assertEqual(observed["initialize"], "disabled")
        self.assertEqual(window.progress.outcome, init_progress.DONE)
        self.assertIn("Ludus initialization complete", observed["outcome"])
        self.assertEqual([step.status for step in window.progress.steps], [init_progress.DONE] * 3)

    def test_failure_shows_error_and_offers_retry(self):
        code, window, observed = self.run_window("fail")
        self.assertEqual(code, 1)
        self.assertIn("Conan install", observed["status"])
        self.assertIn("command failed with exit code 1", observed["outcome"])
        self.assertEqual(observed["retry"], ("Retry", "normal"))
        self.assertTrue(observed["log_visible"])

    def test_cancel_stops_the_runner(self):
        code, window, observed = self.run_window("hang", action=lambda window: window.cancel())
        self.assertEqual(code, init_window.CANCELLED_EXIT_CODE)
        self.assertEqual(window.progress.outcome, init_progress.CANCELLED)
        self.assertEqual(observed["retry"], ("Retry", "normal"))


if __name__ == "__main__":
    unittest.main()
