"""Shared identity and look of the ./init.sh setup windows.

The option selector (``init_ui``) and the progress window (``init_window``)
both create their Tk root with ``create_root`` and style it with ``Theme``.

Identity
    Every window is titled "Ludus Setup". On X11 the root's WM_CLASS is
    ``ludus-setup`` / ``Ludus-setup`` (Tk derives both from ``className``), so
    taskbars group and label it as Ludus Setup rather than "Tk". On Linux the
    process name that ``top``, ``ps -o comm`` and system monitors show becomes
    ``ludus-setup`` through ``prctl``. On macOS the application menu uses the
    main bundle's ``CFBundleName``, which is set before Tk creates the menu
    bar. The Dock reads that name from the bundle on disk instead, so
    ``init_launcher`` runs setup from a generated ``Ludus Setup.app``; this
    run-time name covers ``engine.py init`` started directly. ``iconphoto``
    replaces the Python rocket in the Dock, and
    clicking that Dock icon brings the current window to the front. Both
    windows share one Tk root (see ``create_root``). The full
    command line in ``ps`` still names the interpreter: rewriting ``argv``
    needs the third-party ``setproctitle`` package, which setup does not
    install.

Appearance
    Colours are the semantic tokens of the Ludus Block identity in
    config/design-tokens.json (see docs/architecture/editor-art-direction.md),
    shared with the Editor and the browser editor, in its dark and light
    appearances, plus success, warning and error roles that keep at least
    4.5:1 contrast on every surface they are drawn on (see the tests). The
    appearance follows the desktop (macOS dark mode, GNOME ``color-scheme``,
    ``GTK_THEME`` ending in ``:dark``, the Windows app theme) unless
    ``LUDUS_SETUP_APPEARANCE`` is ``dark`` or ``light``. When the desktop asks
    for more contrast (Windows high contrast, macOS "Increase contrast", GNOME
    ``high-contrast``) or ``LUDUS_HIGH_CONTRAST`` is 1, the tokens' ``highContrast``
    values replace the standard ones, as in the Editor. Spacing uses the
    4/8/12/16/24 logical pixel scale, multiplied by the display scale on
    X11/Windows (macOS already measures in points); ``LUDUS_SETUP_SCALE``
    overrides it.

Icons
    ``init_assets/ludus-icon-<size>.png`` for 16, 24 and 32 are drawn, not scaled:
    ``render_small_icons.py`` redraws the tile and the L on the pixel grid with
    flat brand planes, so the silhouette stays clear at small sizes. The larger
    sizes are Lanczos downscales of the approved docs/wiki/assets/ludus-icon.png.
    Regenerate those with ImageMagick::

        for size in 48 64 96 128 256; do
            convert docs/wiki/assets/ludus-icon.png -filter Lanczos -resize ${size}x${size} \\
                -strip PNG32:scripts/python/init_assets/ludus-icon-${size}.png
        done

    Step states and banners use small vector symbols drawn on a canvas, so
    they look the same on every platform and stay sharp at any scale. Check
    boxes are the Editor's (a rounded ``size.checkbox`` box with the control
    border, filled with the accent and ticked when on), drawn into images at
    the display scale because Tk's own indicator is an X on some platforms.

Size
    A window is never taller than its screen's usable height (on macOS the
    area the menu bar and Dock leave free, read from AppKit); the option
    selector scrolls its options so the header and the Cancel and Initialize
    buttons stay on screen.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
import subprocess
from dataclasses import dataclass
from pathlib import Path

import design_tokens
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
HIGH_CONTRAST_ENVIRONMENT = "LUDUS_HIGH_CONTRAST"
SCALE_ENVIRONMENT = "LUDUS_SETUP_SCALE"
DARK = "dark"
LIGHT = "light"
APPEARANCE_PROBE_SECONDS = 2
SPI_GETHIGHCONTRAST = 0x0042
HCF_HIGHCONTRASTON = 0x00000001

# Linux prctl option that renames the calling thread (and, for the main
# thread, the process "comm" field). Names longer than 15 bytes are cut.
LINUX_SET_PROCESS_NAME = 15
REFERENCE_DOTS_PER_INCH = 96.0
POINTS_PER_INCH = 72.0

# Spacing scale in logical pixels, from config/design-tokens.json.
TOKENS = design_tokens.load()
SPACE_EXTRA_SMALL = TOKENS["spacing"]["extraSmall"]
SPACE_SMALL = TOKENS["spacing"]["small"]
SPACE_MEDIUM = TOKENS["spacing"]["medium"]
SPACE_LARGE = TOKENS["spacing"]["large"]
SPACE_EXTRA_LARGE = TOKENS["spacing"]["extraLarge"]

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
CHECKBOX_INDICATOR_POINTS = TOKENS["size"]["checkbox"]
CONTROL_RADIUS_POINTS = TOKENS["radius"]["control"]
# The Editor's check box (apps/editor/src/editor_style.cpp): tick stroke and its points as fractions of the box.
CHECKBOX_TICK_STROKE_POINTS = 1.5
CHECKBOX_TICK_POINTS = ((0.27, 0.52), (0.43, 0.68), (0.74, 0.34))
# Samples per pixel edge when the check box is drawn, so its rounded corners and tick are smooth.
CHECKBOX_SUPERSAMPLING = 4
# Room kept free around a window for the title bar and, where Tk cannot see them, the menu bar, Dock or taskbar.
WINDOW_FRAME_ALLOWANCE_POINTS = 48
USABLE_SCREEN_FRACTION = 0.9
# A window never asks to be shorter than this, or its whole height when that is less.
MINIMUM_WINDOW_HEIGHT_POINTS = 360
SCROLL_STEP_POINTS = 24
FOCUS_THICKNESS_POINTS = 1
HIGH_CONTRAST_FOCUS_THICKNESS_POINTS = 2
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


def build_palette(appearance: str, is_high_contrast: bool = False) -> Palette:
    """The init window's roles, resolved from the shared tokens for one appearance."""
    def token(name: str) -> str:
        return design_tokens.colour(TOKENS, name, appearance, is_high_contrast)

    return Palette(
        workspace=token("surface.workspace"), panel=token("surface.panel"), raised=token("surface.raised"),
        text=token("text.primary"), secondary_text=token("text.secondary"), disabled_text=token("text.disabled"),
        border=token("border.control"), card_border=token("border.subtle"),
        primary=token("accent.primary"), primary_active=token("accent.primaryActive"),
        on_primary=token("accent.onPrimary"), focus=token("accent.primary"),
        selected=token("accent.surface"), selected_text=token("text.primary"),
        success=token("status.success"), warning=token("status.warning"), error=token("status.danger"),
        success_surface=token("status.successSurface"), warning_surface=token("status.warningSurface"),
        error_surface=token("status.dangerSurface"),
        log_background=token("surface.log"), log_text=token("text.log"),
    )


