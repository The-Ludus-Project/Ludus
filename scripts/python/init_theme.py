"""Shared identity and look of the ./init.sh setup windows.

The option selector (``init_ui``) and the progress window (``init_window``)
both create their Tk root with ``create_root`` and style it with ``Theme``.

Identity
    Every window is titled "Ludus Setup". On X11 the root's WM_CLASS is
    ``ludus-setup`` / ``Ludus-setup`` (Tk derives both from ``className``), so
    taskbars group and label it as Ludus Setup rather than "Tk". On Linux the
    process name that ``top``, ``ps -o comm`` and system monitors show becomes
    ``ludus-setup`` through ``prctl``. On macOS the application menu and Dock
    use the main bundle's ``CFBundleName``, which is set before Tk creates the
    menu bar; ``iconphoto`` replaces the Python rocket in the Dock. The full
    command line in ``ps`` still names the interpreter: rewriting ``argv``
    needs the third-party ``setproctitle`` package, which setup does not
    install.

Appearance
    Colours are the semantic tokens of the Ludus Block identity in
    docs/architecture/editor-art-direction.md, in its dark and light
    appearances, plus success, warning and error roles that keep at least
    4.5:1 contrast on every surface they are drawn on (see the tests). The
    appearance follows the desktop (macOS dark mode, GNOME ``color-scheme``,
    ``GTK_THEME`` ending in ``:dark``, the Windows app theme) unless
    ``LUDUS_SETUP_APPEARANCE`` is ``dark`` or ``light``. Spacing uses the
    4/8/12/16/24 logical pixel scale, multiplied by the display scale on
    X11/Windows (macOS already measures in points); ``LUDUS_SETUP_SCALE``
    overrides it.

Icons
    ``init_assets/ludus-icon-<size>.png`` are Lanczos downscales of the
    approved docs/wiki/assets/ludus-icon.png. Regenerate them with ImageMagick::

        for size in 16 24 32 48 64 96 128 256; do
            convert docs/wiki/assets/ludus-icon.png -filter Lanczos -resize ${size}x${size} \\
                -strip PNG32:scripts/python/init_assets/ludus-icon-${size}.png
        done

    Step states and banners use small vector symbols drawn on a canvas, so
    they look the same on every platform and stay sharp at any scale.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
import subprocess
from dataclasses import dataclass
from pathlib import Path

import init_progress


APPLICATION_NAME = "Ludus Setup"
WINDOW_CLASS = "ludus-setup"
PROCESS_NAME = b"ludus-setup"
ASSET_DIRECTORY = Path(__file__).resolve().parent / "init_assets"
ICON_SIZES = (16, 24, 32, 48, 64, 96, 128, 256)
WINDOW_ICON_SIZES = (16, 32, 48, 64, 128, 256)
HEADER_ICON_POINTS = 48
STATUS_ICON_POINTS = 16
BANNER_ICON_POINTS = 18

APPEARANCE_ENVIRONMENT = "LUDUS_SETUP_APPEARANCE"
SCALE_ENVIRONMENT = "LUDUS_SETUP_SCALE"
DARK = "dark"
LIGHT = "light"
APPEARANCE_PROBE_SECONDS = 2

# Linux prctl option that renames the calling thread (and, for the main
# thread, the process "comm" field). Names longer than 15 bytes are cut.
LINUX_SET_PROCESS_NAME = 15
REFERENCE_DOTS_PER_INCH = 96.0
POINTS_PER_INCH = 72.0

# Spacing scale in logical pixels.
SPACE_EXTRA_SMALL = 4
SPACE_SMALL = 8
SPACE_MEDIUM = 12
SPACE_LARGE = 16
SPACE_EXTRA_LARGE = 24

# Type ramp relative to the platform's default user-interface font size.
TITLE_SIZE_STEP = 8
HEADING_SIZE_STEP = 2
SMALL_SIZE_STEP = -1
MINIMUM_FONT_POINTS = 8
FALLBACK_FONT_POINTS = 10
PREFERRED_MONOSPACE = {
    "Darwin": ("SF Mono", "Menlo", "Monaco"),
    "Windows": ("Cascadia Mono", "Consolas"),
}

PROGRESS_THICKNESS_POINTS = 8
CHECKBOX_INDICATOR_POINTS = 14
FOCUS_THICKNESS_POINTS = 1
SYMBOL_STROKE_DIVISOR = 8.0
PENDING_STROKE_DIVISOR = 12.0
MINIMUM_STROKE_PIXELS = 1.5
SPINNER_EXTENT_DEGREES = 100
SPINNER_STEP_DEGREES = 30
FULL_TURN_DEGREES = 360

# Symbol geometry as fractions of the icon size.
CHECK_POINTS = ((0.28, 0.53), (0.44, 0.68), (0.72, 0.36))
CROSS_INSET = 0.33
DASH_INSET = 0.3
EXCLAMATION_TOP = 0.27
EXCLAMATION_BOTTOM = 0.57
EXCLAMATION_DOT = 0.73
DOT_RADIUS_FRACTION = 0.07

WARNING = "warning"
ERROR = "error"
SUCCESS = "success"


@dataclass(frozen=True)
class Palette:
    workspace: str
    panel: str
    raised: str
    text: str
    secondary_text: str
    disabled_text: str
    border: str
    card_border: str
    primary: str
    primary_active: str
    on_primary: str
    focus: str
    selected: str
    selected_text: str
    success: str
    warning: str
    error: str
    success_surface: str
    warning_surface: str
    error_surface: str
    log_background: str
    log_text: str


LIGHT_PALETTE = Palette(
    workspace="#edf3f2", panel="#ffffff", raised="#e1ece9",
    text="#18232c", secondary_text="#4c6267", disabled_text="#7f9195", border="#71878b", card_border="#c3d2cf",
    primary="#14786f", primary_active="#0f5f58", on_primary="#ffffff", focus="#14786f",
    selected="#d5eee8", selected_text="#18232c",
    success="#1a7535", warning="#865300", error="#b42318",
    success_surface="#e2f3e7", warning_surface="#fbf0d4", error_surface="#fce9e7",
    log_background="#18232c", log_text="#e9f2f1",
)
DARK_PALETTE = Palette(
    workspace="#18232c", panel="#222f39", raised="#2c3c47",
    text="#e9f2f1", secondary_text="#b5c5c9", disabled_text="#71858c", border="#80969e", card_border="#3a4d59",
    primary="#57d2c1", primary_active="#7fe0d3", on_primary="#18232c", focus="#57d2c1",
    selected="#294b4b", selected_text="#e9f2f1",
    success="#74d68f", warning="#f2c25b", error="#ff9a8f",
    success_surface="#1f3a2b", warning_surface="#3f3419", error_surface="#46252a",
    log_background="#111a21", log_text="#d7e3e2",
)
PALETTES = {LIGHT: LIGHT_PALETTE, DARK: DARK_PALETTE}

# ttk style surfaces: widgets sit either on the window background or on a card.
WORKSPACE_SURFACE = "Workspace"
CARD_SURFACE = "Card"


# -- identity -----------------------------------------------------------------

def set_process_name(system: str | None = None) -> bool:
    """Name this process ``ludus-setup`` (Linux) or the app ``Ludus Setup`` (macOS); best effort."""
    system = system or platform.system()
    try:
        if system == "Linux":
            libc = ctypes.CDLL(None, use_errno=True)
            return libc.prctl(LINUX_SET_PROCESS_NAME, ctypes.c_char_p(PROCESS_NAME), 0, 0, 0) == 0
        if system == "Darwin":
            return set_macos_application_name(APPLICATION_NAME)
    except (OSError, AttributeError, ValueError, TypeError):
        return False
    return False


def set_macos_application_name(name: str) -> bool:
    """Set the main bundle's CFBundleName through the Objective-C runtime, without PyObjC.

    It must run before Tk creates its menu bar. Only a mutable info dictionary
    is changed, so an immutable one never raises an Objective-C exception.
    """
    library = ctypes.util.find_library("objc")
    if not library:
        return False
    objc = ctypes.cdll.LoadLibrary(library)
    ctypes.cdll.LoadLibrary("/System/Library/Frameworks/Foundation.framework/Foundation")
    objc.objc_getClass.restype = ctypes.c_void_p
    objc.objc_getClass.argtypes = [ctypes.c_char_p]
    objc.sel_registerName.restype = ctypes.c_void_p
    objc.sel_registerName.argtypes = [ctypes.c_char_p]
    address = ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value
    send = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(address)
    send_object = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(address)
    send_string = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p)(address)
    send_pair = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.c_void_p)(address)
    send_query = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(address)

    def selector(name_bytes: bytes):
        return objc.sel_registerName(name_bytes)

    bundle = send(objc.objc_getClass(b"NSBundle"), selector(b"mainBundle"))
    if not bundle:
        return False
    information = send(bundle, selector(b"infoDictionary"))
    mutable = objc.objc_getClass(b"NSMutableDictionary")
    if not information or not mutable:
        return False
    if not send_query(information, selector(b"isKindOfClass:"), mutable):
        return False
    if not send_query(information, selector(b"respondsToSelector:"), selector(b"setObject:forKey:")):
        return False
    string_class = objc.objc_getClass(b"NSString")
    value = send_string(string_class, selector(b"stringWithUTF8String:"), name.encode("utf-8"))
    key = send_string(string_class, selector(b"stringWithUTF8String:"), b"CFBundleName")
    send_pair(information, selector(b"setObject:forKey:"), value, key)
    return send_object(information, selector(b"objectForKey:"), key) is not None


def create_root(tk, *, system: str | None = None):
    """Create the Tk root with the Ludus Setup name, window class and process name."""
    set_process_name(system)
    root = tk.Tk(className=WINDOW_CLASS)
    root.title(APPLICATION_NAME)
    return root


# -- appearance -----------------------------------------------------------------

def detect_appearance(root=None, *, environment=None, system: str | None = None, run=subprocess.run) -> str:
    """Return ``dark`` or ``light`` for the desktop's current preference."""
    environment = os.environ if environment is None else environment
    system = system or platform.system()
    requested = environment.get(APPEARANCE_ENVIRONMENT, "").strip().lower()
    if requested in PALETTES:
        return requested
    if system == "Darwin":
        if root is None:
            return LIGHT
        try:
            return DARK if bool(int(root.tk.call("::tk::unsupported::MacWindowStyle", "isdark", root))) else LIGHT
        except Exception:  # noqa: BLE001 - TclError or an unexpected reply; Tk older than 8.6.10 lacks isdark.
            return LIGHT
    if system == "Windows":
        return windows_appearance()
    theme = environment.get("GTK_THEME", "").lower()
    if theme.endswith(":dark"):
        return DARK
    try:
        result = run(["gsettings", "get", "org.gnome.desktop.interface", "color-scheme"], capture_output=True,
                     text=True, timeout=APPEARANCE_PROBE_SECONDS, check=False)
    except (OSError, subprocess.SubprocessError):
        return LIGHT
    if result.returncode == 0 and "dark" in result.stdout.lower():
        return DARK
    return LIGHT


