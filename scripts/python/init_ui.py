"""Optional native setup selector; all installation stays in the engine runner.

Tk is loaded only for graphical setup. Closing the window cancels before any
setup mutation. Accepting it opens the progress window (``init_window``),
which runs the engine runner as a child that keeps the terminal for sudo.
"""
from __future__ import annotations

import argparse
import copy
import os
import platform
import sys
from dataclasses import dataclass

from init_denoiser import preset_supports, validate_denoiser_options


@dataclass(frozen=True)
class Persona:
    key: str
    label: str
    description: str
    preset: str
    preset_only: bool = False
    validate: bool = False
    smoke_app: bool = True


NATIVE_PRESET = "macos-clang-development" if platform.system() == "Darwin" else "linux-clang-development"

PERSONAS = (
    Persona("contributor", "Engine contributor",
            "Prepare all native presets for engine work. Build and test later.",
            NATIVE_PRESET),
    Persona("application", "Application developer",
            "Prepare one native preset for building applications with Ludus.",
            NATIVE_PRESET, preset_only=True, smoke_app=False),
    Persona("browser", "Browser developer",
            "Install the pinned Emscripten tools and configure one browser preset.",
            "web-emscripten-development", preset_only=True),
    Persona("validation", "Full validation",
            "Prepare native tools, then build, test, check, run sanitizers and test the SDK.",
            NATIVE_PRESET, validate=True),
)


# Width of wrapped text in the setup window, in logical pixels.
OPTION_WRAP_POINTS = 720
OPTION_COLUMN_WRAP_POINTS = 330
# Left edge of a checkbox's text relative to its indicator.
CHECKBOX_TEXT_INDENT_POINTS = 23
OPTION_LABELS = {
    "with_editor": "Build Ludus editor",
    "with_denoiser": "Include OIDN denoiser",
    "with_tests": "Include native test targets",
    "with_smoke_app": "Include sample applications",
    "with_web_probes": "Include browser feasibility probes",
    "with_shader_probe": "Include shader feasibility probe",
    "with_rad_debugger": "Build RAD Debugger",
    "preset_only": "Prepare only the selected preset",
    "no_system_install": "Use existing system packages",
    "run_tests": "Build and run native tests after setup",
    "validate": "Run full validation after setup",
    "skip_checks": "Skip formatting and static analysis",
    "skip_sanitizers": "Skip sanitizer build and tests",
    "skip_sdk": "Skip SDK consumer test",
    "ci": "Use CI validation policy",
}
OPTION_HINTS = {
    "with_editor": "Includes the 3D preview and live shaders unless you turn them off in Advanced setup.",
    "with_denoiser": "Denoises editor and game render graphs; adds the OIDN runtime to the SDK. "
                     "macOS Apple silicon only.",
    "with_smoke_app": "The smoke test and the native input demo.",
    "with_shader_probe": "Linux Vulkan only; needs separate pinned tools.",
    "with_rad_debugger": "Optional native debugger for Linux x64.",
    "no_system_install": "Skips automatic package installation.",
    "validate": "Build, test, check, run sanitizers and test the SDK.",
    "ci": "Warnings as errors; no local Git hooks.",
}
NESTED_OPTIONS = ("skip_checks", "skip_sanitizers", "skip_sdk")
OPTION_COLUMNS = (
    (("Targets", ("with_editor", "with_denoiser", "with_tests", "with_smoke_app", "with_web_probes", "with_shader_probe",
                  "with_rad_debugger")),),
    (("Setup scope", ("preset_only", "no_system_install")),
     ("After setup", ("run_tests", "validate", "skip_checks", "skip_sanitizers", "skip_sdk", "ci"))),
)


class GuiUnavailable(RuntimeError):
    """The optional Tk window cannot be opened on this host."""


def shader_probe_supported(preset: str) -> bool:
    return platform.system() == "Linux" and preset.startswith("linux-clang-")


