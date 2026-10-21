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


POLL_MILLISECONDS = 50
CLOCK_MILLISECONDS = 250
TERMINATE_GRACE_MILLISECONDS = 5000
READ_CHUNK_BYTES = 65536
MAX_LINES_PER_POLL = 400
MAX_LOG_LINES = 5000
MAX_DETAIL_CHARACTERS = 84
WRAP_PIXELS = 620
LOG_HEIGHT_LINES = 10
# The rest of the runner's closing text stays in the output log, so the window
# still fits a laptop screen with the log open.
MAX_SUMMARY_LINES = 3
WINDOW_MINIMUM_WIDTH = 680
WINDOW_MINIMUM_HEIGHT = 520
SPINNER_FRAMES = "◐◓◑◒"
STATUS_ICONS = {
    init_progress.PENDING: "○",
    init_progress.DONE: "✓",
    init_progress.FAILED: "✗",
    init_progress.CANCELLED: "–",
}
STATUS_COLOURS = {
    init_progress.PENDING: "#7a7a7a",
    init_progress.RUNNING: "#1a5fb4",
    init_progress.DONE: "#26a269",
    init_progress.FAILED: "#c01c28",
    init_progress.CANCELLED: "#9a6700",
}
NOTE_BACKGROUND = "#fff4cc"
ERROR_BACKGROUND = "#fde7e9"
SUCCESS_BACKGROUND = "#e6f4ea"
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
        self.spinner_index = 0
        self.log_lines = 0
        self.step_rows: list[tuple] = []
        self.timers: dict[str, str] = {}
        self.default_foreground = "black"

        self.window = tk.Tk()
        self.window.title("Ludus setup")
        self.window.minsize(WINDOW_MINIMUM_WIDTH, WINDOW_MINIMUM_HEIGHT)
        if geometry:
            self.window.geometry(geometry)
        self._build(title)
        self.window.protocol("WM_DELETE_WINDOW", self.request_close)

    # -- layout --------------------------------------------------------------

    def _build(self, title: str) -> None:
        tk, ttk = self.tk, self.ttk
        panel = ttk.Frame(self.window, padding=24)
        panel.grid(sticky="nsew")
        self.window.columnconfigure(0, weight=1)
        self.window.rowconfigure(0, weight=1)
        panel.columnconfigure(0, weight=1)
        self.panel = panel
        self.heading = tk.StringVar(value="Setting up Ludus")
        ttk.Label(panel, textvariable=self.heading, font=("", 20, "bold")).grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(panel, text=title, wraplength=WRAP_PIXELS).grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 12))

        self.status = tk.StringVar(value="Starting setup…")
        self.status_label = tk.Label(panel, textvariable=self.status, font=("", 12, "bold"), anchor="w",
                                     justify="left", wraplength=WRAP_PIXELS)
        self.status_label.grid(row=2, column=0, sticky="ew")
        self.elapsed = tk.StringVar(value="Elapsed 00:00")
        ttk.Label(panel, textvariable=self.elapsed).grid(row=2, column=1, sticky="e")

        self.bar = ttk.Progressbar(panel, mode="indeterminate", maximum=1.0)
        self.bar.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(8, 4))
        self.bar.start()
        self.detail = tk.StringVar(value="")
        self.activity = tk.StringVar(value="")
        details = ttk.Frame(panel)
        details.grid(row=4, column=0, columnspan=2, sticky="ew")
        ttk.Label(details, textvariable=self.detail).grid(row=0, column=0, sticky="w")
        ttk.Label(details, textvariable=self.activity, foreground="#5e5c64", font=("TkFixedFont", 9)).grid(
            row=1, column=0, sticky="w")

        self.note = tk.StringVar(value="")
        self.note_label = tk.Label(panel, textvariable=self.note, background=NOTE_BACKGROUND, anchor="w",
                                   justify="left", wraplength=WRAP_PIXELS, padx=8, pady=6)
        self.note_row = 5
        self.note_label.grid(row=self.note_row, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.note_label.grid_remove()

        steps = ttk.LabelFrame(panel, text="Steps", padding=8)
        steps.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        steps.columnconfigure(1, weight=1)
        self.steps_frame = steps
        self._rebuild_steps()

        self.outcome = tk.StringVar(value="")
        self.outcome_label = tk.Label(panel, textvariable=self.outcome, anchor="w", justify="left",
                                      wraplength=WRAP_PIXELS, padx=8, pady=8)
        self.outcome_label.grid(row=7, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        self.outcome_label.grid_remove()

        log_bar = ttk.Frame(panel)
        log_bar.grid(row=8, column=0, columnspan=2, sticky="ew", pady=(12, 4))
        log_bar.columnconfigure(1, weight=1)
        self.details_button = ttk.Button(log_bar, text="Show output ▸", command=self.toggle_log)
        self.details_button.grid(row=0, column=0, sticky="w")
        if self.log_path is not None:
            ttk.Label(log_bar, text=f"Full log: {self.log_path}", foreground="#5e5c64").grid(
                row=0, column=1, sticky="w", padx=(12, 0))

        self.log_frame = ttk.Frame(panel)
        self.log_frame.grid(row=9, column=0, columnspan=2, sticky="nsew")
        self.log_frame.columnconfigure(0, weight=1)
        self.log_frame.rowconfigure(0, weight=1)
        self.log = tk.Text(self.log_frame, height=LOG_HEIGHT_LINES, wrap="none", font=("TkFixedFont", 9),
                           state="disabled", background="#1e1e1e", foreground="#d0d0d0")
        scroll = ttk.Scrollbar(self.log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll.grid(row=0, column=1, sticky="ns")
        self.log_frame.grid_remove()
        panel.rowconfigure(9, weight=1)

        buttons = ttk.Frame(panel)
        buttons.grid(row=10, column=0, columnspan=2, sticky="e", pady=(16, 0))
        self.copy_button = ttk.Button(buttons, text="Copy output", command=self.copy_log)
        self.copy_button.pack(side="left", padx=(0, 8))
        # Initialize stays disabled while setup runs; after a failure or a
        # cancellation it reruns the same selection.
        self.initialize_button = ttk.Button(buttons, text="Initialize", state="disabled", command=self.retry)
        self.initialize_button.pack(side="left", padx=(0, 8))
        self.cancel_button = ttk.Button(buttons, text="Cancel", command=self.request_cancel)
        self.cancel_button.pack(side="left")

    def _rebuild_steps(self) -> None:
        for row in self.step_rows:
            for widget in row:
                widget.destroy()
        self.step_rows = []
        for index, step in enumerate(self.progress.steps):
            icon = self.tk.Label(self.steps_frame, text=STATUS_ICONS[init_progress.PENDING], width=2)
            label = self.tk.Label(self.steps_frame, text=step.label, anchor="w")
            self.default_foreground = label.cget("foreground")
            duration = self.tk.Label(self.steps_frame, text="", anchor="e", foreground="#5e5c64")
            icon.grid(row=index, column=0, sticky="w")
            label.grid(row=index, column=1, sticky="ew")
            duration.grid(row=index, column=2, sticky="e")
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
            try:
                self.window.destroy()
            except self.tk.TclError:
                pass
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
        self.spinner_index = (self.spinner_index + 1) % len(SPINNER_FRAMES)
        self._refresh()
        self._schedule(CLOCK_MILLISECONDS, self._tick)

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
        now = self.clock()
        end = self.finished_at if self.finished_at is not None else now
        self.elapsed.set("Elapsed " + init_progress.format_elapsed(end - self.started_at))
        if len(self.step_rows) != len(progress.steps):
            self._rebuild_steps()
        for (icon, label, duration), step in zip(self.step_rows, progress.steps):
            colour = STATUS_COLOURS[step.status]
            if step.status == init_progress.RUNNING:
                glyph = SPINNER_FRAMES[self.spinner_index]
            else:
                glyph = STATUS_ICONS[step.status]
            icon.configure(text=glyph, foreground=colour)
            weight = "bold" if step.status == init_progress.RUNNING else "normal"
            label.configure(text=step.label,
                            foreground=colour if step.status != init_progress.DONE else self.default_foreground,
                            font=("", 10, weight))
            if step.started_at is not None:
                finished = step.finished_at if step.finished_at is not None else (
                    now if step.status == init_progress.RUNNING else None)
                if finished is not None:
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
                self.status.set(f"Step {progress.current + 1} of {len(progress.steps)}: {step.label}")
        if progress.detail_total > 0:
            self.detail.set(shorten(f"{progress.detail_done}/{progress.detail_total}  {progress.detail_text}"))
        else:
            self.detail.set("Working…" if progress.has_started_step else "")
        # The newest output line shows activity even when a step reports no counts.
        self.activity.set("" if progress.is_last_line_status else shorten(progress.last_line))
        if progress.note:
            self.note.set("⚠ " + progress.note)
            self.note_label.grid()
        else:
            self.note_label.grid_remove()

    def _finish(self, exit_code: int | None) -> None:
        now = self.clock()
        self.finished_at = now
        self.progress.finish(exit_code, now, cancelled=self.is_cancelling)
        self.bar.stop()
        self.bar.configure(mode="determinate", value=self.progress.fraction())
        self.detail.set("")
        self.activity.set("")
        outcome = self.progress.outcome
        if outcome == init_progress.DONE:
            self.heading.set("Ludus is ready")
            self.status.set("Setup finished successfully.")
            self.status_label.configure(foreground=STATUS_COLOURS[init_progress.DONE])
            summary = "\n".join(self.progress.summary[:MAX_SUMMARY_LINES]) or "Ludus initialization complete."
            self.outcome.set(summary)
            self.outcome_label.configure(background=SUCCESS_BACKGROUND)
        elif outcome == init_progress.CANCELLED:
            self.heading.set("Setup cancelled")
            self.status.set("Setup was cancelled. Run ./init.sh again to continue.")
            self.status_label.configure(foreground=STATUS_COLOURS[init_progress.CANCELLED])
            self.outcome.set("")
        else:
            self.heading.set("Setup failed")
            failed = next((step.label for step in self.progress.steps if step.status == init_progress.FAILED), None)
            self.status.set(f"Setup failed during: {failed}" if failed else "Setup failed.")
            self.status_label.configure(foreground=STATUS_COLOURS[init_progress.FAILED])
            self.outcome.set("Error: " + self.progress.error +
                             "\nFix the reported problem, then choose Retry or run ./init.sh again. The output below has the details.")
            self.outcome_label.configure(background=ERROR_BACKGROUND)
            self.show_log(True)
        if self.outcome.get():
            self.outcome_label.grid()
        self.note_label.grid_remove()
        self._refresh()
        self.cancel_button.configure(text="Close", state="normal", command=self.window.quit)
        if outcome != init_progress.DONE:
            self.initialize_button.configure(text="Retry", state="normal")
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
        self.status_label.configure(foreground=self.default_foreground)
        self.outcome.set("")
        self.outcome_label.grid_remove()
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.log_lines = 0
        self.bar.configure(mode="indeterminate", value=0)
        self.bar.start()
        self.initialize_button.configure(text="Initialize")
        self.cancel_button.configure(text="Cancel", state="normal", command=self.request_cancel)
        self.start()

    # -- log controls ------------------------------------------------------------

    def toggle_log(self) -> None:
        self.show_log(not self.log_frame.winfo_ismapped())

    def show_log(self, visible: bool) -> None:
        if visible:
            self.log_frame.grid()
            self.details_button.configure(text="Hide output ▾")
            self.log.see("end")
        else:
            self.log_frame.grid_remove()
            self.details_button.configure(text="Show output ▸")

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
