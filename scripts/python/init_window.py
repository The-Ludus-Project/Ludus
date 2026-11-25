"""Graphical progress for ./init.sh after the user chooses Initialize.

The engine runner executes as a child process (``engine.py init --cli`` with
the chosen options) so the window stays responsive: a reader thread copies the
child's output to the launching terminal, to ``out/init/setup.log`` and to a
queue that the Tk event loop drains. Steps come from the progress markers
documented in ``init_progress``. The child keeps the terminal as its standard
input, so sudo still prompts there; the window says when it is waiting.
After a setup that built the editor succeeds, **Run editor** starts it through
the trusted ``editor_launch`` launcher in its own session.
"""
from __future__ import annotations

import gc
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path
from typing import Callable, Sequence

import init_failure
import init_progress
import init_theme


POLL_MILLISECONDS = 50
CLOCK_MILLISECONDS = 250
SPINNER_MILLISECONDS = 80
TERMINATE_GRACE_MILLISECONDS = 5000
READ_CHUNK_BYTES = 65536
MAX_LINES_PER_POLL = 400
MAX_LOG_LINES = 5000
MAX_DETAIL_CHARACTERS = 84
# Width of wrapped text, in logical pixels.
WRAP_POINTS = 640
# Wide enough for every state's footer, so the window keeps its width when setup ends.
WINDOW_WIDTH_POINTS = 720
LOG_HEIGHT_LINES = 10
# After a failure the log grows to show the failing tool's last lines at once,
# while the window still fits a 13-inch laptop screen.
FAILURE_LOG_HEIGHT_LINES = 15
# The rest of the runner's closing text stays in the output log, so the window
# still fits a laptop screen with the log open.
MAX_SUMMARY_LINES = 3
RUNNING_HINT = "Esc to cancel setup"
SUCCESS_HINT = "Enter to close"
SUCCESS_EDITOR_HINT = "Enter to run the editor · Esc to close"
FAILURE_HINT = "Enter to retry · Esc to close"
COPY_OUTPUT_LABEL = "Copy output"
COPY_REPORT_LABEL = "Copy error report"
DEFAULT_FAILURE_ADVICE = "Fix the reported problem, then choose Retry or run ./init.sh again."
OPEN_LOG_FAILED_NOTE = "Could not open a viewer for the log. Its path is on the clipboard."
CANCELLED_EXIT_CODE = 130
RUN_EDITOR_LABEL = "Run editor"
STARTING_EDITOR_STATUS = "Starting the editor…"
# The editor runs on after setup exits, so its output goes to a log instead of the terminal.
EDITOR_LOG_RELATIVE_PATH = Path("out/init/editor.log")
# Run editor waits this long for the launcher to fail (for example, no built editor) before closing setup.
EDITOR_START_CHECK_MILLISECONDS = 1500
MAX_EDITOR_FAILURE_LINES = 4


def child_command(args, root: Path) -> list[str]:
    from init_ui import selection_to_argv
    return [sys.executable, str(root / "scripts/python/engine.py"), "init", *selection_to_argv(args)]


def editor_command(args, root: Path, engine) -> list[str] | None:
    """The trusted ``scripts/editor`` launch for a setup that builds the editor, or None.

    Setup that only configures the editor (``--configure-editor-only``) or
    leaves it out has nothing to launch.
    """
    if not getattr(args, "with_editor", False) or getattr(args, "configure_editor_only", False):
        return None
    return [str(engine.venv_python(root)), str(root / "scripts/python/editor_launch.py"), "--preset", args.preset]


def child_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment[init_progress.PROGRESS_ENVIRONMENT] = "1"
    environment[init_progress.GUI_ENVIRONMENT] = "1"
    environment["PYTHONUNBUFFERED"] = "1"
    return environment


def descendant_process_ids(parent: int) -> list[int]:
    """Return every descendant of ``parent``, deepest first (POSIX ``ps``)."""
    try:
        listing = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "ppid="], capture_output=True, text=True,
                                 check=False).stdout
    except OSError:
        return []
    children: dict[int, list[int]] = {}
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].isdigit() and fields[1].isdigit():
            children.setdefault(int(fields[1]), []).append(int(fields[0]))
    ordered: list[int] = []
    pending = [parent]
    while pending:
        current = pending.pop()
        for child in children.get(current, []):
            ordered.append(child)
            pending.append(child)
    return list(reversed(ordered))


