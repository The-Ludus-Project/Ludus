"""Bounded native artifact inspection before gameplay Create is permitted.

The probe is an owned process because dlopen constructors and Query can crash or
hang. Poll it from the persistent supervisor alongside control and log I/O.
"""
from __future__ import annotations

import os
from pathlib import Path
import struct
import sys
import time
from typing import Any

from editor_tool import OwnedProcess
from play_documents import decode
from play_errors import ProbeError  # noqa: F401  (re-exported for callers)




def elf_identity(path: Path, *, require_symbols: bool = True) -> dict[str, Any]:
    """Inspect x86-64 ELF build ID and embedded debug information without load.

    No external parser or executable is invoked. Every offset/count/read is
    bounded; debug symbols stay in the same immutable module for v1 publication.
    """
    size = path.stat().st_size
    if not 64 <= size <= 512 * 1024 * 1024:
        raise ProbeError("native artifact exceeds the 512 MiB bound or is truncated")
    with path.open("rb") as handle:
        def read(offset: int, count: int) -> bytes:
            if offset < 0 or count < 0 or offset + count > size:
                raise ProbeError("ELF reference outside artifact")
            handle.seek(offset)
            data = handle.read(count)
            if len(data) != count:
                raise ProbeError("artifact changed or truncated during inspection")
            return data

        header = struct.unpack("<16sHHIQQQIHHHHHH", read(0, 64))
        ident, kind, machine, version = header[:4]
        if ident[:7] != b"\x7fELF\x02\x01\x01" or machine != 62 or version != 1 or kind not in (2, 3):
            raise ProbeError("expected a native little-endian x86-64 ELF executable/module")
        phoff, shoff = header[5:7]
        ehsize, phsize, phcount, shsize, shcount, strings_index = header[8:]
        if ehsize != 64 or phsize != 56 or not 0 < phcount <= 1024 or shsize != 64 or not 0 < shcount <= 4096:
            raise ProbeError("unsupported ELF header/table bounds")
        build_ids = set()
        for index in range(phcount):
            entry = struct.unpack("<IIQQQQQQ", read(phoff + index * phsize, phsize))
            if entry[0] != 4:  # PT_NOTE
                continue
            if entry[5] > 64 * 1024:
                raise ProbeError("ELF note region exceeds 64 KiB")
            notes = read(entry[2], entry[5])
            offset = 0
            while offset < len(notes):
                if len(notes) - offset < 12:
                    raise ProbeError("truncated ELF note")
                namesize, descsize, note_type = struct.unpack_from("<III", notes, offset)
                offset += 12
                name_end = offset + namesize
                desc_start = (name_end + 3) & ~3
                desc_end = desc_start + descsize
                next_offset = (desc_end + 3) & ~3
                if namesize > 4096 or descsize > 4096 or next_offset > len(notes):
                    raise ProbeError("invalid ELF note bounds")
                if notes[offset:name_end] == b"GNU\0" and note_type == 3:
                    if not 8 <= descsize <= 64:
                        raise ProbeError("unsupported ELF build ID size")
                    build_ids.add(notes[desc_start:desc_end].hex())
                offset = next_offset
        if len(build_ids) != 1:
            raise ProbeError("artifact must carry exactly one GNU build ID")
        if strings_index >= shcount:
            raise ProbeError("invalid ELF section-name table")
        strings_header = struct.unpack("<IIQQQQIIQQ", read(shoff + strings_index * shsize, shsize))
        if strings_header[5] > 1024 * 1024:
            raise ProbeError("ELF section-name table exceeds 1 MiB")
        names = read(strings_header[4], strings_header[5])
        symbols = False
        for index in range(shcount):
            entry = struct.unpack("<IIQQQQIIQQ", read(shoff + index * shsize, shsize))
            name_index = entry[0]
            if name_index >= len(names):
                raise ProbeError("invalid ELF section name")
            end = names.find(b"\0", name_index)
            if end < 0:
                raise ProbeError("unterminated ELF section name")
            if names[name_index:end] == b".debug_info" and entry[5] > 0:
                read(entry[4], 1)
                if entry[4] + entry[5] > size:
                    raise ProbeError("debug information outside artifact")
                symbols = True
        if require_symbols and not symbols:
            raise ProbeError("Debug/Development artifact has no embedded debug information")
        return {"build_id": next(iter(build_ids)), "embedded_symbols": symbols, "elf_type": kind}


def native_identity(path: Path, symbols: Path | None = None) -> dict[str, Any]:
    """Select the host's native format; never execute the inspected artifact."""
    if sys.platform == "darwin":
        from play_macos import identity
        return identity(path, symbols)
    if sys.platform == "win32":
        # The DLL/executable and its detached PDB; see play_windows.identity.
        from play_windows import identity
        return identity(path, symbols)
    if sys.platform != "linux":
        raise ProbeError("native live editing supports Linux, macOS and Windows artifacts")
    result = elf_identity(path)
    return {**result, "matching_symbols": True,
            "artifact_type": "module" if result["elf_type"] == 3 else "host"}