LIGHT_PALETTE = build_palette(LIGHT)
DARK_PALETTE = build_palette(DARK)
PALETTES = {LIGHT: LIGHT_PALETTE, DARK: DARK_PALETTE}
HIGH_CONTRAST_PALETTES = {LIGHT: build_palette(LIGHT, True), DARK: build_palette(DARK, True)}

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


# Every Tk root this process created, with the tkinter module that made it.
# A root is reused by later windows and its Tcl interpreter is never deleted
# before the process exits; see ``create_root``.
_shared_roots: list[tuple[object, object]] = []

# Run when the Dock icon is clicked (Tk calls ::tk::mac::ReopenApplication for
# the kAEReopenApplication Apple event): bring the current window back from the
# Dock and to the front. A withdrawn root (between the option selector and the
# progress window) stays hidden instead of appearing empty.
MACOS_REOPEN_HANDLER = """
namespace eval ::tk::mac {}
proc ::tk::mac::ReopenApplication {} {
    if {[winfo exists .] == 0 || [wm state .] eq "withdrawn"} {
        return
    }
    wm deiconify .
    raise .
    focus -force .
}
"""


def create_root(tk, *, system: str | None = None):
    """Return the Tk root with the Ludus Setup name, window class and process name.

    The option selector and the progress window share one root, and its Tcl
    interpreter lives until the process exits. On macOS, Tk hands the first
    interpreter it initializes to the application object, which uses it for
    every later Apple event (Dock clicks, Quit, Preferences) and never updates
    it. Deleting that interpreter and creating a second root left a dangling
    pointer, so clicking the Dock icon crashed the process with a bus error
    inside ``handleReopenApplicationEvent``.
    """
    system = system or platform.system()
    root = shared_root(tk)
    if root is None:
        set_process_name(system)
        root = tk.Tk(className=WINDOW_CLASS)
        _shared_roots.append((tk, root))
    else:
        root.deiconify()
    root.title(APPLICATION_NAME)
    if system == "Darwin":
        install_reopen_handler(root)
    return root