def signal_process_tree(process: subprocess.Popen, *, force: bool) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, check=False)
        return
    chosen = signal.SIGKILL if force else signal.SIGTERM
    # Signal grandchildren (CMake, Ninja, Conan, sudo) too: the engine runner
    # waits on them, and SIGTERM to it alone would orphan a running build.
    for process_id in [*descendant_process_ids(process.pid), process.pid]:
        try:
            os.kill(process_id, chosen)
        except OSError:
            pass


class OutputPump:
    """Copy a child's merged output to the terminal, a log file and a queue."""

    def __init__(self, process: subprocess.Popen, log_path: Path | None, echo=None):
        self.process = process
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.log_path = log_path
        self.echo = echo
        self.thread = threading.Thread(target=self._run, name="ludus-init-output", daemon=True)
        self.log_lock = threading.RLock()
        self.log = None

    def start(self) -> None:
        self.thread.start()

    def write_note(self, line: str) -> None:
        """Add one line of setup's own to the log, between the child's output chunks; any thread."""
        try:
            with self.log_lock:
                self._write_note(line)
        except (OSError, ValueError):
            pass

    def _write_note(self, line: str) -> None:
        if self.log is not None:
            self.log.write((line + "\n").encode("utf-8"))
            self.log.flush()

    def _run(self) -> None:
        log = None
        if self.log_path is not None:
            try:
                self.log_path.parent.mkdir(parents=True, exist_ok=True)
                log = self.log_path.open("wb")
            except OSError:
                log = None
        if log is not None:
            # Where the setup windows went comes first, so a window off screen can be explained from the log.
            with self.log_lock:
                self.log = log
                for line in init_theme.diagnostic_lines():
                    self.write_note(line)
            init_theme.set_diagnostic_sink(self.write_note)
        pending = b""
        stream = self.process.stdout
        try:
            while True:
                chunk = os.read(stream.fileno(), READ_CHUNK_BYTES)
                if not chunk:
                    break
                for sink in (self.echo, log):
                    if sink is not None:
                        try:
                            with self.log_lock:
                                sink.write(chunk)
                                sink.flush()
                        except (OSError, ValueError):
                            pass
                pending += chunk
                *complete, pending = pending.split(b"\n")
                for line in complete:
                    self.lines.put(line.decode("utf-8", errors="replace"))
        except OSError:
            pass
        finally:
            if pending:
                self.lines.put(pending.decode("utf-8", errors="replace"))
            if log is not None:
                init_theme.set_diagnostic_sink(None)
                with self.log_lock:
                    self.log = None
                    log.close()
            self.lines.put(None)


