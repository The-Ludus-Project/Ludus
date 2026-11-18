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
HIGH_CONTRAST_TEXT_CONTRAST = 7.0
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

    def test_high_contrast_palettes_meet_the_higher_ratio(self):
        for appearance, palette in init_theme.HIGH_CONTRAST_PALETTES.items():
            for surface in ("workspace", "panel", "raised"):
                for role in ("text", "secondary_text"):
                    with self.subTest(appearance=appearance, role=role, surface=surface):
                        self.assertGreaterEqual(init_theme.contrast_ratio(getattr(palette, role),
                                                                          getattr(palette, surface)),
                                                HIGH_CONTRAST_TEXT_CONTRAST)
            self.assertNotEqual(palette, init_theme.PALETTES[appearance])

    def test_contrast_ratio_extremes(self):
        self.assertAlmostEqual(init_theme.contrast_ratio("#000000", "#ffffff"), 21.0, places=3)
        self.assertAlmostEqual(init_theme.contrast_ratio("#57d2c1", "#57d2c1"), 1.0)


class HighContrastTests(unittest.TestCase):
    def test_override_wins(self):
        for value, expected in (("1", True), ("true", True), ("ON", True), ("0", False), ("off", False)):
            run = Mock()
            self.assertIs(init_theme.detect_high_contrast(environment={"LUDUS_HIGH_CONTRAST": value}, system="Linux",
                                                          run=run), expected)
            run.assert_not_called()

    def test_linux_reads_gnome(self):
        on = Mock(return_value=SimpleNamespace(returncode=0, stdout="true\n"))
        off = Mock(return_value=SimpleNamespace(returncode=0, stdout="false\n"))
        self.assertTrue(init_theme.detect_high_contrast(environment={}, system="Linux", run=on))
        self.assertFalse(init_theme.detect_high_contrast(environment={}, system="Linux", run=off))
        missing = Mock(side_effect=FileNotFoundError("gsettings"))
        self.assertFalse(init_theme.detect_high_contrast(environment={}, system="Linux", run=missing))

    def test_macos_reads_increase_contrast(self):
        on = Mock(return_value=SimpleNamespace(returncode=0, stdout="1\n"))
        unset = Mock(return_value=SimpleNamespace(returncode=1, stdout=""))
        self.assertTrue(init_theme.detect_high_contrast(environment={}, system="Darwin", run=on))
        self.assertEqual(on.call_args.args[0][:3], ["defaults", "read", "com.apple.universalaccess"])
        self.assertFalse(init_theme.detect_high_contrast(environment={}, system="Darwin", run=unset))


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