def apply_defaults(args: argparse.Namespace) -> argparse.Namespace:
    result = copy.copy(args)
    persona = next(persona for persona in PERSONAS if persona.key == args.persona)
    result.preset = args.preset or persona.preset
    if not args.all_presets and not args.preset_only:
        result.preset_only = persona.preset_only or result.preset.startswith("web-emscripten-")
    result.validate = args.validate or persona.validate
    for name, default in (("with_tests", result.validate or args.ci or args.run_tests),
                          ("with_smoke_app", persona.smoke_app),
                          ("with_web_probes", False), ("with_shader_probe", False),
                          ("with_denoiser", preset_supports(result.preset))):
        if getattr(result, name) is None:
            setattr(result, name, default)
    return result


def validate_options(args: argparse.Namespace, engine) -> None:
    if args.with_shader_probe and not shader_probe_supported(args.preset):
        raise engine.EngineError(
            "The shader feasibility probe requires the Linux Vulkan backend; macOS uses Metal. "
            "Rerun ./init.sh --no-shader-probe. This optional probe is not required for "
            "Metal shader compilation, world_demo or the Cornell box sample.")
    if args.all_presets and args.preset_only:
        raise engine.EngineError("--all-presets and --preset-only describe conflicting initialization scopes")
    if (args.validate or args.ci or args.run_tests) and not args.with_tests:
        raise engine.EngineError("Running tests or full validation requires --with-tests; remove --no-tests")
    if args.preset.startswith("web-emscripten-"):
        if args.with_shader_probe:
            raise engine.EngineError("The shader probe is a native-only target")
        if args.with_tests or args.run_tests:
            raise engine.EngineError("Native test targets are unsupported in browser presets; use browser probes instead")
        if args.with_rad_debugger:
            raise engine.EngineError("RAD is native tooling; run ./scripts/setup-rad-debugger separately from browser init")
        from web_build import PRESETS
        if args.preset not in PRESETS:
            raise engine.EngineError("Unknown browser preset: " + args.preset)
        if args.all_presets or args.validate or args.ci:
            raise engine.EngineError("Use web init --preset-only, then web build/test/check separately")
    else:
        if args.with_web_probes:
            raise engine.EngineError("Browser probes require a web-emscripten preset")
        engine.ensure_known_preset(args.preset)
    from init_editor import validate_editor_options
    validate_editor_options(args, engine)
    validate_denoiser_options(args, engine)


# Options passed to the child runner verbatim; every other init option is a
# --with-x/--no-x pair whose default is resolved by apply_defaults.
STORE_TRUE_OPTIONS = (
    ("no_system_install", "--no-system-install"),
    ("with_rad_debugger", "--with-rad-debugger"),
    ("skip_engine_refresh", "--no-engine-refresh"),
    ("run_tests", "--run-tests"),
    ("all_presets", "--all-presets"),
    ("preset_only", "--preset-only"),
    ("validate", "--validate"),
    ("skip_checks", "--skip-checks"),
    ("skip_sanitizers", "--skip-sanitizers"),
    ("skip_sdk", "--skip-sdk"),
    ("locked", "--locked"),
    ("ci", "--ci"),
)
PAIRED_OPTIONS = (
    ("with_editor", "--with-editor", "--no-editor"),
    ("with_editor_preview", "--with-editor-preview", "--no-editor-preview"),
    ("with_denoiser", "--with-denoiser", "--no-denoiser"),
    ("with_tests", "--with-tests", "--no-tests"),
    ("with_smoke_app", "--with-smoke-app", "--no-smoke-app"),
    ("with_web_probes", "--with-web-probes", "--no-web-probes"),
    ("with_shader_probe", "--with-shader-probe", "--no-shader-probe"),
)


def selection_to_argv(args: argparse.Namespace) -> list[str]:
    """Spell a resolved selection as explicit ``engine.py init --cli`` options."""
    argv = [args.preset, "--cli", "--persona", args.persona]
    for name, option in STORE_TRUE_OPTIONS:
        if getattr(args, name, False):
            argv.append(option)
    for name, enabled, disabled in PAIRED_OPTIONS:
        argv.append(enabled if getattr(args, name) else disabled)
    return argv


def wants_gui(args: argparse.Namespace) -> bool:
    if args.cli:
        return False
    if args.gui:
        return True
    if args.ci or os.environ.get("CI") or not sys.stdin.isatty() or not sys.stdout.isatty():
        return False
    return sys.platform != "linux" or bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