def shared_root(tk):
    """The live root an earlier ``create_root`` made with ``tk``, or None."""
    for module, root in reversed(_shared_roots):
        if module is not tk:
            continue
        try:
            return root if bool(root.winfo_exists()) else None
        except Exception:  # noqa: BLE001 - TclError: the main window was destroyed.
            return None
    return None


def install_reopen_handler(root) -> bool:
    """Define the macOS Dock reopen handler in the root's interpreter; best effort."""
    try:
        root.tk.eval(MACOS_REOPEN_HANDLER)
    except Exception:  # noqa: BLE001 - TclError, or a stub root in tests.
        return False
    return True


def release_root(root) -> None:
    """Hide the root and clear what one window put on it, keeping its interpreter alive.

    Call this instead of ``destroy`` when a window is done; the next
    ``create_root`` reuses the root.
    """
    try:
        root.withdraw()
        reset_root(root)
    except Exception:  # noqa: BLE001 - TclError from a destroyed root.
        pass


def reset_root(root) -> None:
    """Remove widgets, bindings, timers, the close handler and the grid layout of ``root``."""
    for child in list(root.winfo_children()):
        child.destroy()
    for sequence in root.bind():
        root.unbind(sequence)
    for timer in root.tk.splitlist(root.tk.call("after", "info")):
        root.after_cancel(timer)
    root.tk.call("wm", "protocol", root._w, "WM_DELETE_WINDOW", "")
    columns, rows = root.grid_size()
    for column in range(columns):
        root.columnconfigure(column, weight=0, minsize=0, uniform="", pad=0)
    for row in range(rows):
        root.rowconfigure(row, weight=0, minsize=0, uniform="", pad=0)
    root.minsize(1, 1)
    root.geometry("")


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


def detect_high_contrast(*, environment=None, system: str | None = None, run=subprocess.run) -> bool:
    """True when the desktop asks for more contrast, as the Editor's ``theme_preferences`` reads it.

    ``LUDUS_HIGH_CONTRAST`` (1/true/on or 0/false/off) wins over the system.
    """
    environment = os.environ if environment is None else environment
    requested = environment.get(HIGH_CONTRAST_ENVIRONMENT, "").strip().lower()
    if requested in ("1", "true", "on"):
        return True
    if requested in ("0", "false", "off"):
        return False
    system = system or platform.system()
    if system == "Windows":
        return windows_high_contrast()
    if system == "Darwin":
        command, positive = ["defaults", "read", "com.apple.universalaccess", "increaseContrast"], "1"
    else:
        command, positive = ["gsettings", "get", "org.gnome.desktop.a11y.interface", "high-contrast"], "true"
    try:
        result = run(command, capture_output=True, text=True, timeout=APPEARANCE_PROBE_SECONDS, check=False)
    except (OSError, subprocess.SubprocessError):
        return False
    return result.returncode == 0 and result.stdout.strip().lower() == positive