class SetupWindow:
    """The running, succeeded, failed and cancelled states of one setup run."""

    def __init__(self, tk, ttk, *, steps: Sequence[init_progress.Step], command: Sequence[str],
                 title: str, log_path: Path | None, cwd: Path, environment: dict[str, str],
                 geometry: str | None = None, echo=None, messagebox=None, clock: Callable[[], float] = time.monotonic,
                 editor_command: Sequence[str] | None = None, editor_log_path: Path | None = None):
        self.tk = tk
        self.ttk = ttk
        self.messagebox = messagebox
        self.command = list(command)
        self.cwd = cwd
        self.environment = environment
        self.log_path = log_path
        self.echo = echo
        self.clock = clock
        self.steps = list(steps)
        self.progress = init_progress.SetupProgress.from_plan(self.steps)
        self.process: subprocess.Popen | None = None
        self.pump: OutputPump | None = None
        self.started_at = clock()
        self.finished_at: float | None = None
        self.is_cancelling = False
        self.should_close_when_done = False
        self.exit_code: int | None = None
        self.log_lines = 0
        # Recent output, searched for a recognised cause when setup fails.
        self.recent_lines: deque[str] = deque(maxlen=init_failure.HINT_SCAN_LINES)
        self.failure_hint: init_failure.FailureHint | None = None
        self.step_rows: list[tuple] = []
        self.timers: dict[str, str] = {}
        # Run editor, offered once setup that built the editor succeeds.
        self.editor_command = list(editor_command) if editor_command else None
        self.editor_log_path = editor_log_path
        self.editor_process: subprocess.Popen | None = None
        self.is_editor_started = False

        self.window = init_theme.create_root(tk)
        self.theme = init_theme.Theme(self.window, ttk)
        self.theme.apply_window_icon(self.window)
        self._build(title)
        init_theme.fit_to_screen(self.window, self.theme.scale)
        self.fitted_height = self.scroll.content.winfo_reqheight()
        if geometry:
            self.window.geometry(geometry)
        else:
            init_theme.centre_window(self.window, self.theme.scale)
        # The option window's place is a hint: a taller progress window there is pulled back on screen.
        init_theme.check_window_fit_later(self.window, "progress", self.theme.scale)
        self.window.protocol("WM_DELETE_WINDOW", self.request_close)
        self.window.bind("<Escape>", lambda _event: self.request_close())
        self.window.bind("<Return>", lambda _event: self.activate_default())
        self.window.bind("<KP_Enter>", lambda _event: self.activate_default())

    # -- layout --------------------------------------------------------------

    def _build(self, title: str) -> None:
        tk, ttk, theme = self.tk, self.ttk, self.theme
        palette, space = theme.palette, theme.space
        wrap = space(WRAP_POINTS)
        self.window.columnconfigure(0, weight=1, minsize=space(WINDOW_WIDTH_POINTS))
        self.window.rowconfigure(1, weight=1)
        from init_ui import build_footer, build_header
        self.heading = tk.StringVar(value="Setting up Ludus")
        self.subtitle = tk.StringVar(value=title)
        build_header(ttk, self.window, theme, "", "", title_variable=self.heading,
                     subtitle_variable=self.subtitle).grid(row=0, column=0, sticky="ew")

        # The status, steps, outcome and log scroll on a short screen, so the Copy output and Close footer stays put.
        self.scroll = init_theme.ScrollArea(tk, ttk, self.window, theme)
        self.scroll.grid(row=1, column=0, sticky="nsew")
        self.scroll.content.columnconfigure(0, weight=1)
        self.scroll.content.rowconfigure(0, weight=1)
        panel = ttk.Frame(self.scroll.content, style="Workspace.TFrame",
                          padding=(space(init_theme.SPACE_EXTRA_LARGE), space(init_theme.SPACE_LARGE)))
        panel.grid(row=0, column=0, sticky="nsew")
        panel.columnconfigure(0, weight=1)
        self.panel = panel
        card_padding = (space(init_theme.SPACE_LARGE), space(init_theme.SPACE_MEDIUM))

        status_card = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        status_card.grid(row=0, column=0, sticky="ew")
        status_card.columnconfigure(0, weight=1)
        self.status = tk.StringVar(value="Starting setup…")
        self.status_label = ttk.Label(status_card, textvariable=self.status, style="Card.Strong.TLabel",
                                      wraplength=wrap, justify="left")
        self.status_label.grid(row=0, column=0, sticky="w")
        self.elapsed = tk.StringVar(value="00:00")
        ttk.Label(status_card, textvariable=self.elapsed, style="Card.Mono.TLabel").grid(row=0, column=1, sticky="e")
        self.bar = ttk.Progressbar(status_card, mode="indeterminate", maximum=1.0,
                                   style="Ludus.Horizontal.TProgressbar")
        self.bar.grid(row=1, column=0, columnspan=2, sticky="ew",
                      pady=(space(init_theme.SPACE_SMALL), space(init_theme.SPACE_SMALL)))
        self.bar.start()
        self.detail = tk.StringVar(value="")
        self.activity = tk.StringVar(value="")
        self.detail_label = ttk.Label(status_card, textvariable=self.detail, style="Card.Secondary.TLabel")
        self.detail_label.grid(row=2, column=0, columnspan=2, sticky="w")
        self.activity_label = ttk.Label(status_card, textvariable=self.activity, style="Card.Mono.TLabel")
        self.activity_label.grid(row=3, column=0, columnspan=2, sticky="w")

        self.note_banner = theme.banner(tk, panel, init_theme.WARNING)
        self.note_banner.set_wrap(wrap)
        self.note = self.note_banner.text
        self.note_label = self.note_banner.frame
        self.note_label.grid(row=1, column=0, sticky="ew", pady=(space(init_theme.SPACE_MEDIUM), 0))
        self.note_label.grid_remove()

        steps = ttk.Frame(panel, style="Card.TFrame", padding=card_padding)
        steps.grid(row=2, column=0, sticky="ew", pady=(space(init_theme.SPACE_MEDIUM), 0))
        steps.columnconfigure(1, weight=1)
        ttk.Label(steps, text="Steps", style="Card.Heading.TLabel").grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, space(init_theme.SPACE_EXTRA_SMALL)))
        self.steps_frame = steps
        self._rebuild_steps()

        self.outcome_banner = theme.banner(tk, panel, init_theme.SUCCESS)
        self.outcome_banner.set_wrap(wrap)
        self.outcome = self.outcome_banner.text
        self.outcome_label = self.outcome_banner.frame
        self.outcome_label.grid(row=3, column=0, sticky="ew", pady=(space(init_theme.SPACE_MEDIUM), 0))
        self.outcome_label.grid_remove()

        log_bar = ttk.Frame(panel, style="Workspace.TFrame")
        log_bar.grid(row=4, column=0, sticky="ew", pady=(space(init_theme.SPACE_SMALL), 0))
        log_bar.columnconfigure(1, weight=1)
        self.details_button = ttk.Button(log_bar, text="Show output", style="Workspace.Link.TButton",
                                         command=self.toggle_log)
        self.details_button.grid(row=0, column=0, sticky="w")
        if self.log_path is not None:
            ttk.Label(log_bar, text=f"Full log  {self.log_path}", style="Workspace.Mono.TLabel").grid(
                row=0, column=1, sticky="e", padx=(space(init_theme.SPACE_MEDIUM), 0))
            self.open_log_button = ttk.Button(log_bar, text="Open full log", style="Workspace.Link.TButton",
                                              command=self.open_log)
            self.open_log_button.grid(row=0, column=2, sticky="e", padx=(space(init_theme.SPACE_SMALL), 0))
            self.copy_path_button = ttk.Button(log_bar, text="Copy path", style="Workspace.Link.TButton",
                                               command=self.copy_log_path)
            self.copy_path_button.grid(row=0, column=3, sticky="e", padx=(space(init_theme.SPACE_SMALL), 0))

        self.log_frame = ttk.Frame(panel, style="Workspace.TFrame")
        self.log_frame.grid(row=5, column=0, sticky="nsew", pady=(space(init_theme.SPACE_EXTRA_SMALL), 0))
        self.log_frame.columnconfigure(0, weight=1)
        self.log_frame.rowconfigure(0, weight=1)
        # A one-character request lets the window, not the log, decide the width.
        self.log = tk.Text(self.log_frame, height=LOG_HEIGHT_LINES, width=1, wrap="none", font=theme.fonts.mono,
                           state="disabled", background=palette.log_background, foreground=palette.log_text,
                           insertbackground=palette.log_text, selectbackground=palette.selected,
                           selectforeground=palette.selected_text, borderwidth=0, highlightthickness=0,
                           padx=space(init_theme.SPACE_SMALL), pady=space(init_theme.SPACE_SMALL))
        scroll = ttk.Scrollbar(self.log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll.grid(row=0, column=1, sticky="ns")
        self.log_frame.grid_remove()
        panel.rowconfigure(5, weight=1)

        self.footer = build_footer(ttk, self.window, theme, RUNNING_HINT)
        self.footer.grid(row=2, column=0, sticky="ew")
        buttons = ttk.Frame(self.footer, style="Workspace.TFrame")
        buttons.grid(row=1, column=1, sticky="e", padx=self.footer.buttons_padding)
        gap = (0, space(init_theme.SPACE_SMALL))
        self.copy_button = ttk.Button(buttons, text=COPY_OUTPUT_LABEL, command=self.copy_log)
        self.copy_button.pack(side="left", padx=gap)
        # Initialize stays disabled while setup runs; after a failure or a
        # cancellation it reruns the same selection.
        self.initialize_button = ttk.Button(buttons, text="Initialize", state="disabled", command=self.retry)
        self.initialize_button.pack(side="left", padx=gap)
        # Shown only after a successful setup that built the editor.
        self.editor_button = ttk.Button(buttons, text=RUN_EDITOR_LABEL, style="Primary.TButton",
                                        command=self.run_editor)
        self.cancel_button = ttk.Button(buttons, text="Cancel", command=self.request_cancel)
        self.cancel_button.pack(side="left")

    def _rebuild_steps(self) -> None:
        for row in self.step_rows:
            for widget in row:
                widget.destroy()
        self.step_rows = []
        space = self.theme.space
        for index, step in enumerate(self.progress.steps, start=1):
            icon = self.theme.status_icon(self.tk, self.steps_frame)
            icon.show(init_progress.PENDING)
            label = self.ttk.Label(self.steps_frame, text=step.label, style="Card.TLabel")
            duration = self.ttk.Label(self.steps_frame, text="", style="Card.Mono.TLabel")
            vertical = space(init_theme.SPACE_EXTRA_SMALL) // 2
            icon.grid(row=index, column=0, sticky="w", padx=(0, space(init_theme.SPACE_SMALL)), pady=vertical)
            label.grid(row=index, column=1, sticky="ew", pady=vertical)
            duration.grid(row=index, column=2, sticky="e", pady=vertical)
            self.step_rows.append((icon, label, duration))

    # -- process ---------------------------------------------------------------

    def start(self) -> bool:
        self.initialize_button.configure(state="disabled")
        try:
            self.process = subprocess.Popen(self.command, cwd=str(self.cwd), env=self.environment,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as error:
            self.progress.error = f"Setup could not start: {error}"
            self._finish(None)
            return False
        self.pump = OutputPump(self.process, self.log_path, self.echo)
        # Tk variables must be finalized on the Tk thread. Collect unreachable
        # ones (the selection window's, an earlier run's) now, so a collection
        # triggered on the reader thread never finds Tk objects to delete.
        gc.collect()
        self.pump.start()
        self._schedule(POLL_MILLISECONDS, self._poll)
        self._schedule(CLOCK_MILLISECONDS, self._tick)
        self._schedule(SPINNER_MILLISECONDS, self._spin)
        return True

    def _schedule(self, milliseconds: int, callback) -> None:
        # Each periodic callback has at most one pending timer; run() cancels
        # them so none fires after the window is destroyed.
        self.timers[callback.__name__] = self.window.after(milliseconds, callback)

    def run(self) -> int:
        """Start the child and the event loop; return the exit status for init.sh."""
        try:
            self.start()
            self.window.mainloop()
        finally:
            if self.process is not None and self.process.poll() is None:
                signal_process_tree(self.process, force=True)
                self.process.wait()
            for timer in self.timers.values():
                try:
                    self.window.after_cancel(timer)
                except self.tk.TclError:
                    pass
            if self.process is not None and self.process.stdout is not None:
                self.process.stdout.close()
            # Hide rather than destroy, so the Tcl interpreter that macOS
            # Apple events use stays valid until the process exits.
            init_theme.release_root(self.window)
        if self.progress.outcome == init_progress.CANCELLED:
            return CANCELLED_EXIT_CODE
        if self.exit_code is None:
            return 1
        return self.exit_code

    def request_cancel(self) -> None:
        if self.finished_at is not None or self.is_cancelling:
            return
        if self.messagebox is not None and not self.messagebox.askyesno(
                "Cancel setup?",
                "Stop setup now? Finished steps are kept, and running ./init.sh again resumes from them.",
                parent=self.window):
            return
        self.cancel()

    def cancel(self) -> None:
        if self.process is None or self.finished_at is not None or self.is_cancelling:
            return
        self.is_cancelling = True
        self.status.set("Cancelling…")
        self.cancel_button.configure(state="disabled")
        signal_process_tree(self.process, force=False)
        self._schedule(TERMINATE_GRACE_MILLISECONDS, self._force_stop)

    def _force_stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            signal_process_tree(self.process, force=True)

    def activate_default(self) -> None:
        """Enter: run the editor or close after success, retry after a failure or cancellation; nothing while running."""
        if self.finished_at is None:
            return
        if self.progress.outcome == init_progress.DONE:
            if self.editor_command is not None:
                self.run_editor()
            else:
                self.window.quit()
        else:
            self.retry()

    def request_close(self) -> None:
        if self.finished_at is not None:
            self.window.quit()
            return
        if self.messagebox is not None and not self.messagebox.askyesno(
                "Stop setup?", "Setup is still running. Stop it and close this window?", parent=self.window):
            return
        self.should_close_when_done = True
        self.cancel()

    # -- updates -----------------------------------------------------------------

    def _poll(self) -> None:
        assert self.pump is not None
        appended: list[str] = []
        ended = False
        for _ in range(MAX_LINES_PER_POLL):
            try:
                line = self.pump.lines.get_nowait()
            except queue.Empty:
                break
            if line is None:
                ended = True
                break
            if self.progress.feed(line, self.clock()):
                appended.append(init_progress.clean_line(line))
        if appended:
            self.recent_lines.extend(appended)
            self._append_log(appended)
        self._refresh()
        if ended:
            self.exit_code = self.process.wait()
            self.process.stdout.close()
            self._finish(self.exit_code)
            return
        self._schedule(POLL_MILLISECONDS, self._poll)

    def _tick(self) -> None:
        if self.finished_at is not None:
            return
        self._refresh()
        self._schedule(CLOCK_MILLISECONDS, self._tick)

    def _spin(self) -> None:
        if self.finished_at is not None:
            return
        for icon, _label, _duration in self.step_rows:
            icon.advance()
        self._schedule(SPINNER_MILLISECONDS, self._spin)

    def _append_log(self, lines: list[str]) -> None:
        at_end = self.log.yview()[1] >= 1.0
        self.log.configure(state="normal")
        self.log.insert("end", "\n".join(lines) + "\n")
        self.log_lines += len(lines)
        if self.log_lines > MAX_LOG_LINES:
            excess = self.log_lines - MAX_LOG_LINES
            self.log.delete("1.0", f"{excess + 1}.0")
            self.log_lines = MAX_LOG_LINES
        self.log.configure(state="disabled")
        if at_end:
            self.log.see("end")

    def _refresh(self) -> None:
        progress = self.progress
        palette = self.theme.palette
        now = self.clock()
        end = self.finished_at if self.finished_at is not None else now
        self.elapsed.set(init_progress.format_elapsed(end - self.started_at))
        if len(self.step_rows) != len(progress.steps):
            self._rebuild_steps()
        for (icon, label, duration), step in zip(self.step_rows, progress.steps):
            icon.show(step.status)
            if step.status == init_progress.RUNNING:
                style, colour = "Card.Strong.TLabel", palette.text
            elif step.status == init_progress.FAILED:
                style, colour = "Card.Strong.TLabel", palette.error
            elif step.status == init_progress.PENDING:
                style, colour = "Card.TLabel", palette.secondary_text
            else:
                style, colour = "Card.TLabel", palette.text
            label.configure(text=step.label, style=style, foreground=colour)
            if step.started_at is not None:
                finished = step.finished_at if step.finished_at is not None else (
                    now if step.status == init_progress.RUNNING else None)
                if step.is_up_to_date and step.status != init_progress.FAILED:
                    duration.configure(text=init_progress.UP_TO_DATE_LABEL)
                elif finished is not None:
                    duration.configure(text=init_progress.format_elapsed(finished - step.started_at))
        if self.finished_at is not None:
            return
        if progress.has_started_step:
            if str(self.bar.cget("mode")) != "determinate":
                self.bar.stop()
                self.bar.configure(mode="determinate")
            self.bar.configure(value=progress.fraction())
            step = progress.steps[progress.current]
            if not self.is_cancelling:
                self.status.set(f"Step {progress.current + 1} of {len(progress.steps)}  ·  {step.label}")
        if progress.detail_total > 0:
            self.detail.set(shorten(f"{progress.detail_done}/{progress.detail_total}  {progress.detail_text}"))
        else:
            self.detail.set("Working…" if progress.has_started_step else "")
        # The newest output line shows activity even when a step reports no counts.
        self.activity.set("" if progress.is_last_line_status else shorten(progress.last_line))
        notice = progress.user_action or progress.note
        if notice:
            self.note.set(notice)
            self.note_label.grid()
        else:
            self.note_label.grid_remove()
        self.fit_to_content()

    def fit_to_content(self) -> None:
        """Resize the window when its content asks for a different height: the outcome banner, a note or the log.

        The window grows to the content's height but never past the usable screen; beyond that the content
        scrolls and the footer stays visible.
        """
        self.window.update_idletasks()
        height = self.scroll.content.winfo_reqheight()
        if height != self.fitted_height:
            self.fitted_height = height
            init_theme.fit_to_screen(self.window, self.theme.scale)

    def _finish(self, exit_code: int | None) -> None:
        now = self.clock()
        palette = self.theme.palette
        self.finished_at = now
        self.progress.finish(exit_code, now, cancelled=self.is_cancelling)
        self.bar.stop()
        self.bar.configure(mode="determinate", value=self.progress.fraction())
        self.detail.set("")
        self.activity.set("")
        self.detail_label.grid_remove()
        self.activity_label.grid_remove()
        outcome = self.progress.outcome
        if outcome == init_progress.DONE:
            self.heading.set("Ludus is ready")
            self.status.set("Setup finished successfully.")
            self.status_label.configure(foreground=palette.success)
            summary = "\n".join(self.progress.summary[:MAX_SUMMARY_LINES]) or "Ludus initialization complete."
            self.outcome.set(summary)
            self.outcome_banner.set_kind(init_theme.SUCCESS)
            self.footer.hint.configure(text=SUCCESS_HINT if self.editor_command is None else SUCCESS_EDITOR_HINT)
        elif outcome == init_progress.CANCELLED:
            self.heading.set("Setup cancelled")
            self.status.set("Setup was cancelled. Run ./init.sh again to continue.")
            self.status_label.configure(foreground=palette.warning)
            self.outcome.set("")
            self.footer.hint.configure(text=FAILURE_HINT)
        else:
            self.heading.set("Setup failed")
            failed = self.failed_step_label()
            self.status.set(f"Setup failed during: {failed}" if failed else "Setup failed.")
            self.status_label.configure(foreground=palette.error)
            self.failure_hint = init_failure.find_hint([*self.recent_lines, self.progress.error])
            advice = self.failure_hint.advice if self.failure_hint is not None else DEFAULT_FAILURE_ADVICE
            self.outcome.set("Error: " + self.progress.error + "\n\n" + advice +
                             "\nThe last lines of output are below; the full log has everything.")
            self.outcome_banner.set_kind(init_theme.ERROR)
            self.footer.hint.configure(text=FAILURE_HINT)
            self.copy_button.configure(text=COPY_REPORT_LABEL)
            self.log.configure(height=FAILURE_LOG_HEIGHT_LINES)
            self.show_log(True)
        if self.outcome.get():
            self.outcome_label.grid()
        self.note_label.grid_remove()
        self._refresh()
        if self.outcome.get():
            # On a short screen the summary may sit below the fold; bring it into view.
            self.window.after_idle(lambda: self.scroll.show(self.outcome_label))
        offers_editor = outcome == init_progress.DONE and self.editor_command is not None
        self.cancel_button.configure(text="Close", state="normal", command=self.window.quit,
                                     style="Primary.TButton" if outcome == init_progress.DONE and not offers_editor
                                     else "TButton")
        if outcome != init_progress.DONE:
            self.initialize_button.configure(text="Retry", state="normal", style="Primary.TButton")
            self.initialize_button.focus_set()
        elif offers_editor:
            # The editor is built: running it is the next step, Close the alternative.
            self.initialize_button.pack_forget()
            self.editor_button.pack(side="left", padx=(0, self.theme.space(init_theme.SPACE_SMALL)),
                                    before=self.cancel_button)
            self.editor_button.focus_set()
        else:
            # Nothing is left to run; Close is the only action.
            self.initialize_button.pack_forget()
            self.cancel_button.focus_set()
        if self.should_close_when_done:
            self.window.quit()

    def run_editor(self) -> bool:
        """Start the built editor through its launcher, then close setup once it has not failed at once.

        The editor gets its own session and keeps running after setup exits.
        When the launcher cannot start or exits with an error, the window stays
        open and shows the launcher's last lines, which name the fix.
        """
        if (self.editor_command is None or self.finished_at is None or self.progress.outcome != init_progress.DONE
                or self.editor_process is not None):
            return False
        environment = {name: value for name, value in self.environment.items()
                       if name != init_progress.PROGRESS_ENVIRONMENT}
        output = subprocess.DEVNULL
        try:
            if self.editor_log_path is not None:
                self.editor_log_path.parent.mkdir(parents=True, exist_ok=True)
                output = self.editor_log_path.open("wb")
            try:
                self.editor_process = subprocess.Popen(self.editor_command, cwd=str(self.cwd), env=environment,
                                                       stdin=subprocess.DEVNULL, stdout=output,
                                                       stderr=subprocess.STDOUT, start_new_session=True)
            finally:
                if output is not subprocess.DEVNULL:
                    output.close()
        except OSError as error:
            self._show_editor_failure([f"The editor could not start: {error}"])
            return False
        self.editor_button.configure(state="disabled")
        self.status.set(STARTING_EDITOR_STATUS)
        self._schedule(EDITOR_START_CHECK_MILLISECONDS, self._check_editor)
        return True

    def _check_editor(self) -> None:
        assert self.editor_process is not None
        exit_code = self.editor_process.poll()
        if exit_code is None or exit_code == 0:
            self.is_editor_started = True
            self.window.quit()
            return
        self.editor_process = None
        lines = [f"The editor did not start (exit code {exit_code})."]
        lines.extend(self.editor_log_tail())
        self._show_editor_failure(lines)

    def editor_log_tail(self) -> list[str]:
        if self.editor_log_path is None:
            return []
        try:
            text = self.editor_log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return []
        return [line for line in text.splitlines() if line.strip()][-MAX_EDITOR_FAILURE_LINES:]

    def _show_editor_failure(self, lines: list[str]) -> None:
        if self.editor_log_path is not None:
            lines = [*lines, f"Full output: {self.editor_log_path}"]
        self.status.set("Setup finished successfully.")
        self.note.set("\n".join(lines))
        self.note_label.grid()
        self.editor_button.configure(state="normal")
        self.fit_to_content()
        self.window.after_idle(lambda: self.scroll.show(self.note_label))

    def retry(self) -> None:
        """Run the same selection again after a failure or cancellation."""
        if self.finished_at is None or self.progress.outcome == init_progress.DONE:
            return
        self.progress = init_progress.SetupProgress.from_plan(self.steps)
        self._rebuild_steps()
        self.started_at = self.clock()
        self.finished_at = None
        self.is_cancelling = False
        self.exit_code = None
        self.heading.set("Setting up Ludus")
        self.status.set("Starting setup…")
        self.status_label.configure(foreground=self.theme.palette.text)
        self.outcome.set("")
        self.outcome_label.grid_remove()
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.log_lines = 0
        self.recent_lines.clear()
        self.failure_hint = None
        self.log.configure(height=LOG_HEIGHT_LINES)
        self.copy_button.configure(text=COPY_OUTPUT_LABEL)
        self.bar.configure(mode="indeterminate", value=0)
        self.bar.start()
        self.detail_label.grid()
        self.activity_label.grid()
        self.footer.hint.configure(text=RUNNING_HINT)
        self.initialize_button.configure(text="Initialize", style="TButton")
        self.cancel_button.configure(text="Cancel", state="normal", command=self.request_cancel, style="TButton")
        self.start()

    # -- log controls ------------------------------------------------------------

    def toggle_log(self) -> None:
        self.show_log(not self.log_frame.winfo_ismapped())

    def show_log(self, visible: bool) -> None:
        if visible:
            self.log_frame.grid()
            self.details_button.configure(text="Hide output")
            self.log.see("end")
        else:
            self.log_frame.grid_remove()
            self.details_button.configure(text="Show output")
        self.fit_to_content()

    def copy_log(self) -> None:
        """Copy the output; after a failure copy a report with the step, hint, log path and last lines."""
        if self.finished_at is not None and self.progress.outcome == init_progress.FAILED:
            self._copy_text("\n".join(self.failure_report(include_tail=True)) + "\n")
        else:
            self._copy_text(self.log.get("1.0", "end"))

    def copy_log_path(self) -> None:
        if self.log_path is not None:
            self._copy_text(str(self.log_path))

    def open_log(self) -> bool:
        """Open the full log in the desktop's viewer; copy its path instead when that fails."""
        if self.log_path is not None and init_failure.open_in_viewer(self.log_path):
            return True
        self.copy_log_path()
        self.note.set(OPEN_LOG_FAILED_NOTE)
        self.note_label.grid()
        return False

    def _copy_text(self, text: str) -> None:
        self.window.clipboard_clear()
        self.window.clipboard_append(text)

    def failed_step_label(self) -> str | None:
        return next((step.label for step in self.progress.steps if step.status == init_progress.FAILED), None)

    def failure_report(self, *, include_tail: bool) -> list[str]:
        """The failure as plain text, shared with the terminal summary printed after the window closes."""
        last_lines = init_failure.tail(self.recent_lines) if include_tail else ()
        return init_failure.report_lines(step_label=self.failed_step_label(), error=self.progress.error,
                                         hint=self.failure_hint, log_path=self.log_path, last_lines=last_lines)


def shorten(text: str) -> str:
    if len(text) <= MAX_DETAIL_CHARACTERS:
        return text
    return text[:MAX_DETAIL_CHARACTERS - 1] + "…"


def describe(args) -> str:
    from init_ui import PERSONAS
    persona = next((item.label for item in PERSONAS if item.key == args.persona), args.persona)
    return f"{persona} · {args.preset} · Repository: {Path(__file__).resolve().parents[2]}"


def run_setup(args, engine, *, geometry: str | None = None) -> int:
    """Run the selected setup in a child process while showing its progress."""
    try:
        import tkinter as tk
        from tkinter import messagebox, ttk
    except ImportError:
        return int(args.func(args))
    root = engine.repo_root()
    environment = child_environment()
    if args.ci:
        environment["CI"] = "true"
    steps = init_progress.plan(args, environment=environment)
    echo = getattr(sys.stdout, "buffer", None)
    editor_log_path = root / EDITOR_LOG_RELATIVE_PATH
    try:
        window = SetupWindow(tk, ttk, steps=steps, command=child_command(args, root), title=describe(args),
                             log_path=root / "out/init/setup.log", cwd=root, environment=environment,
                             geometry=geometry, echo=echo, messagebox=messagebox,
                             editor_command=editor_command(args, root, engine), editor_log_path=editor_log_path)
    except tk.TclError as error:
        print(f"Graphical progress unavailable: {error}\nContinuing in this terminal.", file=sys.stderr)
        return int(args.func(args))
    result = window.run()
    if window.is_editor_started:
        print(f"Started the Ludus editor from the setup window. Its output goes to {editor_log_path}.")
    if result == CANCELLED_EXIT_CODE:
        print("Initialization cancelled from the setup window.")
    elif window.progress.outcome == init_progress.FAILED:
        # The output was echoed above; repeat the diagnosis where the terminal ends.
        print("")
        for line in window.failure_report(include_tail=False):
            print(line)
    return result