def prepare_init(args: argparse.Namespace, engine) -> argparse.Namespace | None:
    args = apply_defaults(args)
    validate_options(args, engine)
    if wants_gui(args):
        try:
            args = select_options(args, engine)
            if args is not None:
                args.show_progress_window = True
        except GuiUnavailable as error:
            if args.gui:
                raise engine.EngineError(str(error)) from error
            print(f"Graphical setup unavailable: {error}\nContinuing with CLI setup.", file=sys.stderr)
    if args is not None:
        validate_options(args, engine)
    return args


def select_options(args: argparse.Namespace, engine) -> argparse.Namespace | None:
    try:
        import tkinter as tk
        from tkinter import messagebox, ttk
    except ImportError as error:
        raise GuiUnavailable(
            "Tkinter is missing for this Python. Install python3-tk on Ubuntu, or "
            f"python-tk@{sys.version_info.major}.{sys.version_info.minor} with Homebrew on macOS; "
            "alternatively use --cli."
        ) from error
    # Thanks to the Python Software Foundation, "IDLE and tkinter with Tcl/Tk
    # on macOS", for documenting Apple's Tk 8.5 rendering/crash problems:
    # https://www.python.org/download/mac/tcltk/
    # Check before Tk() so an unsupported runtime cannot open a blank window.
    if sys.platform == "darwin" and tk.TkVersion < 8.6:
        raise GuiUnavailable(
            "macOS graphical setup requires Tk 8.6 or later. Apple's system Tk is unsupported. "
            "Use a modern Python with Tk from Homebrew or python.org, or use --cli.")
    import init_theme
    try:
        window = init_theme.create_root(tk)
    except tk.TclError as error:
        raise GuiUnavailable("Cannot open a desktop display. Use --cli, or check your display connection.") from error

    selection = None
    try:
        theme = init_theme.Theme(window, ttk)
        theme.apply_window_icon(window)
        space = theme.space
        wrap = space(OPTION_WRAP_POINTS)
        window.columnconfigure(0, weight=1)
        window.rowconfigure(1, weight=1)
        header = build_header(ttk, window, theme, "Set up Ludus", "Choose the tools and workflow you need.")
        header.grid(row=0, column=0, sticky="ew")

        panel = ttk.Frame(window, style="Workspace.TFrame",
                          padding=(space(init_theme.SPACE_EXTRA_LARGE), space(init_theme.SPACE_LARGE)))
        panel.grid(row=1, column=0, sticky="nsew")
        panel.columnconfigure(0, weight=1)

        persona = tk.StringVar(value=next(item.label for item in PERSONAS if item.key == args.persona))
        preset = tk.StringVar(value=args.preset)
        description = tk.StringVar()
        summary = tk.StringVar()
        flags = {name: tk.BooleanVar(value=getattr(args, name)) for name in (
            "preset_only", "no_system_install", "with_rad_debugger", "with_editor", "with_editor_preview", "with_denoiser",
            "with_tests", "run_tests", "with_smoke_app", "with_web_probes", "with_shader_probe", "validate",
            "skip_checks", "skip_sanitizers", "skip_sdk", "ci",
        )}

        card_padding = (space(init_theme.SPACE_LARGE), space(init_theme.SPACE_MEDIUM))
        workflow_card = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        workflow_card.grid(row=0, column=0, sticky="ew")
        workflow_card.columnconfigure(1, weight=1, uniform="choice")
        workflow_card.columnconfigure(3, weight=1, uniform="choice")
        label_gap = space(init_theme.SPACE_MEDIUM)
        ttk.Label(workflow_card, text="Workflow", style="Card.Strong.TLabel").grid(
            row=0, column=0, sticky="w", padx=(0, label_gap))
        workflow = ttk.Combobox(workflow_card, textvariable=persona, state="readonly",
                                values=[item.label for item in PERSONAS], font=theme.fonts.body)
        workflow.grid(row=0, column=1, sticky="ew")
        from web_build import PRESETS
        ttk.Label(workflow_card, text="Build preset", style="Card.Strong.TLabel").grid(
            row=0, column=2, sticky="w", padx=(space(init_theme.SPACE_EXTRA_LARGE), label_gap))
        presets = ttk.Combobox(workflow_card, textvariable=preset, state="readonly",
                               values=[*engine.HOST_PRESETS, *PRESETS], font=theme.fonts.body)
        presets.grid(row=0, column=3, sticky="ew")
        ttk.Label(workflow_card, textvariable=description, style="Card.Secondary.TLabel", wraplength=wrap).grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(space(init_theme.SPACE_SMALL), 0))

        options_card = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        options_card.grid(row=1, column=0, sticky="ew", pady=(space(init_theme.SPACE_MEDIUM), 0))
        options_card.columnconfigure(0, weight=1, uniform="options")
        options_card.columnconfigure(1, weight=1, uniform="options")
        controls = {}
        column_wrap = space(OPTION_COLUMN_WRAP_POINTS)
        hint_indent = space(CHECKBOX_TEXT_INDENT_POINTS)
        for column, sections in enumerate(OPTION_COLUMNS):
            column_frame = ttk.Frame(options_card, style="Panel.TFrame")
            column_frame.grid(row=0, column=column, sticky="nsew",
                              padx=(0, space(init_theme.SPACE_EXTRA_LARGE)) if column == 0 else 0)
            row = 0
            for section_index, (title, names) in enumerate(sections):
                ttk.Label(column_frame, text=title, style="Card.Heading.TLabel").grid(
                    row=row, column=0, sticky="w",
                    pady=(space(init_theme.SPACE_LARGE) if section_index else 0, space(init_theme.SPACE_EXTRA_SMALL)))
                row += 1
                for name in names:
                    indent = hint_indent if name in NESTED_OPTIONS else 0
                    controls[name] = ttk.Checkbutton(column_frame, text=OPTION_LABELS[name], variable=flags[name],
                                                     command=lambda: refresh(), style="Card.TCheckbutton")
                    controls[name].grid(row=row, column=0, sticky="w", padx=(indent, 0))
                    row += 1
                    if name in OPTION_HINTS:
                        ttk.Label(column_frame, text=OPTION_HINTS[name], style="Card.Secondary.TLabel",
                                  wraplength=column_wrap - hint_indent - indent).grid(
                            row=row, column=0, sticky="w", padx=(hint_indent + indent, 0),
                            pady=(0, space(init_theme.SPACE_EXTRA_SMALL)))
                        row += 1

        advanced_toggle = ttk.Button(panel, text="Advanced setup", style="Workspace.Link.TButton",
                                     command=lambda: toggle_advanced())
        advanced_toggle.grid(row=2, column=0, sticky="w", pady=(space(init_theme.SPACE_SMALL), 0))
        advanced = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        advanced.grid(row=3, column=0, sticky="ew", pady=(space(init_theme.SPACE_EXTRA_SMALL), 0))
        ttk.Label(advanced, text="Editor", style="Card.Heading.TLabel").grid(row=0, column=0, sticky="w")
        controls["with_editor_preview"] = ttk.Checkbutton(
            advanced, text="Include 3D preview and live shaders (disable for a lightweight editor)",
            variable=flags["with_editor_preview"], command=lambda: refresh(), style="Card.TCheckbutton")
        controls["with_editor_preview"].grid(row=1, column=0, sticky="w", pady=(space(init_theme.SPACE_EXTRA_SMALL), 0))
        advanced.grid_remove()

        summary_card = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        summary_card.grid(row=4, column=0, sticky="ew", pady=(space(init_theme.SPACE_MEDIUM), 0))
        summary_card.columnconfigure(0, weight=1)
        ttk.Label(summary_card, textvariable=summary, style="Card.TLabel", wraplength=wrap, justify="left").grid(
            row=0, column=0, sticky="w", pady=(0, space(init_theme.SPACE_SMALL)))
        ttk.Label(summary_card, text=f"Repository  {engine.repo_root()}", style="Card.Mono.TLabel",
                  wraplength=wrap).grid(row=1, column=0, sticky="w")

        def current_args():
            result = copy.copy(args)
            result.persona = next(item.key for item in PERSONAS if item.label == persona.get())
            result.preset = preset.get()
            for name, value in flags.items():
                setattr(result, name, value.get())
            result.all_presets = not result.preset_only
            return result

        def refresh():
            browser = preset.get().startswith("web-")
            if browser:
                flags["preset_only"].set(True)
                for name in ("with_rad_debugger", "with_editor", "with_tests", "run_tests", "with_shader_probe", "validate", "ci"):
                    flags[name].set(False)
            from init_editor import SUPPORTED_PRESETS, supported_host
            family = "macos-" if platform.system() == "Darwin" else "linux-"
            editor_supported = supported_host() and preset.get() in SUPPORTED_PRESETS and preset.get().startswith(family)
            if not editor_supported:
                flags["with_editor"].set(False)
            controls["with_editor"].configure(state="normal" if editor_supported else "disabled")
            controls["with_editor_preview"].configure(
                state="normal" if editor_supported and flags["with_editor"].get() else "disabled")
            # Unavailable presets turn the denoiser off; returning to a supported one restores the default.
            denoiser_supported = preset_supports(preset.get())
            if denoiser_supported and str(controls["with_denoiser"].cget("state")) == "disabled":
                flags["with_denoiser"].set(True)
            if not denoiser_supported:
                flags["with_denoiser"].set(False)
            controls["with_denoiser"].configure(state="normal" if denoiser_supported else "disabled")
            validating = flags["validate"].get() or flags["ci"].get()
            if validating or flags["run_tests"].get():
                flags["with_tests"].set(True)
            if not browser:
                flags["with_web_probes"].set(False)
            probe_supported = shader_probe_supported(preset.get())
            if not probe_supported:
                flags["with_shader_probe"].set(False)
            controls["with_shader_probe"].configure(state="normal" if probe_supported else "disabled")
            controls["with_web_probes"].configure(state="normal" if browser else "disabled")
            controls["with_tests"].configure(state="disabled" if browser or validating or flags["run_tests"].get() else "normal")
            controls["run_tests"].configure(state="disabled" if browser or validating else "normal")
            for name in ("preset_only", "with_rad_debugger", "validate", "ci"):
                controls[name].configure(state="disabled" if browser else "normal")
            linux_host = platform.system() == "Linux" and platform.machine().lower() in ("x86_64", "amd64")
            if not linux_host:
                flags["with_rad_debugger"].set(False)
                controls["with_rad_debugger"].configure(state="disabled")
            for name in ("skip_checks", "skip_sanitizers", "skip_sdk"):
                controls[name].configure(state="normal" if validating else "disabled")
            # Native validation prepares every preset regardless of --preset-only.
            scope = "one browser preset" if browser else (
                "one native preset" if flags["preset_only"].get() and not validating else "all native presets")
            action = "Setup, engine builds and validation" if validating else "Tool and dependency setup"
            if flags["with_editor"].get() and not validating:
                action = "Tool and dependency setup plus the optional editor build"
            if flags["run_tests"].get() and not validating:
                action = "Setup plus test builds and execution"
            included = [label for name, label in (("with_tests", "tests"), ("with_smoke_app", "samples"),
                        ("with_web_probes", "browser probes"), ("with_shader_probe", "shader probe"), ("with_editor", "editor"),
                        ("with_denoiser", "OIDN denoiser")) if flags[name].get()]
            if flags["with_editor"].get():
                included.append("3D preview + live shaders" if flags["with_editor_preview"].get() else "document-only editor")
            summary.set(f"{action} for {scope}.\nOptional targets: {', '.join(included) or 'none'}.\n"
                        "Progress appears in a window after you choose Initialize; "
                        "any sudo password prompt appears in the launching terminal.")

        def change_workflow(_event=None):
            chosen = next(item for item in PERSONAS if item.label == persona.get())
            description.set(chosen.description)
            preset.set(chosen.preset)
            flags["preset_only"].set(chosen.preset_only)
            flags["validate"].set(chosen.validate)
            flags["ci"].set(False)
            flags["with_tests"].set(chosen.validate)
            flags["run_tests"].set(False)
            flags["with_smoke_app"].set(chosen.smoke_app)
            flags["with_web_probes"].set(False)
            flags["with_shader_probe"].set(False)
            refresh()

        def toggle_advanced():
            if advanced.winfo_ismapped():
                advanced.grid_remove()
            else:
                advanced.grid()
            init_theme.fit_minimum_size(window)

        workflow.bind("<<ComboboxSelected>>", change_workflow)
        presets.bind("<<ComboboxSelected>>", lambda _event: refresh())
        description.set(next(item.description for item in PERSONAS if item.key == args.persona))

        def accept(_event=None):
            nonlocal selection
            candidate = current_args()
            try:
                validate_options(candidate, engine)
            except engine.EngineError as error:
                messagebox.showerror("Check setup options", str(error), parent=window)
                return
            # Open the progress window where the selector was.
            candidate.window_geometry = f"+{window.winfo_x()}+{window.winfo_y()}"
            selection = candidate
            window.quit()

        footer = build_footer(ttk, window, theme, "Enter to initialize · Esc to cancel")
        footer.grid(row=2, column=0, sticky="ew")
        buttons = ttk.Frame(footer, style="Workspace.TFrame")
        buttons.grid(row=1, column=1, sticky="e", padx=footer.buttons_padding)
        ttk.Button(buttons, text="Cancel", command=window.quit).pack(side="left", padx=(0, space(init_theme.SPACE_SMALL)))
        initialize = ttk.Button(buttons, text="Initialize", style="Primary.TButton", command=accept)
        initialize.pack(side="left")
        window.protocol("WM_DELETE_WINDOW", window.quit)
        window.bind("<Escape>", lambda _event: window.quit())
        window.bind("<Return>", accept)
        window.bind("<KP_Enter>", accept)
        refresh()
        init_theme.fit_minimum_size(window)
        init_theme.centre_window(window)
        initialize.focus_set()
        window.mainloop()
    finally:
        # Also release Tk when a callback/building the form fails. The root
        # is hidden rather than destroyed: the progress window reuses it, and
        # on macOS its interpreter must outlive every later Apple event.
        init_theme.release_root(window)
    return selection


