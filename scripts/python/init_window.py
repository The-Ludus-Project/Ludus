"""Graphical progress for ./init.sh after the user chooses Initialize.

The engine runner executes as a child process (``engine.py init --cli`` with
the chosen options) so the window stays responsive: a reader thread copies the
child's output to the launching terminal, to ``out/init/setup.log`` and to a
queue that the Tk event loop drains. Steps come from the progress markers
documented in ``init_progress``. The child keeps the terminal as its standard
input, so sudo still prompts there; the window says when it is waiting.
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
from pathlib import Path
from typing import Callable, Sequence

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
# The rest of the runner's closing text stays in the output log, so the window
# still fits a laptop screen with the log open.
MAX_SUMMARY_LINES = 3
RUNNING_HINT = "Esc to cancel setup"
SUCCESS_HINT = "Enter to close"
FAILURE_HINT = "Enter to retry · Esc to close"
CANCELLED_EXIT_CODE = 130


def child_command(args, root: Path) -> list[str]:
    from init_ui import selection_to_argv
    return [sys.executable, str(root / "scripts/python/engine.py"), "init", *selection_to_argv(args)]


def child_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment[init_progress.PROGRESS_ENVIRONMENT] = "1"
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

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        log = None
        if self.log_path is not None:
            try:
                self.log_path.parent.mkdir(parents=True, exist_ok=True)
                log = self.log_path.open("wb")
            except OSError:
                log = None
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
                log.close()
            self.lines.put(None)


class SetupWindow:
    """The running, succeeded, failed and cancelled states of one setup run."""

    def __init__(self, tk, ttk, *, steps: Sequence[init_progress.Step], command: Sequence[str],
                 title: str, log_path: Path | None, cwd: Path, environment: dict[str, str],
                 geometry: str | None = None, echo=None, messagebox=None, clock: Callable[[], float] = time.monotonic):
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
        self.step_rows: list[tuple] = []
        self.timers: dict[str, str] = {}

        self.window = init_theme.create_root(tk)
        self.theme = init_theme.Theme(self.window, ttk)
        self.theme.apply_window_icon(self.window)
        self._build(title)
        init_theme.fit_minimum_size(self.window)
        if geometry:
            self.window.geometry(geometry)
        else:
            init_theme.centre_window(self.window)
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

        panel = ttk.Frame(self.window, style="Workspace.TFrame",
                          padding=(space(init_theme.SPACE_EXTRA_LARGE), space(init_theme.SPACE_LARGE)))
        panel.grid(row=1, column=0, sticky="nsew")
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
        self.copy_button = ttk.Button(buttons, text="Copy output", command=self.copy_log)
        self.copy_button.pack(side="left", padx=gap)
        # Initialize stays disabled while setup runs; after a failure or a
        # cancellation it reruns the same selection.
        self.initialize_button = ttk.Button(buttons, text="Initialize", state="disabled", command=self.retry)
        self.initialize_button.pack(side="left", padx=gap)
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
        """Enter: close after success, retry after a failure or cancellation; nothing while running."""
        if self.finished_at is None:
            return
        if self.progress.outcome == init_progress.DONE:
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
        if progress.note:
            self.note.set(progress.note)
            self.note_label.grid()
        else:
            self.note_label.grid_remove()

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
            self.footer.hint.configure(text=SUCCESS_HINT)
        elif outcome == init_progress.CANCELLED:
            self.heading.set("Setup cancelled")
            self.status.set("Setup was cancelled. Run ./init.sh again to continue.")
            self.status_label.configure(foreground=palette.warning)
            self.outcome.set("")
            self.footer.hint.configure(text=FAILURE_HINT)
        else:
            self.heading.set("Setup failed")
            failed = next((step.label for step in self.progress.steps if step.status == init_progress.FAILED), None)
            self.status.set(f"Setup failed during: {failed}" if failed else "Setup failed.")
            self.status_label.configure(foreground=palette.error)
            self.outcome.set("Error: " + self.progress.error +
                             "\nFix the reported problem, then choose Retry or run ./init.sh again. The output below has the details.")
            self.outcome_banner.set_kind(init_theme.ERROR)
            self.footer.hint.configure(text=FAILURE_HINT)
            self.show_log(True)
        if self.outcome.get():
            self.outcome_label.grid()
        self.note_label.grid_remove()
        self._refresh()
        self.cancel_button.configure(text="Close", state="normal", command=self.window.quit,
                                     style="Primary.TButton" if outcome == init_progress.DONE else "TButton")
        if outcome != init_progress.DONE:
            self.initialize_button.configure(text="Retry", state="normal", style="Primary.TButton")
            self.initialize_button.focus_set()
        else:
            # Nothing is left to run; Close is the only action.
            self.initialize_button.pack_forget()
            self.cancel_button.focus_set()
        if self.should_close_when_done:
            self.window.quit()

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

    def copy_log(self) -> None:
        self.window.clipboard_clear()
        self.window.clipboard_append(self.log.get("1.0", "end"))


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
    try:
        window = SetupWindow(tk, ttk, steps=steps, command=child_command(args, root), title=describe(args),
                             log_path=root / "out/init/setup.log", cwd=root, environment=environment,
                             geometry=geometry, echo=echo, messagebox=messagebox)
    except tk.TclError as error:
        print(f"Graphical progress unavailable: {error}\nContinuing in this terminal.", file=sys.stderr)
        return int(args.func(args))
    result = window.run()
    if result == CANCELLED_EXIT_CODE:
        print("Initialization cancelled from the setup window.")
    return result
