"""Stable error vocabulary shared by the CLI and Editor backend.

Every recoverable failure in the host tooling is reported with a stable string
``code`` plus a human ``message``. The codes match the Editor E0 ``ResultCode``
vocabulary (``apps/editor/src/internal/project_descriptor.h``) so the CLI and the
Editor streaming protocol describe the same failures identically (P07). A
``ToolingError`` never crosses into C++: it is caught at the protocol/CLI
boundary and translated to a structured result.
"""

from __future__ import annotations


class ToolingError(Exception):
    """A recoverable host-tooling failure with a stable code and message."""

    def __init__(self, code: str, message: str, *, location: dict | None = None,
                 diagnostics: list | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        # Optional authored-source mapping (package-relative source, configuration,
        # node, line). Diagnostics list mapped validation or compiler messages; both are copied
        # into versioned JSON results and never contain machine-absolute paths.
        self.location = location
        self.diagnostics = diagnostics

    def __str__(self) -> str:  # pragma: no cover - trivial
        return f"{self.code}: {self.message}"


# Stable exit statuses for the human CLI. The Editor uses the structured
# streaming protocol instead of parsing human text (design "public commands").
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_UNAVAILABLE = 3  # a required prerequisite (SDK/tool) is missing
EXIT_CANCELLED = 130


# Result/error codes. Keep in sync with the C++ ResultCode enum and with
# editor_project.ProjectError codes.
INVALID_PROJECT = "InvalidProject"
UNSUPPORTED_VERSION = "UnsupportedVersion"
CONFLICT = "Conflict"
BUSY = "Busy"
MISSING_TOOLS = "MissingTools"
BOOTSTRAP_STALE = "BootstrapStale"

# SDK / store codes.
SDK_NOT_FOUND = "SdkNotFound"
SDK_INCOMPATIBLE = "SdkIncompatible"
SDK_CORRUPT = "SdkCorrupt"
ARCHIVE_INVALID = "ArchiveInvalid"
DIGEST_MISMATCH = "DigestMismatch"
MANIFEST_INVALID = "ManifestInvalid"
INSTALL_CANCELLED = "InstallCancelled"

# Project create / resolution codes.
DESTINATION_EXISTS = "DestinationExists"
GENERATION_FAILED = "GenerationFailed"
LOCK_MISMATCH = "LockMismatch"
MIGRATION_FAILED = "MigrationFailed"
UNRESOLVED_LOCK = "UnresolvedLock"
STAMP_CHANGED = "StampChanged"
