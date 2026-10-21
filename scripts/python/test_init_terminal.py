"""Terminal formatting of ./init.sh: colour policy and stable plain text."""
from __future__ import annotations

import contextlib
import io
import unittest

import init_progress
import init_terminal


class FakeTerminal(io.StringIO):
    def __init__(self, *, is_terminal: bool, encoding: str = "utf-8"):
        super().__init__()
        self.is_terminal = is_terminal
        self._encoding = encoding

    def isatty(self) -> bool:
        return self.is_terminal

    @property
    def encoding(self) -> str:
        return self._encoding


class ColourPolicyTests(unittest.TestCase):
    def test_colour_needs_a_terminal_without_no_color(self):
        terminal = FakeTerminal(is_terminal=True)
        self.assertTrue(init_terminal.supports_colour(terminal, {"TERM": "xterm-256color"}))
        self.assertFalse(init_terminal.supports_colour(FakeTerminal(is_terminal=False), {}))
        self.assertFalse(init_terminal.supports_colour(terminal, {"NO_COLOR": "1"}))
        self.assertFalse(init_terminal.supports_colour(terminal, {"TERM": "dumb"}))
        # https://no-color.org: an empty NO_COLOR does not disable colour.
        self.assertTrue(init_terminal.supports_colour(terminal, {"NO_COLOR": ""}))

    def test_disabled_painter_returns_plain_text(self):
        plain = init_terminal.Painter(False)
        self.assertEqual(init_terminal.step_line(3, 9, "Conan install", plain), "==> [3/9] Conan install")
        self.assertEqual(init_terminal.error_line("boom", plain), "error: boom")
        self.assertEqual(init_terminal.command_echo("cmake --version", plain), "+ cmake --version")
        self.assertNotIn("\x1b", "".join(init_terminal.banner_lines([("Preset", "x")], plain)))

    def test_coloured_text_cleans_to_the_plain_text(self):
        coloured = init_terminal.Painter(True)
        line = init_terminal.step_line(3, 9, "Conan install", coloured)
        self.assertIn("\x1b[", line)
        self.assertEqual(init_progress.clean_line(line), "==> [3/9] Conan install")
        error = init_terminal.error_line("boom", coloured)
        self.assertEqual(init_progress.clean_line(error), "error: boom")
        completion = init_terminal.success("Ludus initialization complete.", coloured)
        progress = init_progress.SetupProgress.from_plan([])
        progress.feed(completion, 0.0)
        self.assertEqual(progress.summary, ["Ludus initialization complete."])

    def test_banner_aligns_labels_and_falls_back_to_ascii(self):
        plain = init_terminal.Painter(False)
        lines = init_terminal.banner_lines([("Repository", "/r"), ("Preset", "p")], plain,
                                           FakeTerminal(is_terminal=False, encoding="ascii"))
        self.assertEqual(lines[0], "Ludus Setup")
        self.assertEqual(set(lines[1]), {"-"})
        self.assertEqual(lines[2:], ["  Repository  /r", "  Preset      p"])
        unicode_rule = init_terminal.banner_lines([], plain, FakeTerminal(is_terminal=False))[1]
        self.assertEqual(set(unicode_rule), {init_terminal.RULE_CHARACTER})


class ReporterFailureTests(unittest.TestCase):
    def tearDown(self):
        init_progress.stop()

    def test_terminal_failure_names_the_step(self):
        steps = [init_progress.Step("a", "Alpha"), init_progress.Step("b", "Beta")]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            init_progress.start(steps, environment={})
            init_progress.begin("b")
            init_progress.fail()
        self.assertIn("Setup stopped at step 2/2: Beta (after 00:00).", output.getvalue())
        self.assertNotIn("\x1b", output.getvalue())

    def test_window_runs_do_not_print_the_terminal_failure(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            init_progress.start([init_progress.Step("a", "Alpha")], environment={"LUDUS_INIT_PROGRESS": "1"})
            init_progress.begin("a")
            init_progress.fail()
        self.assertNotIn("Setup stopped", output.getvalue())


if __name__ == "__main__":
    unittest.main()
