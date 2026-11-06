"""Native artifact inspection errors, importable without the POSIX editor tooling.

play_probe re-exports ProbeError; the Mach-O and Windows readers import it from here so
they also load on hosts where editor_tool (fcntl, process groups) is unavailable.
"""
from __future__ import annotations


class ProbeError(ValueError):
    def __init__(self, message: str, *, cleanup_confirmed: bool = True):
        super().__init__(message)
        self.cleanup_confirmed = cleanup_confirmed
