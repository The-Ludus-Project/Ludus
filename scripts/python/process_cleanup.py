"""Shared completion evidence for owned native processes."""

from dataclasses import dataclass
from typing import Optional

KILL_OBSERVE_DEADLINE = 1.0


@dataclass
class CleanupReport:
    confirmed: bool
    forced: bool = False
    exit_code: Optional[int] = None
    signal: Optional[int] = None
