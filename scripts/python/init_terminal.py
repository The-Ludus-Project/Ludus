"""Terminal presentation for ./init.sh: banner, step headers and summaries.

Colour is used only when standard output is an interactive terminal, ``TERM``
is not ``dumb`` and ``NO_COLOR`` (https://no-color.org) is unset or empty.
Text is identical with and without colour, so logs and the graphical setup
window, which reads the runner through a pipe, always see plain lines.
"""
from __future__ import annotations

import os
import sys
from typing import Sequence


NO_COLOUR_ENVIRONMENT = "NO_COLOR"
ESCAPE_START = "\x1b["
ESCAPE_RESET = "\x1b[0m"
# Select Graphic Rendition parameters. Cyan stands in for the Ludus mint,
# which most terminal palettes cannot show exactly.
STYLE_CODES = {
    "bold": "1",
    "dim": "2",
    "accent": "36",
    "success": "32",
    "error": "31",
    "warning": "33",
}
BANNER_TITLE = "Ludus Setup"
STEP_MARKER = "==>"
RULE_CHARACTER = "─"
ASCII_RULE_CHARACTER = "-"
LABEL_GAP = 2


def supports_colour(stream=None, environment=None) -> bool:
    """Whether escapes may be written to ``stream`` (standard output by default)."""
    stream = sys.stdout if stream is None else stream
    environment = os.environ if environment is None else environment
    if environment.get(NO_COLOUR_ENVIRONMENT, "") != "":
        return False
    if environment.get("TERM", "") == "dumb":
        return False
    try:
        return bool(stream.isatty())
    except (AttributeError, ValueError, OSError):
        return False


def supports_unicode(stream=None) -> bool:
    stream = sys.stdout if stream is None else stream
    encoding = str(getattr(stream, "encoding", "") or "").lower().replace("-", "")
    return encoding.startswith("utf")


class Painter:
    """Wrap text in terminal styles when colour is enabled; otherwise return it unchanged."""

    def __init__(self, is_enabled: bool):
        self.is_enabled = is_enabled

    def paint(self, text: str, *styles: str) -> str:
        if not self.is_enabled or not styles or not text:
            return text
        codes = ";".join(STYLE_CODES[style] for style in styles)
        return f"{ESCAPE_START}{codes}m{text}{ESCAPE_RESET}"


def painter_for(stream=None, environment=None) -> Painter:
    return Painter(supports_colour(stream, environment))


def rule(width: int, stream=None) -> str:
    return (RULE_CHARACTER if supports_unicode(stream) else ASCII_RULE_CHARACTER) * width


def aligned_rows(rows: Sequence[tuple[str, str]], painter: Painter, *, indent: str = "  ") -> list[str]:
    """Two columns: dim labels padded to the widest label, then values."""
    if not rows:
        return []
    width = max(len(label) for label, _ in rows) + LABEL_GAP
    return [indent + painter.paint(label.ljust(width), "dim") + value for label, value in rows]


def banner_lines(rows: Sequence[tuple[str, str]], painter: Painter, stream=None) -> list[str]:
    """The opening header: product title, a rule and aligned facts about this run."""
    return [painter.paint(BANNER_TITLE, "bold", "accent"), painter.paint(rule(len(BANNER_TITLE), stream), "accent"),
            *aligned_rows(rows, painter)]


def step_line(index: int, total: int, label: str, painter: Painter) -> str:
    """``==> [3/9] Label`` with a coloured marker and a bold label in a terminal."""
    return f"{painter.paint(STEP_MARKER, 'bold', 'accent')} {painter.paint(f'[{index}/{total}]', 'dim')} {painter.paint(label, 'bold')}"


def command_echo(text: str, painter: Painter) -> str:
    """The ``+ command`` echo before a subprocess runs; dimmed so tool output stands out."""
    return painter.paint(f"+ {text}", "dim")


def up_to_date(text: str, painter: Painter) -> str:
    """The line a step prints when it skips work whose inputs have not changed."""
    return painter.paint(f"Up to date: {text}", "dim")


def heading(text: str, painter: Painter) -> str:
    return painter.paint(text, "bold")


def success(text: str, painter: Painter) -> str:
    return painter.paint(text, "bold", "success")


def failure(text: str, painter: Painter) -> str:
    return painter.paint(text, "bold", "error")


def error_line(message: str, painter: Painter) -> str:
    """``error: message``; the prefix stays plain text so the setup window can parse it."""
    return f"{painter.paint('error:', 'bold', 'error')} {message}"


CHECK_MARKERS = {"passed": ("ok", "success"), "warning": ("warning", "warning"), "failed": ("failed", "error")}


def check_line(label: str, status: str, message: str, painter: Painter, *, width: int = 0) -> str:
    """``  [ok]       Free disk space  120.0 GB free``: one preflight result with a coloured status marker."""
    marker, style = CHECK_MARKERS[status]
    padded = f"[{marker}]".ljust(max(len(f"[{text}]") for text, _style in CHECK_MARKERS.values()))
    return f"  {painter.paint(padded, 'bold', style)} {painter.paint(label.ljust(width), 'dim')}  {message}"