def windows_high_contrast() -> bool:
    """The ``HCF_HIGHCONTRASTON`` flag of ``SystemParametersInfo(SPI_GETHIGHCONTRAST)``."""
    try:
        class HighContrast(ctypes.Structure):
            _fields_ = [("cbSize", ctypes.c_uint), ("dwFlags", ctypes.c_uint), ("lpszDefaultScheme", ctypes.c_void_p)]

        state = HighContrast(ctypes.sizeof(HighContrast), 0, None)
        if not ctypes.windll.user32.SystemParametersInfoW(SPI_GETHIGHCONTRAST, state.cbSize, ctypes.byref(state), 0):
            return False
    except (AttributeError, OSError):
        return False
    return bool(state.dwFlags & HCF_HIGHCONTRASTON)


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

    def __init__(self, root, ttk, *, appearance: str | None = None, environment=None, system: str | None = None,
                 high_contrast: bool | None = None):
        self.root = root
        self.ttk = ttk
        self.system = system or platform.system()
        self.appearance = appearance or detect_appearance(root, environment=environment, system=self.system)
        self.is_high_contrast = (detect_high_contrast(environment=environment, system=self.system)
                                 if high_contrast is None else high_contrast)
        self.palette = (HIGH_CONTRAST_PALETTES if self.is_high_contrast else PALETTES)[self.appearance]
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
        focus_thickness = HIGH_CONTRAST_FOCUS_THICKNESS_POINTS if self.is_high_contrast else FOCUS_THICKNESS_POINTS
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
                            padding=(0, self.space(2)), focuscolor=palette.focus, font=fonts.body)
            style.map(f"{surface}.TCheckbutton",
                      background=[("active", background)],
                      foreground=[("disabled", palette.disabled_text)])
            # The theme's own indicator is an X on some platforms; draw the Editor's box and tick instead.
            style.layout(f"{surface}.TCheckbutton", [("Checkbutton.padding", {"sticky": "nswe", "children": [
                (self._checkbox_element(surface), {"side": "left", "sticky": ""}),
                ("Checkbutton.focus", {"side": "left", "sticky": "w", "children": [
                    ("Checkbutton.label", {"sticky": "nswe"})]})]})])
        # Cards differ from the window by surface colour; their edge is decorative,
        # so it uses the softer card border rather than the 3:1 control border.
        style.configure("Card.TFrame", background=palette.panel, bordercolor=palette.card_border, relief="solid",
                        borderwidth=focus_thickness)
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
                        focusthickness=focus_thickness, padding=padding, font=fonts.body, anchor="center")
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
        style.configure("Workspace.Vertical.TScrollbar", background=palette.raised, troughcolor=palette.workspace,
                        bordercolor=palette.workspace, arrowcolor=palette.secondary_text,
                        lightcolor=palette.raised, darkcolor=palette.raised)
        style.map("Workspace.Vertical.TScrollbar", background=[("active", palette.selected)])
        style.configure("TSeparator", background=palette.border)
        # Classic Tk widgets, including those of message boxes on X11.
        for option, value in (("*Background", palette.workspace), ("*Foreground", palette.text),
                              ("*activeBackground", palette.selected), ("*activeForeground", palette.text),
                              ("*selectBackground", palette.selected), ("*selectForeground", palette.selected_text),
                              ("*highlightColor", palette.focus), ("*highlightBackground", palette.workspace)):
            self.root.option_add(option, value)

    # -- widgets

    def _checkbox_element(self, surface: str) -> str:
        """Create (once per look) the image element that draws check boxes on ``surface`` and return its name."""
        palette = self.palette
        size = self.space(CHECKBOX_INDICATOR_POINTS)
        gap = self.space(SPACE_SMALL)
        edge_width = self.space(HIGH_CONTRAST_FOCUS_THICKNESS_POINTS if self.is_high_contrast
                                else FOCUS_THICKNESS_POINTS)
        background = self.surface_colour(surface)
        key = (surface, background, palette.panel, palette.border, palette.primary, palette.on_primary,
               palette.disabled_text, size, gap, edge_width)
        name = f"{surface}.Ludus{abs(hash(key)):x}.indicator"
        # Elements and images live in the Tk interpreter, which every window of this process shares. The images are
        # created by name rather than as PhotoImage objects, so they last as long as the interpreter that uses them.
        images = self.root.__dict__.setdefault("_ludus_checkbox_images", {})
        if name in images:
            return name

        def draw(state: str, fill: str, edge: str, mark: str | None) -> str:
            image = f"{name}.{state}"
            rows = render_checkbox(size, gap, background=background, fill=fill, edge=edge, mark=mark,
                                   edge_width=edge_width, radius=self.space(CONTROL_RADIUS_POINTS),
                                   stroke=max(1.0, CHECKBOX_TICK_STROKE_POINTS * self.scale))
            self.root.tk.call("image", "create", "photo", image, "-width", size + gap, "-height", size)
            self.root.tk.call(image, "put", " ".join("{" + " ".join(row) + "}" for row in rows))
            return image

        states = {
            "unchecked": draw("unchecked", palette.panel, palette.border, None),
            "hovered": draw("hovered", palette.panel, palette.primary, None),
            "checked": draw("checked", palette.primary, palette.primary, palette.on_primary),
            "disabled": draw("disabled", palette.panel, palette.disabled_text, None),
            "disabled_checked": draw("disabled_checked", palette.panel, palette.disabled_text, palette.disabled_text),
        }
        self.style.element_create(name, "image", states["unchecked"],
                                  ("disabled", "selected", states["disabled_checked"]),
                                  ("disabled", states["disabled"]),
                                  ("selected", states["checked"]),
                                  ("active", states["hovered"]),
                                  sticky="")
        images[name] = states
        return name

    def status_icon(self, tk, parent, surface: str = CARD_SURFACE) -> StatusIcon:
        return StatusIcon(tk, parent, self, self.surface_colour(surface), STATUS_ICON_POINTS)

    def banner(self, tk, parent, kind: str) -> Banner:
        return Banner(tk, parent, self, kind)


