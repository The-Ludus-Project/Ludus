"""Ludus design tokens: one JSON source for the Editor, the init window and the browser editor.

``config/design-tokens.json`` owns every colour, spacing and radius token and the
contrast pairs that must hold. Consumers:

* the Qt editor reads a header generated from the JSON at configure time
  (``apps/editor/src/internal/design_tokens.h.in``);
* ``init_theme`` builds its palettes from ``colour`` and ``spacing``;
* the browser editor's ``editor.css`` carries a generated block of CSS custom
  properties between the ``BEGIN``/``END`` markers below. Refresh it with
  ``python3 scripts/python/design_tokens.py --write``; ``--check`` (and the unit
  test) fails when it is stale.

Colour names are ``role.name`` (for example ``surface.workspace``); each has a
``dark`` and a ``light`` value as ``#rrggbb``.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TOKEN_FILE = REPOSITORY_ROOT / "config" / "design-tokens.json"
BROWSER_STYLESHEET = REPOSITORY_ROOT / "apps" / "editor" / "browser" / "editor.css"

DARK = "dark"
LIGHT = "light"
APPEARANCES = (DARK, LIGHT)

BEGIN_MARKER = "/* BEGIN generated design tokens (scripts/python/design_tokens.py --write) */"
END_MARKER = "/* END generated design tokens */"

COLOUR_PATTERN = re.compile(r"^#[0-9a-f]{6}$")
NAME_PATTERN = re.compile(r"^[a-z]+\.[a-zA-Z]+$")
LINEAR_THRESHOLD = 0.03928
LINEAR_DIVISOR = 12.92
GAMMA_OFFSET = 0.055
GAMMA_SCALE = 1.055
GAMMA_EXPONENT = 2.4
FLARE = 0.05
LUMINANCE_WEIGHTS = (0.2126, 0.7152, 0.0722)
CHANNEL_MAXIMUM = 255.0
CHANNEL_OFFSETS = (1, 3, 5)


class TokenError(ValueError):
    """The token file is malformed."""


def load(path: Path = TOKEN_FILE) -> dict:
    """Read and validate the token file."""
    document = json.loads(path.read_text(encoding="utf-8"))
    validate(document)
    return document


def validate(document: dict) -> None:
    """Raise ``TokenError`` unless names, colours and contrast pairs are well formed."""
    colours = document.get("colours")
    if not isinstance(colours, dict) or not colours:
        raise TokenError("colours must be a non-empty object")
    for name, entry in colours.items():
        if NAME_PATTERN.match(name) is None:
            raise TokenError(f"colour name '{name}' must look like role.name")
        for appearance in APPEARANCES:
            value = entry.get(appearance, "")
            if COLOUR_PATTERN.match(value) is None:
                raise TokenError(f"colour '{name}' {appearance} value '{value}' must be #rrggbb in lowercase")
    for pair in document.get("contrast", []):
        for role in ("foreground", "background"):
            if pair.get(role) not in colours:
                raise TokenError(f"contrast pair '{pair.get('label')}' names unknown colour '{pair.get(role)}'")
        if not isinstance(pair.get("minimum"), (int, float)) or pair["minimum"] < 1:
            raise TokenError(f"contrast pair '{pair.get('label')}' needs a minimum of at least 1")
    for group in ("spacing", "radius"):
        if not all(isinstance(value, int) and value > 0 for value in document.get(group, {}).values()):
            raise TokenError(f"{group} values must be positive integers")


def colour(document: dict, name: str, appearance: str) -> str:
    """Return ``#rrggbb`` of token ``name`` for ``appearance``."""
    return document["colours"][name][appearance]


def relative_luminance(value: str) -> float:
    """WCAG 2 relative luminance of ``#rrggbb``."""
    channels = [int(value[index:index + 2], 16) / CHANNEL_MAXIMUM for index in CHANNEL_OFFSETS]
    linear = [c / LINEAR_DIVISOR if c <= LINEAR_THRESHOLD else ((c + GAMMA_OFFSET) / GAMMA_SCALE) ** GAMMA_EXPONENT
              for c in channels]
    return sum(weight * channel for weight, channel in zip(LUMINANCE_WEIGHTS, linear))


def contrast_ratio(first: str, second: str) -> float:
    lighter, darker = sorted((relative_luminance(first), relative_luminance(second)), reverse=True)
    return (lighter + FLARE) / (darker + FLARE)


def failing_pairs(document: dict) -> list[str]:
    """Descriptions of every contrast pair that fails in either appearance."""
    failures = []
    for appearance in APPEARANCES:
        for pair in document["contrast"]:
            ratio = contrast_ratio(colour(document, pair["foreground"], appearance),
                                   colour(document, pair["background"], appearance))
            if ratio < pair["minimum"]:
                failures.append(f"{appearance}: {pair['label']} is {ratio:.2f}:1, needs {pair['minimum']}:1")
    return failures


def css_variable(name: str) -> str:
    """``surface.workspace`` becomes ``--ludus-surface-workspace``; camelCase words are hyphenated."""
    return "--ludus-" + re.sub(r"(?<=[a-z])(?=[A-Z])", "-", name.replace(".", "-")).lower()


def render_css(document: dict) -> str:
    """The generated block, markers included, with dark values by default and light ones on request."""
    def declarations(appearance: str, indent: str) -> list[str]:
        return [f"{indent}{css_variable(name)}: {entry[appearance]};" for name, entry in document["colours"].items()]

    lines = [BEGIN_MARKER, ":root {", "  color-scheme: dark light;"]
    lines += declarations(DARK, "  ")
    lines += [f"  --ludus-space-{re.sub(r'(?<=[a-z])(?=[A-Z])', '-', key).lower()}: {value}px;"
              for key, value in document["spacing"].items()]
    lines += [f"  --ludus-radius-{key}: {value}px;" for key, value in document["radius"].items()]
    lines += ["}", "@media (prefers-color-scheme: light) {", "  :root {"]
    lines += declarations(LIGHT, "    ")
    lines += ["  }", "}", END_MARKER]
    return "\n".join(lines) + "\n"


def stylesheet_is_current(document: dict, stylesheet: str) -> bool:
    start = stylesheet.find(BEGIN_MARKER)
    end = stylesheet.find(END_MARKER)
    if start < 0 or end < start:
        return False
    return stylesheet[start:end + len(END_MARKER)] + "\n" == render_css(document)


def write_stylesheet(document: dict, path: Path = BROWSER_STYLESHEET) -> bool:
    """Replace the generated block in ``path``; return whether the file changed."""
    stylesheet = path.read_text(encoding="utf-8")
    start = stylesheet.find(BEGIN_MARKER)
    end = stylesheet.find(END_MARKER)
    block = render_css(document)
    if start < 0 or end < start:
        updated = block + stylesheet
    else:
        updated = stylesheet[:start] + block + stylesheet[end + len(END_MARKER) + 1:]
    path.write_text(updated, encoding="utf-8")
    return updated != stylesheet


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--check", action="store_true", help="fail when contrast pairs or editor.css are out of date")
    action.add_argument("--write", action="store_true", help="refresh the generated block in editor.css")
    arguments = parser.parse_args(argv)
    document = load()
    if arguments.write:
        print("updated" if write_stylesheet(document) else "already current", BROWSER_STYLESHEET)
        return 0
    problems = failing_pairs(document)
    if not stylesheet_is_current(document, BROWSER_STYLESHEET.read_text(encoding="utf-8")):
        problems.append(f"{BROWSER_STYLESHEET} is stale; run scripts/python/design_tokens.py --write")
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