class CheckboxTests(unittest.TestCase):
    """The setup check box is the Editor's: a rounded token-size box, filled with the accent and ticked when on."""

    def render(self, *, mark):
        palette = init_theme.LIGHT_PALETTE
        size, gap = init_theme.CHECKBOX_INDICATOR_POINTS, init_theme.SPACE_SMALL
        rows = init_theme.render_checkbox(size, gap, background=palette.workspace, fill=palette.primary if mark
                                          else palette.panel, edge=palette.primary if mark else palette.border,
                                          mark=mark, edge_width=1, radius=init_theme.CONTROL_RADIUS_POINTS,
                                          stroke=init_theme.CHECKBOX_TICK_STROKE_POINTS)
        return palette, size, gap, rows

    def test_box_has_token_size_rounded_corners_and_a_gap_before_the_label(self):
        palette, size, gap, rows = self.render(mark=None)
        self.assertEqual(len(rows), size)
        self.assertTrue(all(len(row) == size + gap for row in rows))
        # Rounded: the corner pixel is the surface, the middle of each edge is the control border.
        self.assertEqual(rows[0][0], palette.workspace.lower())
        self.assertEqual(rows[0][size // 2], palette.border.lower())
        self.assertEqual(rows[size // 2][0], palette.border.lower())
        self.assertEqual(rows[size // 2][size // 2], palette.panel.lower())
        self.assertEqual({pixel for row in rows for pixel in row[size:]}, {palette.workspace.lower()})

    def test_checked_box_is_filled_and_carries_a_tick_not_a_cross(self):
        palette, size, _gap, rows = self.render(mark=init_theme.LIGHT_PALETTE.on_primary)
        mark = palette.on_primary.lower()
        corner_x, corner_y = (round(value * size - 0.5) for value in init_theme.CHECKBOX_TICK_POINTS[1])
        # The tick is thinner than a pixel is wide at 1x, so its corner pixel is mostly, not wholly, the mark.
        def distance(first: str, second: str) -> int:
            return sum(abs(int(first[index:index + 2], 16) - int(second[index:index + 2], 16)) for index in (1, 3, 5))
        self.assertLess(distance(rows[corner_y][corner_x], mark), distance(rows[corner_y][corner_x],
                                                                           palette.primary.lower()))
        # A cross would mark the upper-left and lower-right quarters; the tick leaves them filled.
        quarter = size // 4
        self.assertEqual(rows[quarter + 1][quarter + 1], palette.primary.lower())
        self.assertEqual(rows[size - quarter - 2][size - quarter - 2], palette.primary.lower())


# A MacBook Air (13-inch, notch): 1470 x 956 points, a 37-point menu bar and a 70-point Dock at the bottom.
MACBOOK_AIR_FRAME = ((0.0, 0.0), (1470.0, 956.0))
MACBOOK_AIR_VISIBLE = ((0.0, 70.0), (1470.0, 849.0))
# A 1080p display beside it, which [NSScreen mainScreen] answers while the launching Terminal is the key window.
EXTERNAL_FRAME = ((1470.0, 0.0), (1920.0, 1080.0))
EXTERNAL_VISIBLE = ((1470.0, 0.0), (1920.0, 1055.0))


class VisibleFrameTests(unittest.TestCase):
    def test_menu_bar_screen_converts_to_tk_coordinates(self):
        self.assertEqual(init_theme.visible_area_from_frames(MACBOOK_AIR_FRAME, MACBOOK_AIR_VISIBLE, 956.0),
                         (37.0, 849.0))

    def test_another_displays_frame_is_rejected(self):
        # Trusting it gave a 1007-point usable height on a 956-point screen: the window was centred for a taller
        # screen and its footer ended below the Dock (round 11 MacBook Air capture).
        self.assertIsNone(init_theme.visible_area_from_frames(EXTERNAL_FRAME, EXTERNAL_VISIBLE, 956.0))
        # A visible frame taller than its own screen is a misread structure, not a screen.
        self.assertIsNone(init_theme.visible_area_from_frames(MACBOOK_AIR_FRAME, ((0.0, 0.0), (1470.0, 2000.0)),
                                                              956.0))

    def test_rejected_frame_falls_back_and_says_why(self):
        root = SimpleNamespace(winfo_screenheight=lambda: 956, winfo_screenwidth=lambda: 1470,
                               wm_maxsize=lambda: (1470, 956))
        with patch.object(init_theme, "read_appkit_frames",
                          return_value=(EXTERNAL_FRAME, EXTERNAL_VISIBLE, "NSScreen screens[0]")):
            top, usable = init_theme.usable_screen_area(root, system="Darwin")
            self.assertIn("rejected", init_theme._last_frame_note[0])
        self.assertEqual(top, init_theme.MACOS_MENU_BAR_ALLOWANCE_POINTS)
        self.assertLessEqual(top + usable + init_theme.WINDOW_FRAME_ALLOWANCE_POINTS, 956)
        with patch.object(init_theme, "read_appkit_frames", return_value=(None, None, "AppKit unreadable (OSError)")):
            init_theme.usable_screen_area(root, system="Darwin")
            self.assertEqual(init_theme._last_frame_note[0], "AppKit unreadable (OSError)")
        with patch.object(init_theme, "read_appkit_frames",
                          return_value=(MACBOOK_AIR_FRAME, MACBOOK_AIR_VISIBLE, "NSScreen screens[0]")):
            self.assertEqual(init_theme.usable_screen_area(root, system="Darwin"),
                             (37, 849 - init_theme.WINDOW_FRAME_ALLOWANCE_POINTS))


@unittest.skipUnless(os.environ.get("LUDUS_TEST_GUI") == "1", "requires a desktop display")
class ThemeWindowTests(unittest.TestCase):
    def test_check_buttons_draw_the_token_indicator_in_every_state(self):
        import tkinter as tk
        from tkinter import ttk

        root = init_theme.create_root(tk)
        try:
            for appearance in (init_theme.LIGHT, init_theme.DARK):
                with self.subTest(appearance=appearance):
                    theme = init_theme.Theme(root, ttk, appearance=appearance)
                    for surface in (init_theme.WORKSPACE_SURFACE, init_theme.CARD_SURFACE):
                        layout = str(theme.style.layout(f"{surface}.TCheckbutton"))
                        self.assertIn(f"{surface}.Ludus", layout)
                        self.assertNotIn("Checkbutton.indicator", layout)
                    states = next(iter(root._ludus_checkbox_images.values()))
                    self.assertEqual(set(states), {"unchecked", "hovered", "checked", "disabled", "disabled_checked"})
                    for image in states.values():
                        self.assertEqual(int(root.tk.call("image", "height", image)),
                                         theme.space(init_theme.CHECKBOX_INDICATOR_POINTS))
                    box = ttk.Checkbutton(root, text="Option", style="Card.TCheckbutton")
                    box.grid()
                    box.invoke()
                    root.update_idletasks()
                    self.assertTrue(box.instate(["selected"]))
                    box.destroy()
            # A second theme with the same look reuses the elements instead of failing to recreate them.
            init_theme.Theme(root, ttk, appearance=init_theme.LIGHT)
        finally:
            root.destroy()

    def test_scroll_area_scrolls_only_when_its_content_is_taller(self):
        import tkinter as tk
        from tkinter import ttk

        root = init_theme.create_root(tk)
        try:
            theme = init_theme.Theme(root, ttk, appearance=init_theme.LIGHT)
            root.columnconfigure(0, weight=1)
            root.rowconfigure(0, weight=1)
            area = init_theme.ScrollArea(tk, ttk, root, theme)
            area.grid(row=0, column=0, sticky="nsew")
            rows = [ttk.Label(area.content, text=f"Row {index}") for index in range(40)]
            for index, label in enumerate(rows):
                label.grid(row=index, column=0, sticky="w")
            root.update_idletasks()
            natural = root.winfo_reqheight()
            root.geometry(f"{root.winfo_reqwidth()}x{natural}")
            root.update()
            self.assertFalse(area.is_scrollable)
            self.assertFalse(area.scrollbar.winfo_ismapped())

            root.geometry(f"{root.winfo_reqwidth()}x{natural // 3}")
            root.update()
            self.assertTrue(area.is_scrollable)
            self.assertTrue(area.scrollbar.winfo_ismapped())
            # The scroll bar floats over the margin, so the content keeps its width.
            self.assertEqual(area.content.winfo_width(), area.canvas.winfo_width())
            area.show(rows[-1])
            root.update()
            bottom = rows[-1].winfo_rooty() + rows[-1].winfo_height()
            self.assertLessEqual(bottom, area.canvas.winfo_rooty() + area.canvas.winfo_height())
            self.assertGreater(area.canvas.canvasy(0), 0)
        finally:
            root.destroy()

    def test_macos_windows_fit_between_the_menu_bar_and_the_dock(self):
        import tkinter as tk

        root = init_theme.create_root(tk)
        try:
            menu_bar, visible = 37, 380
            tall = tk.Frame(root, width=200, height=root.winfo_screenheight() * 2)
            tall.grid()
            with patch.object(init_theme, "macos_visible_frame", return_value=(float(menu_bar), float(visible))), \
                    patch.object(init_theme.platform, "system", return_value="Darwin"):
                top, usable = init_theme.usable_screen_area(root)
                init_theme.fit_to_screen(root)
                init_theme.centre_window(root)
            root.update()
            self.assertEqual(top, menu_bar)
            self.assertEqual(usable, visible - init_theme.WINDOW_FRAME_ALLOWANCE_POINTS)
            self.assertLessEqual(root.winfo_height(), usable)
            # Centred in the free area: below the menu bar, and its bottom (title bar included) above the Dock.
            self.assertGreaterEqual(root.winfo_y(), menu_bar)
            self.assertLessEqual(root.winfo_y() + root.winfo_height() + init_theme.WINDOW_FRAME_ALLOWANCE_POINTS,
                                 menu_bar + visible)
        finally:
            root.destroy()

    def test_unreadable_macos_frame_falls_back_to_the_screen(self):
        import tkinter as tk

        root = init_theme.create_root(tk)
        try:
            with patch.object(init_theme, "macos_visible_frame", return_value=None):
                top, usable = init_theme.usable_screen_area(root, system="Darwin")
            # Below the tallest menu bar, and with its title bar room still above the bottom of the screen.
            self.assertEqual(top, init_theme.MACOS_MENU_BAR_ALLOWANCE_POINTS)
            self.assertLessEqual(top + usable + init_theme.WINDOW_FRAME_ALLOWANCE_POINTS, root.winfo_screenheight())
        finally:
            root.destroy()

    def test_content_that_grows_after_the_fit_scrolls_instead_of_pushing_the_footer_off(self):
        import tkinter as tk
        from tkinter import ttk

        root = init_theme.create_root(tk)
        try:
            theme = init_theme.Theme(root, ttk)
            root.columnconfigure(0, weight=1)
            root.rowconfigure(0, weight=1)
            area = init_theme.ScrollArea(tk, ttk, root, theme)
            area.grid(row=0, column=0, sticky="nsew")
            tk.Frame(area.content, width=300, height=200).grid(row=0, column=0)
            footer = ttk.Button(root, text="Initialize")
            footer.grid(row=1, column=0)
            with patch.object(init_theme, "usable_screen_area", return_value=(37, 400)):
                init_theme.fit_to_screen(root)
                init_theme.centre_window(root)
                root.update()
                self.assertEqual(int(root.wm_maxsize()[1]), 400)
                # Content asks for far more once the window shows, as fonts resolved at first draw do on macOS.
                tk.Frame(area.content, width=300, height=root.winfo_screenheight()).grid(row=1, column=0)
                root.update()
                area.refresh()
                root.update()
                self.assertLessEqual(root.winfo_height(), 400)
                self.assertTrue(area.is_scrollable)
                self.assertLessEqual(footer.winfo_rooty() + footer.winfo_height(), root.winfo_rooty() + 400)
        finally:
            root.destroy()

    def test_a_window_drawn_too_low_is_pulled_back_and_logged(self):
        import tempfile
        import tkinter as tk

        root = init_theme.create_root(tk)
        directory = tempfile.TemporaryDirectory()
        try:
            tk.Frame(root, width=300, height=300).grid()
            log_path = Path(directory.name) / "out/init/setup.log"
            with patch.object(init_theme, "usable_screen_area", return_value=(37, 500)):
                root.geometry("300x300+40+400")
                root.update()
                init_theme.check_window_fit_later(root, "options", log_path=log_path)
                deadline = 50
                while not log_path.exists() and deadline:
                    root.after(50)
                    root.update()
                    deadline -= 1
                root.update()
                self.assertLessEqual(root.winfo_y() + root.winfo_height(), 37 + 500)
            log = log_path.read_text(encoding="utf-8")
            self.assertIn("Ludus Setup window (options, moved back on screen)", log)
            for fact in ("screen ", "visible frame", "usable area top 37 height 500", "content ", "window 300x"):
                self.assertIn(fact, log)
        finally:
            root.destroy()
            directory.cleanup()

    def test_windows_fit_the_usable_screen_height(self):
        import tkinter as tk

        root = init_theme.create_root(tk)
        try:
            tall = tk.Frame(root, width=200, height=root.winfo_screenheight() * 2)
            tall.grid()
            init_theme.fit_to_screen(root)
            init_theme.centre_window(root)
            root.update()
            usable = init_theme.usable_screen_height(root)
            self.assertLess(usable, root.winfo_screenheight())
            self.assertLessEqual(root.winfo_height(), usable)
            self.assertGreaterEqual(root.winfo_rooty(), 0)
            self.assertLessEqual(root.winfo_rooty() + root.winfo_height(), root.winfo_screenheight())
        finally:
            root.destroy()

    def test_completed_status_disk_is_inset_from_canvas_edges(self):
        import tkinter as tk
        from tkinter import ttk

        root = init_theme.create_root(tk)
        try:
            theme = init_theme.Theme(root, ttk)
            for scale in (1.0, 2.0):
                with self.subTest(scale=scale), patch.object(theme, "scale", scale):
                    icon = theme.status_icon(tk, root)
                    icon.show(init_progress.DONE)
                    disk, mark = icon.canvas.find_all()
                    left, top, right, bottom = icon.canvas.coords(disk)
                    self.assertEqual(left, top)
                    self.assertEqual(right, bottom)
                    self.assertGreaterEqual(left, 1)
                    self.assertEqual(icon.size - right, left)
                    self.assertEqual(icon.canvas.itemcget(disk, "outline"), "")
                    self.assertEqual(icon.canvas.type(mark), "line")
        finally:
            root.destroy()

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