def _hex_to_rgb(colour: str) -> tuple[int, int, int]:
    return int(colour[1:3], 16), int(colour[3:5], 16), int(colour[5:7], 16)


def _rounded_box_distance(x: float, y: float, size: float, radius: float) -> float:
    """Signed distance from (x, y) to a ``size`` square with ``radius`` corners at the origin; negative inside."""
    half = size / 2
    qx = abs(x - half) - (half - radius)
    qy = abs(y - half) - (half - radius)
    outside = (max(qx, 0.0) ** 2 + max(qy, 0.0) ** 2) ** 0.5
    return outside + min(max(qx, qy), 0.0) - radius


def _segment_distance(x: float, y: float, start: tuple[float, float], end: tuple[float, float]) -> float:
    dx, dy = end[0] - start[0], end[1] - start[1]
    along = max(0.0, min(1.0, ((x - start[0]) * dx + (y - start[1]) * dy) / (dx * dx + dy * dy)))
    return ((x - start[0] - along * dx) ** 2 + (y - start[1] - along * dy) ** 2) ** 0.5


def render_checkbox(size: int, gap: int, *, background: str, fill: str, edge: str, mark: str | None,
                    edge_width: int, radius: float, stroke: float) -> list[list[str]]:
    """Rows of ``#rrggbb`` pixels: a rounded box with an edge, a tick when ``mark`` is set, then ``gap`` columns.

    Tk images have no partial transparency, so the box is blended into the surface colour it sits on. Each pixel
    averages ``CHECKBOX_SUPERSAMPLING`` squared samples, which keeps the corners and the tick smooth.
    """
    colours = {name: _hex_to_rgb(value) for name, value in
               (("background", background), ("fill", fill), ("edge", edge), ("mark", mark or fill))}
    tick = [(x * size, y * size) for x, y in CHECKBOX_TICK_POINTS]
    samples = CHECKBOX_SUPERSAMPLING
    background_hex = background.lower()
    rows = []
    for row in range(size):
        pixels = []
        for column in range(size):
            totals = [0, 0, 0]
            for sample_y in range(samples):
                for sample_x in range(samples):
                    x = column + (sample_x + 0.5) / samples
                    y = row + (sample_y + 0.5) / samples
                    distance = _rounded_box_distance(x, y, size, radius)
                    if distance > 0:
                        colour = colours["background"]
                    elif distance > -edge_width:
                        colour = colours["edge"]
                    elif mark is not None and min(_segment_distance(x, y, tick[index], tick[index + 1])
                                                  for index in range(len(tick) - 1)) <= stroke / 2:
                        colour = colours["mark"]
                    else:
                        colour = colours["fill"]
                    for channel in range(3):
                        totals[channel] += colour[channel]
            count = samples * samples
            pixels.append("#" + "".join(f"{round(total / count):02x}" for total in totals))
        rows.append(pixels + [background_hex] * gap)
    return rows


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
        edge_padding = 1
        inset = stroke / 2 + edge_padding
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
        canvas.create_oval(edge_padding, edge_padding, size - edge_padding, size - edge_padding,
                           fill=colour, outline="")
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