def validate_metadata(value: Any, expected: dict | None = None) -> dict:
    fields = {"sdk_identity", "abi_major", "abi_minor", "capabilities", "property_schema", "checkpoint_schema"}
    if not isinstance(value, dict) or set(value) != fields:
        raise ProbeError("unknown or incomplete module query metadata")
    identity = value["sdk_identity"]
    if not isinstance(identity, str) or not identity or "\0" in identity or len(identity.encode("utf-8")) > 256:
        raise ProbeError("invalid module SDK identity")
    for key in fields - {"sdk_identity"}:
        if type(value[key]) is not int or not 0 <= value[key] < 2**32:
            raise ProbeError(f"invalid module {key}")
    if value["abi_major"] != 1 or value["abi_minor"] > 1 or value["capabilities"] & ~15 or (value["capabilities"] & 8 and value["abi_minor"] < 1):
        raise ProbeError("unsupported module ABI/capabilities")
    if (value["capabilities"] & 1 and value["checkpoint_schema"] == 0) or \
            (value["capabilities"] & 2 and value["property_schema"] == 0):
        raise ProbeError("advertised capability has no schema")
    if expected is not None:
        for key in fields:
            if value[key] != expected.get(key):
                raise ProbeError(f"manifest/query disagreement: {key}")
    return dict(value)


def debugger_stopped(pid: int) -> bool:
    if sys.platform == "darwin":
        from editor_process_macos import process_stopped
        return process_stopped(pid)
    try:
        # Non-stop debugging can suspend a game worker while the leader runs.
        # Any traced stopped thread keeps that generation's code reachable.
        for index, thread in enumerate(Path(f"/proc/{pid}/task").iterdir()):
            if index >= 1024:
                return True  # unable to prove all threads clear within budget
            try:
                fields = dict(line.split(":", 1) for line in (thread / "status").read_text().splitlines())
            except FileNotFoundError:
                continue  # thread retired during this observation
            if int(fields.get("TracerPid", "0")) != 0 and fields.get("State", "").strip().startswith(("t", "T")):
                return True
        return False
    except (OSError, ValueError):
        return False


class ModuleProbe:
    """One asynchronous Query process; successful metadata precedes host spawn."""
    def __init__(self, host: Path, module: Path, *, cwd: Path, env: dict[str, str],
                 lease_fd: int, expected: dict | None = None, timeout: float = 5.0):
        self.child = OwnedProcess([str(host), "--inspect-module", str(module)], str(cwd), env,
                                  pass_fds=(lease_fd,))
        self.expected = expected
        self.timeout = timeout
        self.last_poll = time.monotonic()
        self.remaining = timeout
        self.stdout = bytearray()
        self.stderr = bytearray()
        self.metadata: dict | None = None
        self.finished = False
        self.cleanup_confirmed = False
        os.set_blocking(self.child.stdout_fd, False)
        os.set_blocking(self.child.stderr_fd, False)

    def _fail(self, message: str) -> None:
        report = self.child.cleanup(cancelled=True)
        self.finished = True
        self.cleanup_confirmed = report.confirmed
        raise ProbeError(message, cleanup_confirmed=report.confirmed)

    def _drain(self) -> None:
        try:
            for fd, buffer in ((self.child.stdout_fd, self.stdout), (self.child.stderr_fd, self.stderr)):
                for _ in range(17):
                    try:
                        data = os.read(fd, 4096)
                    except (BlockingIOError, InterruptedError):
                        break
                    if not data:
                        break
                    if len(buffer) + len(data) > 64 * 1024:
                        self._fail("module query output exceeds 64 KiB")
                    buffer.extend(data)
        except OSError as exc:
            self._fail(f"module query I/O failed: {exc}")

    def poll(self) -> dict | None:
        if self.finished:
            return self.metadata
        now = time.monotonic()
        if not debugger_stopped(self.child.pid):
            self.remaining -= now - self.last_poll
        self.last_poll = now
        if self.remaining <= 0:
            self._fail("module query timed out; gameplay was not started")
        self._drain()
        observed = self.child.wait_exit_nowait()
        if observed is None:
            return None
        self._drain()  # Include bytes written between the first drain and exit.
        report = self.child.finalize_normal()
        self.finished = True
        self.cleanup_confirmed = report.confirmed
        if not report.confirmed or report.exit_code != 0 or report.signal is not None:
            raise ProbeError(f"module query failed: exit={report.exit_code}, signal={report.signal}",
                             cleanup_confirmed=report.confirmed)
        try:
            self.metadata = validate_metadata(decode(bytes(self.stdout)), self.expected)
        except (ValueError, UnicodeError) as exc:
            raise ProbeError(f"invalid module query: {exc}") from exc
        return self.metadata

    def cancel(self) -> bool:
        if self.finished:
            return self.cleanup_confirmed
        report = self.child.cleanup(cancelled=True)
        self.finished = True
        self.cleanup_confirmed = report.confirmed
        return report.confirmed