def build_header(ttk, window, theme, title: str, subtitle: str, *, title_variable=None, subtitle_variable=None):
    """The branded band at the top of both setup windows: icon, title and subtitle."""
    import init_theme
    space = theme.space
    header = ttk.Frame(window, style="Header.TFrame")
    content = ttk.Frame(header, style="Header.TFrame",
                        padding=(space(init_theme.SPACE_EXTRA_LARGE), space(init_theme.SPACE_LARGE)))
    content.grid(row=0, column=0, sticky="ew")
    header.columnconfigure(0, weight=1)
    content.columnconfigure(1, weight=1)
    image = theme.header_image()
    if image is not None:
        ttk.Label(content, image=image, style="Header.TLabel").grid(
            row=0, column=0, rowspan=2, sticky="w", padx=(0, space(init_theme.SPACE_LARGE)))
    title_options = {"textvariable": title_variable} if title_variable is not None else {"text": title}
    subtitle_options = {"textvariable": subtitle_variable} if subtitle_variable is not None else {"text": subtitle}
    ttk.Label(content, style="Header.Title.TLabel", **title_options).grid(row=0, column=1, sticky="sw")
    header.subtitle = ttk.Label(content, style="Header.Secondary.TLabel", wraplength=space(OPTION_WRAP_POINTS),
                                **subtitle_options)
    header.subtitle.grid(row=1, column=1, sticky="nw", pady=(space(init_theme.SPACE_EXTRA_SMALL) // 2, 0))
    ttk.Frame(header, style="Rule.TFrame", height=1).grid(row=1, column=0, sticky="ew")
    return header


def build_footer(ttk, window, theme, hint: str):
    """A ruled footer with a keyboard hint on the left; callers add buttons at column 1."""
    import init_theme
    space = theme.space
    footer = ttk.Frame(window, style="Workspace.TFrame")
    footer.columnconfigure(0, weight=1)
    ttk.Frame(footer, style="Rule.TFrame", height=1).grid(row=0, column=0, columnspan=2, sticky="ew")
    footer.hint = ttk.Label(footer, text=hint, style="Workspace.Secondary.TLabel")
    footer.hint.grid(row=1, column=0, sticky="w", padx=(space(init_theme.SPACE_EXTRA_LARGE), 0),
                     pady=space(init_theme.SPACE_LARGE))
    # Callers place their button row at column 1 with this horizontal padding.
    footer.buttons_padding = (0, space(init_theme.SPACE_EXTRA_LARGE))
    return footer