def windows_appearance() -> str:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as key:
            value, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
    except (ImportError, OSError):
        return LIGHT
    return DARK if value == 0 else LIGHT


def relative_luminance(colour: str) -> float:
    """WCAG 2 relative luminance of ``#rrggbb``."""
    channels = [int(colour[index:index + 2], 16) / 255.0 for index in (1, 3, 5)]
    linear = [value / 12.92 if value <= 0.03928 else ((value + 0.055) / 1.055) ** 2.4 for value in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def contrast_ratio(first: str, second: str) -> float:
    lighter, darker = sorted((relative_luminance(first), relative_luminance(second)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)


def interface_scale(root, *, environment=None, system: str | None = None) -> float:
    """Logical-to-device pixel factor; also adjusts Tk's point scaling when overridden."""
    environment = os.environ if environment is None else environment
    system = system or platform.system()
    requested = environment.get(SCALE_ENVIRONMENT, "")
    try:
        scale = float(requested) if requested else 0.0
    except ValueError:
        scale = 0.0
    if scale <= 0.0:
        if system == "Darwin":
            return 1.0
        try:
            scale = max(1.0, float(root.winfo_fpixels("1i")) / REFERENCE_DOTS_PER_INCH)
        except Exception:  # noqa: BLE001 - a stub or a closing display.
            scale = 1.0
        desktop = environment.get("GDK_SCALE", "")
        if desktop.isdigit() and int(desktop) > scale:
            scale = float(int(desktop))
        else:
            return scale
    try:
        root.tk.call("tk", "scaling", scale * REFERENCE_DOTS_PER_INCH / POINTS_PER_INCH)
    except Exception:  # noqa: BLE001
        pass
    return scale


# -- theme ----------------------------------------------------------------------

@dataclass
class Fonts:
    body: object
    small: object
    heading: object
    title: object
    mono: object
    strong: object


def create_fonts(root, system: str) -> Fonts:
    from tkinter import font as tkfont
    default = tkfont.nametofont("TkDefaultFont", root=root).actual()
    family = default.get("family") or "TkDefaultFont"
    size = int(default.get("size") or FALLBACK_FONT_POINTS)
    if size < 0:
        # Negative Tk sizes are pixels; convert to points.
        size = max(MINIMUM_FONT_POINTS, round(-size * POINTS_PER_INCH / float(root.winfo_fpixels("1i"))))
    monospace = tkfont.nametofont("TkFixedFont", root=root).actual().get("family") or "TkFixedFont"
    available = set(tkfont.families(root))
    for candidate in PREFERRED_MONOSPACE.get(system, ()):
        if candidate in available:
            monospace = candidate
            break
    small = max(MINIMUM_FONT_POINTS, size + SMALL_SIZE_STEP)

    def make(points, weight="normal", typeface=family):
        return tkfont.Font(root=root, family=typeface, size=points, weight=weight)
    return Fonts(body=make(size), small=make(small), heading=make(size + HEADING_SIZE_STEP, "bold"),
                 title=make(size + TITLE_SIZE_STEP, "bold"), mono=make(small, typeface=monospace),
                 strong=make(size, "bold"))


class Theme:
    """Fonts, colours, spacing, ttk styles and icons for one Tk root."""

    def __init__(self, root, ttk, *, appearance: str | None = None, environment=None, system: str | None = None):
        self.root = root
        self.ttk = ttk
        self.system = system or platform.system()
        self.appearance = appearance or detect_appearance(root, environment=environment, system=self.system)
        self.palette = PALETTES[self.appearance]
        self.scale = interface_scale(root, environment=environment, system=self.system)
        self.fonts = create_fonts(root, self.system)
        self._images: dict[int, object] = {}
        self.style = ttk.Style(root)
        self._configure_styles()

    def space(self, points: float) -> int:
        """Convert logical pixels to device pixels for padding and sizes."""
        return int(round(points * self.scale))

    def surface_colour(self, surface: str) -> str:
        return self.palette.panel if surface == CARD_SURFACE else self.palette.workspace

    # -- icons

    def image(self, size: int):
        """The bundled Ludus icon at ``size`` pixels, or None when it cannot be loaded."""
        if size in self._images:
            return self._images[size]
        path = ASSET_DIRECTORY / f"ludus-icon-{size}.png"
        try:
            import tkinter as tk
            image = tk.PhotoImage(master=self.root, file=str(path))
        except Exception:  # noqa: BLE001 - TclError, missing file or a Tk without PNG support.
            image = None
        self._images[size] = image
        return image

    def header_image(self):
        wanted = self.space(HEADER_ICON_POINTS)
        size = next((candidate for candidate in ICON_SIZES if candidate >= wanted), ICON_SIZES[-1])
        return self.image(size)

    def apply_window_icon(self, window) -> bool:
        images = [image for image in (self.image(size) for size in reversed(WINDOW_ICON_SIZES)) if image is not None]
        if not images:
            return False
        try:
            window.iconphoto(True, *images)
        except Exception:  # noqa: BLE001
            return False
        return True

    # -- styles

    def _configure_styles(self) -> None:
        palette, fonts, style = self.palette, self.fonts, self.style
        if "clam" in style.theme_names():
            style.theme_use("clam")
        self.root.configure(background=palette.workspace)
        style.configure(".", background=palette.workspace, foreground=palette.text, fieldbackground=palette.panel,
                        bordercolor=palette.border, lightcolor=palette.workspace, darkcolor=palette.workspace,
                        troughcolor=palette.raised, focuscolor=palette.focus, selectbackground=palette.selected,
                        selectforeground=palette.selected_text, insertcolor=palette.text, font=fonts.body)
        style.map(".", foreground=[("disabled", palette.disabled_text)])
        for surface in (WORKSPACE_SURFACE, CARD_SURFACE):
            background = self.surface_colour(surface)
            style.configure(f"{surface}.TFrame", background=background)
            style.configure(f"{surface}.TLabel", background=background, foreground=palette.text, font=fonts.body)
            style.configure(f"{surface}.Strong.TLabel", background=background, foreground=palette.text,
                            font=fonts.strong)
            style.configure(f"{surface}.Secondary.TLabel", background=background, foreground=palette.secondary_text,
                            font=fonts.small)
            style.configure(f"{surface}.Heading.TLabel", background=background, foreground=palette.text,
                            font=fonts.heading)
            style.configure(f"{surface}.Title.TLabel", background=background, foreground=palette.text,
                            font=fonts.title)
            style.configure(f"{surface}.Mono.TLabel", background=background, foreground=palette.secondary_text,
                            font=fonts.mono)
            style.configure(f"{surface}.TCheckbutton", background=background, foreground=palette.text,
                            indicatorbackground=palette.panel, indicatorforeground=palette.on_primary,
                            indicatorsize=self.space(CHECKBOX_INDICATOR_POINTS),
                            indicatormargin=(0, 0, self.space(SPACE_SMALL), 0), padding=(0, self.space(2)),
                            focuscolor=palette.focus, font=fonts.body)
            style.map(f"{surface}.TCheckbutton",
                      background=[("active", background)],
                      foreground=[("disabled", palette.disabled_text)],
                      indicatorbackground=[("disabled", palette.raised), ("selected", palette.primary),
                                           ("pressed", palette.selected)],
                      indicatorforeground=[("disabled", palette.disabled_text)],
                      upperbordercolor=[("focus", palette.focus)], lowerbordercolor=[("focus", palette.focus)])
        # Cards differ from the window by surface colour; their edge is decorative,
        # so it uses the softer card border rather than the 3:1 control border.
        style.configure("Card.TFrame", background=palette.panel, bordercolor=palette.card_border, relief="solid",
                        borderwidth=FOCUS_THICKNESS_POINTS)
        style.configure("Panel.TFrame", background=palette.panel)
        style.configure("Header.TFrame", background=palette.panel)
        for name in ("Title", "Secondary", "Mono", "TLabel", "Strong", "Heading"):
            label = "Header.TLabel" if name == "TLabel" else f"Header.{name}.TLabel"
            base = "Card.TLabel" if name == "TLabel" else f"Card.{name}.TLabel"
            style.configure(label, **{key: style.lookup(base, key) for key in ("background", "foreground", "font")})
        style.configure("Rule.TFrame", background=palette.card_border)

        padding = (self.space(SPACE_LARGE), self.space(SPACE_SMALL) - 1)
        style.configure("TButton", background=palette.raised, foreground=palette.text, bordercolor=palette.border,
                        lightcolor=palette.raised, darkcolor=palette.raised, focuscolor=palette.focus,
                        focusthickness=FOCUS_THICKNESS_POINTS, padding=padding, font=fonts.body, anchor="center")
        style.map("TButton",
                  background=[("disabled", palette.workspace), ("pressed", palette.selected),
                              ("active", palette.selected)],
                  lightcolor=[("disabled", palette.workspace), ("pressed", palette.selected),
                              ("active", palette.selected)],
                  darkcolor=[("disabled", palette.workspace), ("pressed", palette.selected),
                             ("active", palette.selected)],
                  bordercolor=[("focus", palette.focus)],
                  foreground=[("disabled", palette.disabled_text)])
        style.configure("Primary.TButton", background=palette.primary, foreground=palette.on_primary,
                        bordercolor=palette.primary, lightcolor=palette.primary, darkcolor=palette.primary,
                        focuscolor=palette.on_primary, font=fonts.strong)
        style.map("Primary.TButton",
                  background=[("disabled", palette.raised), ("pressed", palette.primary_active),
                              ("active", palette.primary_active)],
                  lightcolor=[("disabled", palette.raised), ("pressed", palette.primary_active),
                              ("active", palette.primary_active)],
                  darkcolor=[("disabled", palette.raised), ("pressed", palette.primary_active),
                             ("active", palette.primary_active)],
                  bordercolor=[("disabled", palette.raised), ("focus", palette.text)],
                  foreground=[("disabled", palette.disabled_text)])
        for surface in (WORKSPACE_SURFACE, CARD_SURFACE):
            background = self.surface_colour(surface)
            style.configure(f"{surface}.Link.TButton", background=background, foreground=palette.focus,
                            bordercolor=background, lightcolor=background, darkcolor=background,
                            padding=(0, self.space(SPACE_EXTRA_SMALL)), font=fonts.body, anchor="w")
            style.map(f"{surface}.Link.TButton",
                      background=[("active", background), ("pressed", background)],
                      lightcolor=[("active", background), ("pressed", background)],
                      darkcolor=[("active", background), ("pressed", background)],
                      bordercolor=[("focus", palette.focus)],
                      foreground=[("disabled", palette.disabled_text), ("active", palette.primary_active)])

        style.configure("TCombobox", fieldbackground=palette.panel, background=palette.raised,
                        foreground=palette.text, arrowcolor=palette.text, bordercolor=palette.border,
                        lightcolor=palette.panel, darkcolor=palette.panel, padding=self.space(SPACE_EXTRA_SMALL),
                        arrowsize=self.space(SPACE_MEDIUM))
        style.map("TCombobox",
                  fieldbackground=[("readonly", palette.panel), ("disabled", palette.raised)],
                  foreground=[("disabled", palette.disabled_text), ("readonly", palette.text)],
                  selectbackground=[("readonly", palette.panel)], selectforeground=[("readonly", palette.text)],
                  background=[("active", palette.selected), ("pressed", palette.selected)],
                  bordercolor=[("focus", palette.focus)], lightcolor=[("focus", palette.focus)],
                  darkcolor=[("focus", palette.focus)])
        for option, value in (("background", palette.panel), ("foreground", palette.text),
                              ("selectBackground", palette.selected), ("selectForeground", palette.selected_text)):
            self.root.option_add(f"*TCombobox*Listbox.{option}", value)
        self.root.option_add("*TCombobox*Listbox.font", fonts.body)

        style.configure("Ludus.Horizontal.TProgressbar", troughcolor=palette.raised, background=palette.primary,
                        bordercolor=palette.raised, lightcolor=palette.primary, darkcolor=palette.primary,
                        thickness=self.space(PROGRESS_THICKNESS_POINTS))
        style.configure("Vertical.TScrollbar", background=palette.raised, troughcolor=palette.log_background,
                        bordercolor=palette.log_background, arrowcolor=palette.secondary_text,
                        lightcolor=palette.raised, darkcolor=palette.raised)
        style.map("Vertical.TScrollbar", background=[("active", palette.selected)])
        style.configure("TSeparator", background=palette.border)
        # Classic Tk widgets, including those of message boxes on X11.
        for option, value in (("*Background", palette.workspace), ("*Foreground", palette.text),
                              ("*activeBackground", palette.selected), ("*activeForeground", palette.text),
                              ("*selectBackground", palette.selected), ("*selectForeground", palette.selected_text),
                              ("*highlightColor", palette.focus), ("*highlightBackground", palette.workspace)):
            self.root.option_add(option, value)

    # -- widgets

    def status_icon(self, tk, parent, surface: str = CARD_SURFACE) -> StatusIcon:
        return StatusIcon(tk, parent, self, self.surface_colour(surface), STATUS_ICON_POINTS)

    def banner(self, tk, parent, kind: str) -> Banner:
        return Banner(tk, parent, self, kind)


def status_colour(palette: Palette, status: str) -> str:
    return {
        init_progress.PENDING: palette.border,
        init_progress.RUNNING: palette.focus,
        init_progress.DONE: palette.success,
        init_progress.FAILED: palette.error,
        init_progress.CANCELLED: palette.warning,
        WARNING: palette.warning,
        ERROR: palette.error,
        SUCCESS: palette.success,
    }[status]


class StatusIcon:
    """A small canvas symbol: hollow ring, spinner, tick, cross, dash or exclamation mark."""

    def __init__(self, tk, parent, theme: Theme, background: str, points: int):
        self.theme = theme
        self.size = theme.space(points)
        self.canvas = tk.Canvas(parent, width=self.size, height=self.size, highlightthickness=0, borderwidth=0,
                                background=background, takefocus=0)
        self.status = ""
        self.angle = 0

    def grid(self, **options) -> None:
        self.canvas.grid(**options)

    def pack(self, **options) -> None:
        self.canvas.pack(**options)

    def destroy(self) -> None:
        self.canvas.destroy()

    def configure_background(self, colour: str) -> None:
        self.canvas.configure(background=colour)

    def advance(self) -> None:
        """Turn the spinner by one step; other states do not animate."""
        self.angle = (self.angle - SPINNER_STEP_DEGREES) % FULL_TURN_DEGREES
        if self.status == init_progress.RUNNING:
            self._draw()

    def show(self, status: str) -> None:
        if status != self.status:
            self.status = status
            self._draw()

    def _draw(self) -> None:
        canvas, size, palette = self.canvas, self.size, self.theme.palette
        canvas.delete("all")
        stroke = max(MINIMUM_STROKE_PIXELS, size / SYMBOL_STROKE_DIVISOR)
        inset = stroke / 2 + 1
        box = (inset, inset, size - inset, size - inset)
        colour = status_colour(palette, self.status)
        mark = palette.panel if self.theme.appearance == LIGHT else palette.workspace
        if self.status == init_progress.PENDING:
            canvas.create_oval(*box, outline=colour, width=max(1.0, size / PENDING_STROKE_DIVISOR))
            return
        if self.status == init_progress.RUNNING:
            canvas.create_oval(*box, outline=palette.raised, width=stroke)
            canvas.create_arc(*box, start=self.angle, extent=SPINNER_EXTENT_DEGREES, style="arc", outline=colour,
                              width=stroke)
            return
        canvas.create_oval(0, 0, size, size, fill=colour, outline=colour)
        if self.status in (init_progress.DONE, SUCCESS):
            canvas.create_line(*[value * size for point in CHECK_POINTS for value in point], fill=mark, width=stroke,
                               capstyle="round", joinstyle="round")
        elif self.status in (init_progress.FAILED, ERROR):
            near, far = CROSS_INSET * size, (1 - CROSS_INSET) * size
            canvas.create_line(near, near, far, far, fill=mark, width=stroke, capstyle="round")
            canvas.create_line(far, near, near, far, fill=mark, width=stroke, capstyle="round")
        elif self.status == init_progress.CANCELLED:
            canvas.create_line(DASH_INSET * size, size / 2, (1 - DASH_INSET) * size, size / 2, fill=mark,
                               width=stroke, capstyle="round")
        else:
            canvas.create_line(size / 2, EXCLAMATION_TOP * size, size / 2, EXCLAMATION_BOTTOM * size, fill=mark,
                               width=stroke, capstyle="round")
            radius = max(1.0, DOT_RADIUS_FRACTION * size)
            canvas.create_oval(size / 2 - radius, EXCLAMATION_DOT * size - radius, size / 2 + radius,
                               EXCLAMATION_DOT * size + radius, fill=mark, outline=mark)


class Banner:
    """A tinted message row with a symbol: warning (sudo prompt), error or success."""

    def __init__(self, tk, parent, theme: Theme, kind: str):
        self.theme = theme
        self.frame = tk.Frame(parent, borderwidth=0, highlightthickness=0)
        self.icon = StatusIcon(tk, self.frame, theme, theme.palette.panel, BANNER_ICON_POINTS)
        self.icon.canvas.grid(row=0, column=0, sticky="n", padx=(theme.space(SPACE_MEDIUM), theme.space(SPACE_SMALL)),
                              pady=theme.space(SPACE_MEDIUM))
        self.text = tk.StringVar(master=parent, value="")
        self.label = tk.Label(self.frame, textvariable=self.text, anchor="w", justify="left", font=theme.fonts.body,
                              foreground=theme.palette.text, borderwidth=0)
        self.label.grid(row=0, column=1, sticky="ew", padx=(0, theme.space(SPACE_MEDIUM)),
                        pady=theme.space(SPACE_MEDIUM))
        self.frame.columnconfigure(1, weight=1)
        self.set_kind(kind)

    def set_kind(self, kind: str) -> None:
        palette = self.theme.palette
        surface = {WARNING: palette.warning_surface, ERROR: palette.error_surface,
                   SUCCESS: palette.success_surface}[kind]
        for widget in (self.frame, self.label):
            widget.configure(background=surface)
        self.icon.configure_background(surface)
        self.icon.status = ""
        self.icon.show(kind)

    def set_wrap(self, pixels: int) -> None:
        """Wrap the message so the whole banner, symbol and padding included, is ``pixels`` wide."""
        chrome = self.theme.space(BANNER_ICON_POINTS + SPACE_MEDIUM + SPACE_SMALL + SPACE_MEDIUM)
        self.label.configure(wraplength=max(1, pixels - chrome))


def centre_window(window) -> None:
    """Place ``window`` in the middle of its screen at its requested size."""
    window.update_idletasks()
    width, height = window.winfo_reqwidth(), window.winfo_reqheight()
    x = max(0, (window.winfo_screenwidth() - width) // 2)
    y = max(0, (window.winfo_screenheight() - height) // 2)
    window.geometry(f"+{x}+{y}")


def fit_minimum_size(window) -> None:
    """Never let the window shrink below the size its content asks for."""
    window.update_idletasks()
    window.minsize(window.winfo_reqwidth(), window.winfo_reqheight())
