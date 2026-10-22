"""Identity, palette, icons and drawing of the ./init.sh setup windows."""
from __future__ import annotations

import os
import platform
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import init_progress
import init_theme

MINIMUM_TEXT_CONTRAST = 4.5
MINIMUM_CONTROL_CONTRAST = 3.0
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    assert data[:8] == PNG_SIGNATURE, path
    return struct.unpack(">II", data[16:24])


class PaletteTests(unittest.TestCase):
    def test_text_and_states_meet_contrast_on_every_surface(self):
        for appearance, palette in init_theme.PALETTES.items():
            pairs = []
            for surface in ("workspace", "panel", "raised"):
                pairs += [("text", surface), ("secondary_text", surface)]
            for surface in ("workspace", "panel"):
                pairs += [(role, surface) for role in ("success", "warning", "error", "focus")]
            pairs += [("text", surface) for surface in ("success_surface", "warning_surface", "error_surface",
                                                        "selected")]
            pairs += [("on_primary", "primary"), ("on_primary", "primary_active"), ("log_text", "log_background"),
                      ("selected_text", "selected")]
            for foreground, background in pairs:
                with self.subTest(appearance=appearance, foreground=foreground, background=background):
                    self.assertGreaterEqual(init_theme.contrast_ratio(getattr(palette, foreground),
                                                                      getattr(palette, background)),
                                            MINIMUM_TEXT_CONTRAST)
            for surface in ("workspace", "panel"):
                with self.subTest(appearance=appearance, control_border=surface):
                    self.assertGreaterEqual(init_theme.contrast_ratio(palette.border, getattr(palette, surface)),
                                            MINIMUM_CONTROL_CONTRAST)

    def test_contrast_ratio_extremes(self):
        self.assertAlmostEqual(init_theme.contrast_ratio("#000000", "#ffffff"), 21.0, places=3)
        self.assertAlmostEqual(init_theme.contrast_ratio("#57d2c1", "#57d2c1"), 1.0)


class AppearanceTests(unittest.TestCase):
    def test_override_wins(self):
        for value, expected in (("dark", "dark"), ("LIGHT", "light")):
            run = Mock()
            self.assertEqual(init_theme.detect_appearance(environment={"LUDUS_SETUP_APPEARANCE": value},
                                                          system="Linux", run=run), expected)
            run.assert_not_called()

    def test_linux_follows_gnome_and_gtk(self):
        dark = Mock(return_value=SimpleNamespace(returncode=0, stdout="'prefer-dark'\n"))
        light = Mock(return_value=SimpleNamespace(returncode=0, stdout="'default'\n"))
        self.assertEqual(init_theme.detect_appearance(environment={}, system="Linux", run=dark), "dark")
        self.assertEqual(init_theme.detect_appearance(environment={}, system="Linux", run=light), "light")
        self.assertEqual(init_theme.detect_appearance(environment={"GTK_THEME": "Adwaita:dark"}, system="Linux",
                                                      run=light), "dark")
        missing = Mock(side_effect=FileNotFoundError("gsettings"))
        self.assertEqual(init_theme.detect_appearance(environment={}, system="Linux", run=missing), "light")

    def test_macos_asks_tk_and_tolerates_old_tk(self):
        root = Mock()
        root.tk.call.return_value = 1
        self.assertEqual(init_theme.detect_appearance(root, environment={}, system="Darwin"), "dark")
        root.tk.call.side_effect = RuntimeError("invalid command name")
        self.assertEqual(init_theme.detect_appearance(root, environment={}, system="Darwin"), "light")


class IdentityTests(unittest.TestCase):
    def test_root_uses_the_setup_class_and_title(self):
        tk = SimpleNamespace(Tk=Mock())
        root = init_theme.create_root(tk, system="Unknown")
        tk.Tk.assert_called_once_with(className=init_theme.WINDOW_CLASS)
        root.title.assert_called_once_with(init_theme.APPLICATION_NAME)

    def test_later_windows_reuse_the_first_root(self):
        tk = SimpleNamespace(Tk=Mock())
        first = init_theme.create_root(tk, system="Unknown")
        second = init_theme.create_root(tk, system="Unknown")
        self.assertIs(first, second)
        tk.Tk.assert_called_once_with(className=init_theme.WINDOW_CLASS)
        second.deiconify.assert_called_once_with()

    def test_a_destroyed_root_is_replaced_and_still_retained(self):
        tk = SimpleNamespace(Tk=Mock(side_effect=[Mock(), Mock()]))
        first = init_theme.create_root(tk, system="Unknown")
        first.winfo_exists.side_effect = RuntimeError("main window destroyed")
        second = init_theme.create_root(tk, system="Unknown")
        self.assertIsNot(first, second)
        retained = [root for module, root in init_theme._shared_roots if module is tk]
        self.assertEqual(retained, [first, second])

    def test_macos_root_defines_the_dock_reopen_handler(self):
        tk = SimpleNamespace(Tk=Mock())
        with patch.object(init_theme, "set_process_name", return_value=False):
            root = init_theme.create_root(tk, system="Darwin")
        root.tk.eval.assert_called_once_with(init_theme.MACOS_REOPEN_HANDLER)
        # Tk 8.6 and 9 call this exact command for the kAEReopenApplication event.
        self.assertIn("proc ::tk::mac::ReopenApplication {}", init_theme.MACOS_REOPEN_HANDLER)

    def test_reopen_handler_failure_does_not_stop_setup(self):
        root = Mock()
        root.tk.eval.side_effect = RuntimeError("no Tcl")
        self.assertFalse(init_theme.install_reopen_handler(root))

    def test_release_never_destroys_the_root(self):
        root = Mock()
        root.winfo_children.return_value = []
        root.bind.return_value = ()
        root.tk.splitlist.return_value = ()
        root.grid_size.return_value = (0, 0)
        init_theme.release_root(root)
        root.withdraw.assert_called_once_with()
        root.destroy.assert_not_called()

    def test_setup_windows_do_not_destroy_the_shared_root(self):
        # Destroying the first root deletes the interpreter macOS keeps for
        # Apple events; a Dock click then crashes with a bus error.
        directory = Path(init_theme.__file__).parent
        for name in ("init_ui.py", "init_window.py"):
            with self.subTest(name=name):
                source = (directory / name).read_text(encoding="utf-8")
                self.assertNotIn("window.destroy", source)
                self.assertIn("init_theme.release_root(", source)

    def test_unsupported_hosts_keep_their_process_name(self):
        self.assertFalse(init_theme.set_process_name("Unknown"))

    @unittest.skipUnless(platform.system() == "Linux", "prctl is Linux-only")
    def test_linux_process_name(self):
        # Run in a child so the test runner keeps its own name.
        script = ("import sys; sys.path.insert(0, sys.argv[1]); import init_theme; "
                  "assert init_theme.set_process_name('Linux'); print(open('/proc/self/comm').read().strip())")
        result = subprocess.run([sys.executable, "-c", script, str(Path(init_theme.__file__).parent)],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), init_theme.PROCESS_NAME.decode())