class ScrollArea:
    """A vertically scrolling frame whose scroll bar appears only while its ``content`` is taller than it.

    The area asks for its content's whole size, so it costs nothing on a screen with room. The scroll bar floats
    over the right edge (the content's own margin) instead of taking width, so showing it never rewraps text.
    """

    def __init__(self, tk, ttk, parent, theme: Theme, surface: str = WORKSPACE_SURFACE):
        self.theme = theme
        background = theme.surface_colour(surface)
        self.frame = ttk.Frame(parent, style=f"{surface}.TFrame")
        self.frame.columnconfigure(0, weight=1)
        self.frame.rowconfigure(0, weight=1)
        self.canvas = tk.Canvas(self.frame, background=background, highlightthickness=0, borderwidth=0,
                                takefocus=0, yscrollincrement=theme.space(SCROLL_STEP_POINTS))
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.content = ttk.Frame(self.canvas, style=f"{surface}.TFrame")
        self._item = self.canvas.create_window(0, 0, window=self.content, anchor="nw")
        self.scrollbar = ttk.Scrollbar(self.frame, orient="vertical", command=self.canvas.yview,
                                       style="Workspace.Vertical.TScrollbar")
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        self.content.bind("<Configure>", lambda _event: self._update(), add="+")
        self.canvas.bind("<Configure>", self._resize, add="+")
        self.frame.bind("<Enter>", lambda _event: self._bind_wheel(True), add="+")
        self.frame.bind("<Leave>", lambda _event: self._bind_wheel(False), add="+")
        self.frame.bind("<Destroy>", lambda _event: self._bind_wheel(False), add="+")

    def grid(self, **options) -> None:
        self.frame.grid(**options)

    @property
    def is_scrollable(self) -> bool:
        return self.content.winfo_reqheight() > self.canvas.winfo_height() > 1

    def _resize(self, event) -> None:
        self.canvas.itemconfigure(self._item, width=event.width)
        self._update()

    def _update(self) -> None:
        width, height = self.content.winfo_reqwidth(), self.content.winfo_reqheight()
        self.canvas.configure(width=width, height=height, scrollregion=(0, 0, width, height))
        if self.is_scrollable:
            self.scrollbar.place(in_=self.canvas, relx=1.0, rely=0.0, relheight=1.0, anchor="ne")
        else:
            self.scrollbar.place_forget()
            self.canvas.yview_moveto(0.0)

    def _bind_wheel(self, is_inside: bool) -> None:
        events = ("<MouseWheel>", "<Button-4>", "<Button-5>")
        for event in events:
            if is_inside:
                self.canvas.bind_all(event, self._wheel)
            else:
                self.canvas.unbind_all(event)

    def _wheel(self, event):
        if not self.is_scrollable:
            return None
        delta = getattr(event, "delta", 0)
        upward = event.num == 4 or delta > 0
        self.canvas.yview_scroll(-1 if upward else 1, "units")
        return "break"

    def show(self, widget) -> None:
        """Scroll just enough that ``widget`` (inside the content) is fully visible, as when it takes focus."""
        if not self.is_scrollable:
            return
        self.canvas.update_idletasks()
        top = widget.winfo_rooty() - self.content.winfo_rooty()
        bottom = top + widget.winfo_height()
        total = max(1, self.content.winfo_height())
        visible_top = self.canvas.canvasy(0)
        visible_bottom = visible_top + self.canvas.winfo_height()
        # Tk snaps scrolling to whole steps, so round towards showing all of the widget.
        step = max(1, int(self.canvas.cget("yscrollincrement")))
        if top < visible_top:
            self.canvas.yview_moveto((top // step) * step / total)
        elif bottom > visible_bottom:
            self.canvas.yview_moveto(-(-(bottom - self.canvas.winfo_height()) // step) * step / total)


class _Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class _Size(ctypes.Structure):
    _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]


class _Rectangle(ctypes.Structure):
    _fields_ = [("origin", _Point), ("size", _Size)]


def macos_visible_frame() -> tuple[float, float] | None:
    """Top and height, in points from the top of the main screen, of the area the menu bar and Dock leave free.

    Read from ``[[NSScreen mainScreen] visibleFrame]`` through the Objective-C runtime, without PyObjC. Tk's
    screen height and maximum window size both include the menu bar and the Dock, so a window sized or centred
    from them can sit behind the Dock. Returns None when the frame cannot be read.
    """
    try:
        library = ctypes.util.find_library("objc")
        if not library:
            return None
        objc = ctypes.cdll.LoadLibrary(library)
        ctypes.cdll.LoadLibrary("/System/Library/Frameworks/AppKit.framework/AppKit")
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.objc_getClass.argtypes = [ctypes.c_char_p]
        objc.sel_registerName.restype = ctypes.c_void_p
        objc.sel_registerName.argtypes = [ctypes.c_char_p]
        send = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
            ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value)
        # A 32-byte struct comes back in registers on arm64 and through memory (the _stret entry) on x86-64.
        rectangle_entry = objc.objc_msgSend if platform.machine() == "arm64" else objc.objc_msgSend_stret
        send_rectangle = ctypes.CFUNCTYPE(_Rectangle, ctypes.c_void_p, ctypes.c_void_p)(
            ctypes.cast(rectangle_entry, ctypes.c_void_p).value)
        screen = send(objc.objc_getClass(b"NSScreen"), objc.sel_registerName(b"mainScreen"))
        if not screen:
            return None
        whole = send_rectangle(screen, objc.sel_registerName(b"frame"))
        visible = send_rectangle(screen, objc.sel_registerName(b"visibleFrame"))
    except Exception:  # noqa: BLE001 - no AppKit, or an unexpected runtime.
        return None
    if visible.size.height <= 0 or whole.size.height <= 0:
        return None
    # AppKit measures up from the bottom of the screen; Tk measures down from the top.
    top = whole.size.height - (visible.origin.y - whole.origin.y + visible.size.height)
    return max(0.0, top), visible.size.height


def usable_screen_area(window, scale: float = 1.0, *, system: str | None = None) -> tuple[int, int]:
    """Top and height of the screen area a window may take, with room left for its title bar.

    On macOS this is the area the menu bar and Dock leave free. Elsewhere it is Tk's maximum window size, at most
    a fraction of the screen so a taskbar stays clear.
    """
    system = system or platform.system()
    frame = int(round(WINDOW_FRAME_ALLOWANCE_POINTS * scale))
    visible = macos_visible_frame() if system == "Darwin" else None
    if visible is not None:
        top, height = visible
        return int(top), max(1, int(height) - frame)
    screen = window.winfo_screenheight()
    try:
        maximum = int(window.wm_maxsize()[1])
    except Exception:  # noqa: BLE001 - a Tk without a window manager answer.
        maximum = screen
    return 0, max(1, int(min(maximum, screen * USABLE_SCREEN_FRACTION)) - frame)


def usable_screen_height(window, scale: float = 1.0) -> int:
    """Height a window's content may take on its screen; see ``usable_screen_area``."""
    return usable_screen_area(window, scale)[1]


def fit_to_screen(window, scale: float = 1.0) -> None:
    """Size ``window`` to its content but never taller than its screen; a ``ScrollArea`` takes up the difference."""
    window.update_idletasks()
    width, height = window.winfo_reqwidth(), min(window.winfo_reqheight(), usable_screen_height(window, scale))
    minimum = min(height, int(round(MINIMUM_WINDOW_HEIGHT_POINTS * scale)))
    window.minsize(width, minimum)
    window.geometry(f"{width}x{height}")


def centre_window(window, scale: float = 1.0) -> None:
    """Centre ``window`` in the usable screen area at its requested size, or that area's height."""
    window.update_idletasks()
    top, usable = usable_screen_area(window, scale)
    width, height = window.winfo_reqwidth(), min(window.winfo_reqheight(), usable)
    x = max(0, (window.winfo_screenwidth() - width) // 2)
    y = top + max(0, (usable - height) // 2)
    window.geometry(f"+{x}+{y}")


def fit_minimum_size(window) -> None:
    """Never let the window shrink below the size its content asks for."""
    window.update_idletasks()
    window.minsize(window.winfo_reqwidth(), window.winfo_reqheight())