class AssetTests(unittest.TestCase):
    def test_every_icon_size_is_bundled_at_its_size(self):
        for size in init_theme.ICON_SIZES:
            with self.subTest(size=size):
                self.assertEqual(png_size(init_theme.ASSET_DIRECTORY / f"ludus-icon-{size}.png"), (size, size))
        self.assertTrue(set(init_theme.WINDOW_ICON_SIZES) <= set(init_theme.ICON_SIZES))


@unittest.skipUnless(os.environ.get("LUDUS_TEST_GUI") == "1", "requires a desktop display")
class ThemeWindowTests(unittest.TestCase):
    def test_both_appearances_style_icons_and_draw_every_state(self):
        import tkinter as tk
        from tkinter import ttk
        for appearance in (init_theme.LIGHT, init_theme.DARK):
            with self.subTest(appearance=appearance):
                root = init_theme.create_root(tk)
                try:
                    theme = init_theme.Theme(root, ttk, appearance=appearance)
                    self.assertEqual(root.title(), init_theme.APPLICATION_NAME)
                    self.assertEqual(root.winfo_class(), "Ludus-setup")
                    self.assertTrue(theme.apply_window_icon(root))
                    self.assertIsNotNone(theme.header_image())
                    self.assertGreaterEqual(theme.space(init_theme.SPACE_LARGE), init_theme.SPACE_LARGE)
                    self.assertEqual(theme.style.lookup("Primary.TButton", "background"), theme.palette.primary)
                    icon = theme.status_icon(tk, root)
                    for status in (init_progress.PENDING, init_progress.RUNNING, init_progress.DONE,
                                   init_progress.FAILED, init_progress.CANCELLED, init_theme.WARNING):
                        icon.show(status)
                        icon.advance()
                        self.assertTrue(icon.canvas.find_all())
                    banner = theme.banner(tk, root, init_theme.ERROR)
                    banner.set_kind(init_theme.WARNING)
                    banner.set_wrap(theme.space(init_theme.SPACE_LARGE))
                    root.update_idletasks()
                finally:
                    root.destroy()

    def test_shared_root_survives_release_and_handles_reopen(self):
        import tkinter as tk
        from tkinter import ttk
        with patch.object(init_theme, "set_process_name", return_value=False):
            root = init_theme.create_root(tk, system="Darwin")
        interpreter = root.tk
        ttk.Label(root, text="selector").grid(row=0, column=0)
        root.columnconfigure(0, weight=1)
        root.bind("<Escape>", lambda _event: None)
        root.protocol("WM_DELETE_WINDOW", root.quit)
        root.after(60_000, lambda: None)
        root.update()
        root.iconify()
        root.update()
        root.tk.call("::tk::mac::ReopenApplication")
        root.update()
        self.assertEqual(root.state(), "normal")

        init_theme.release_root(root)
        self.assertEqual(root.state(), "withdrawn")
        self.assertEqual(root.winfo_children(), [])
        self.assertEqual(root.bind(), ())
        self.assertEqual(root.tk.splitlist(root.tk.call("after", "info")), ())
        self.assertEqual(int(root.grid_columnconfigure(0)["weight"]), 0)
        # A Dock click between windows must not show an empty root.
        root.tk.call("::tk::mac::ReopenApplication")
        self.assertEqual(root.state(), "withdrawn")

        with patch.object(init_theme, "set_process_name", return_value=False):
            again = init_theme.create_root(tk, system="Darwin")
        try:
            self.assertIs(again, root)
            self.assertIs(again.tk, interpreter)
            self.assertEqual(again.state(), "normal")
        finally:
            init_theme.release_root(again)

    def test_missing_icons_do_not_stop_setup(self):
        import tkinter as tk
        from tkinter import ttk
        root = init_theme.create_root(tk)
        try:
            theme = init_theme.Theme(root, ttk, appearance=init_theme.LIGHT)
            original = init_theme.ASSET_DIRECTORY
            init_theme.ASSET_DIRECTORY = Path("/nonexistent")
            try:
                theme._images.clear()
                self.assertFalse(theme.apply_window_icon(root))
                self.assertIsNone(theme.header_image())
            finally:
                init_theme.ASSET_DIRECTORY = original
        finally:
            root.destroy()


if __name__ == "__main__":
    unittest.main()
