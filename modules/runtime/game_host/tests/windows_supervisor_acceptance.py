#!/usr/bin/env python3
"""A real Windows GameHost process supervised over an inherited loopback control socket.

Python's socket.socketpair() is a connected loopback TCP pair on Windows. The supervisor
passes one end to ludus_game_host.exe as an inherited SOCKET handle (--control-fd) and
drives the framed protocol: Hello, Load, Status, a committed reload, a rejected reload and
Stop. A second session proves that closing the supervisor end ends the host, and a third
that a killed host ends the supervisor's stream. Generations are distinct copies, since
Windows locks a loaded DLL; every copy must be deletable once the host has exited.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts" / "python"))
from editor_process_windows import OwnedWindowsProcess  # noqa: E402
from play_probe import ModuleProbe, native_identity  # noqa: E402
from play_session import GenerationLease, publish_generation, read_manifest  # noqa: E402

PROJECT_ID = "0000000000000011"
GAME_ID = "0000000000000022"
EPOCH = "0000000000000001"
DEADLINE_SECONDS = 20
FRAME_HEADER = struct.Struct("<I")


def hex_id(value):
    return f"{value:016x}"


class Session:
    def __init__(self, host, module, generation):
        self.events = []
        self.buffer = bytearray()
        self.session = None
        parent, child = socket.socketpair()
        argv = [str(host), "--module", str(module), "--control-fd", str(child.fileno()),
                "--project-id", PROJECT_ID, "--game-id", GAME_ID, "--project-epoch", EPOCH,
                "--generation", hex_id(generation), "--await-load", "--headless"]
        self.control = parent
        try:
            with child:
                self.process = OwnedWindowsProcess(argv, None, dict(os.environ), pass_handles=(child.fileno(),))
        except BaseException:
            self.control.close()
            raise
        try:
            self.control.settimeout(0.05)
            ready = self.await_event(lambda event: event.get("event") == "SessionReady")
            self.session = ready["session"]
        except BaseException:
            self.close()
            raise

    def send(self, request, command, **fields):
        payload = json.dumps({"protocol": 1, "session": self.session, "epoch": EPOCH, "request": hex_id(request),
                              "command": command, **fields}, separators=(",", ":")).encode()
        self.control.sendall(FRAME_HEADER.pack(len(payload)) + payload)

    def pump(self):
        try:
            data = self.control.recv(65536)
        except socket.timeout:
            return True
        except ConnectionError:
            return False
        if not data:
            return False
        self.buffer.extend(data)
        while len(self.buffer) >= FRAME_HEADER.size:
            size, = FRAME_HEADER.unpack_from(self.buffer)
            if len(self.buffer) < FRAME_HEADER.size + size:
                break
            event = json.loads(bytes(self.buffer[FRAME_HEADER.size:FRAME_HEADER.size + size]))
            del self.buffer[:FRAME_HEADER.size + size]
            self.events.append(event)
        return True

    def await_event(self, predicate):
        deadline = time.monotonic() + DEADLINE_SECONDS
        seen = 0
        while time.monotonic() < deadline:
            for event in self.events[seen:]:
                if predicate(event):
                    return event
            seen = len(self.events)
            if not self.pump():
                raise AssertionError(f"control stream ended early: {self.events[-5:]} {self.output()}")
        raise AssertionError(f"event deadline: {self.events[-5:]}")

    def status(self, request):
        return self.await_event(lambda event: event.get("request") == hex_id(request) and "status" in event)

    def has_phase(self, phase):
        return any(event.get("event") == "ReloadPhase" and event.get("phase") == phase for event in self.events)

    def output(self):
        output = []
        for read in (self.process.read_stdout, self.process.read_stderr):
            while True:
                try:
                    chunk = read(65536)
                except BlockingIOError:
                    break
                if not chunk:
                    break
                output.append(chunk)
        return b"".join(output).decode("utf-8", "replace")

    def wait_exit(self):
        deadline = time.monotonic() + DEADLINE_SECONDS
        while time.monotonic() < deadline:
            observed = self.process.wait_exit_nowait()
            if observed is not None:
                return observed[0]
            time.sleep(0.01)
        raise AssertionError(f"host exit deadline: {self.output()}")

    def close(self):
        self.control.close()
        assert self.process.cleanup(cancelled=True).confirmed, "game host job did not empty"


def check_reload_session(host, generation_a, generation_b, incompatible):
    session = Session(host, generation_a, 1)
    try:
        session.send(1, "Hello")
        assert session.status(1)["status"] == "Ok"
        session.send(2, "Load", generation=hex_id(1))
        loaded = session.status(2)
        assert loaded["status"] == "Ok", loaded
        assert any(event.get("event") == "ModuleReady" for event in session.events)
        session.send(3, "Status")
        assert session.status(3)["status"] == "Ok"
        session.send(4, "Reload", expected_generation=hex_id(1), generation=hex_id(2), module_path=str(generation_b))
        reloaded = session.status(4)
        assert reloaded["status"] == "Ok" and session.has_phase("Done"), reloaded
        session.send(5, "Reload", expected_generation=hex_id(2), generation=hex_id(3), module_path=str(incompatible))
        session.status(5)
        assert session.has_phase("Rejected"), session.events[-5:]
        session.send(6, "Status")
        assert session.status(6)["status"] == "Ok"
        session.send(7, "Stop")
        session.await_event(lambda event: event.get("event") == "SessionEnded")
        assert session.wait_exit() == 0, session.output()
    finally:
        session.close()


def check_supervisor_close_ends_host(host, module):
    session = Session(host, module, 1)
    try:
        session.send(1, "Hello")
        session.status(1)
        session.send(2, "Load", generation=hex_id(1))
        assert session.status(2)["status"] == "Ok"
        # End of stream cancels the session (design 10); the host must not linger.
        session.control.close()
        session.wait_exit()
    finally:
        session.close()


def check_killed_host_ends_stream(host, module):
    session = Session(host, module, 1)
    try:
        session.process.request_terminate()
        session.wait_exit()
        deadline = time.monotonic() + DEADLINE_SECONDS
        while session.pump():
            assert time.monotonic() < deadline, "supervisor stream outlived its host"
    finally:
        session.close()


def check_published_generation(root, host, module):
    def inspect_staged(staging, module_name, host_name, lease_fd):
        identities = [native_identity(staging / name) for name in (module_name, host_name)]
        assert identities[0]["machine"] == identities[1]["machine"]
        probe = ModuleProbe(staging / host_name, staging / module_name,
                            cwd=staging, env=dict(os.environ), lease_fd=lease_fd)
        try:
            metadata = probe.poll()
            while metadata is None:
                time.sleep(0.01)
                metadata = probe.poll()
            return {**metadata, "module_build_id": identities[0]["build_id"],
                    "host_build_id": identities[1]["build_id"], "embedded_symbols": False}
        finally:
            assert probe.cancel(), "generation probe cleanup was not confirmed"

    generation = publish_generation(
        generations_root=root / "generations", module_artifact=module, host_artifact=host,
        symbol_artifact=module.with_suffix(".pdb"), host_symbol_artifact=host.with_suffix(".pdb"),
        manifest_fields=dict(project_id=PROJECT_ID, game_id=GAME_ID, module_target="fixture",
                             host_target="host", build_request_revision="windows-acceptance"),
        declared_source_inputs=[module], validate_payloads=inspect_staged)
    with GenerationLease(generation):
        manifest = read_manifest(generation)
        session = Session(generation / manifest["host_file"], generation / manifest["module_file"], 1)
        try:
            session.send(1, "Hello")
            assert session.status(1)["status"] == "Ok"
            session.send(2, "Load", generation=hex_id(1))
            assert session.status(2)["status"] == "Ok"
            session.send(3, "Stop")
            session.await_event(lambda event: event.get("event") == "SessionEnded")
            assert session.wait_exit() == 0, session.output()
        finally:
            session.close()


def check_script_debug_session(host, module, replacement, evidence_path):
    """Drive the installed SDK sample through the Windows host and the real Luau VM."""
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    program = next(item for item in evidence["programs"] if item["path"] == "encounter.luau")
    # Line 4 in the source changes state. The cooker replaces the strict line with its contract prelude.
    authored_line = 4
    compiled_line = program["first_line"] + authored_line - 2
    asset = program["asset"]
    session = Session(host, module, 1)
    generation = hex_id(1)

    def require_status(request, expected="Ok"):
        result = session.status(request)
        if result["status"] != expected:
            raise RuntimeError(f"Expected {expected} for request {request}: {result}")
        return result

    def debug(request, action, expected="Ok", **fields):
        payload = json.dumps({"version": 1, "action": action, **fields}, separators=(",", ":"))
        session.send(request, "ScriptDebug", expected_generation=generation, extension=1, payload=payload)
        result = require_status(request, expected)
        return json.loads(result["message"]) if expected == "Ok" else None

    def identity(snapshot):
        return {field: snapshot[field] for field in ("session", "execution", "stop")}

    try:
        session.send(1, "Hello")
        require_status(1)
        session.send(2, "Load", generation=hex_id(1))
        require_status(2)
        initial = debug(3, "inspect")
        if (initial["provider"] != "luau" or initial["package"] != evidence["key"] or initial["partial"]
                or initial["interactions"] != 0):
            raise RuntimeError(f"Unexpected initial Luau state: {initial}")
        debug(4, "breakpoint", asset=asset, line=compiled_line, enabled=True, **identity(initial))
        stopped = debug(5, "interact", asset=asset, amount=1, **identity(initial))
        if not stopped["partial"] or stopped["interactions"] != 0 or not stopped["frames"]:
            raise RuntimeError(f"The Luau breakpoint did not preserve the unpublished tick: {stopped}")
        stepped = debug(6, "into", **identity(stopped))
        if not stepped["partial"] or stepped["stop"] == stopped["stop"] or not stepped["frames"]:
            raise RuntimeError(f"The Luau step did not reach a new stop: {stepped}")
        debug(7, "continue", expected="InvalidRequest", **identity(stopped))
        finished = debug(8, "continue", **identity(stepped))
        if finished["partial"] or finished["interactions"] != 1:
            raise RuntimeError(f"The resumed Luau tick did not commit exactly once: {finished}")
        session.send(9, "Reload", expected_generation=generation, generation=hex_id(2),
                     module_path=str(replacement))
        generation = require_status(9)["generation"]
        if generation != hex_id(2):
            raise RuntimeError(f"Expected the second scripted module, got {generation}")
        reloaded = debug(10, "inspect")
        if reloaded["package"] == finished["package"] or reloaded["interactions"] != 1:
            raise RuntimeError(f"Reload did not activate new script code and retain state: {reloaded}")
        edited = debug(11, "interact", asset=asset, amount=1, **identity(reloaded))
        if edited["partial"] or edited["interactions"] != 3:
            raise RuntimeError(f"Reloaded script did not apply the edited increment: {edited}")
        session.send(12, "Stop")
        session.await_event(lambda event: event.get("event") == "SessionEnded")
        if session.wait_exit() != 0:
            raise RuntimeError(f"Scripted GameHost failed to stop: {session.output()}")
    finally:
        session.close()
    print("Installed Luau breakpoint, step, stale stop, resume and scripted DLL reload passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--a", type=Path)
    parser.add_argument("--b", type=Path)
    parser.add_argument("--incompatible", type=Path)
    parser.add_argument("--scripted-module", type=Path)
    parser.add_argument("--scripted-replacement", type=Path)
    parser.add_argument("--script-evidence", type=Path)
    args = parser.parse_args()
    scripted = (args.scripted_module, args.scripted_replacement, args.script_evidence)
    if any(scripted):
        if not all(scripted):
            parser.error("provide --scripted-module, --scripted-replacement and --script-evidence together")
        check_script_debug_session(args.host, *scripted)
        return
    if not all((args.a, args.b, args.incompatible)):
        parser.error("provide --a, --b and --incompatible, or --scripted-module")
    with tempfile.TemporaryDirectory(prefix="ludus-supervisor-") as directory:
        root = Path(directory)
        check_published_generation(root, args.host, args.a)
        generations = []
        for index, source in enumerate((args.a, args.b, args.incompatible, args.a, args.a), start=1):
            destination = root / f"generation-{index}.dll"
            shutil.copyfile(source, destination)
            generations.append(destination)
        check_reload_session(args.host, *generations[:3])
        check_supervisor_close_ends_host(args.host, generations[3])
        check_killed_host_ends_stream(args.host, generations[4])
        # Exited hosts hold no generation: every copy can be removed.
        for generation in generations:
            generation.unlink()
    print("Inherited-socket Hello/Load/Status/reload/reject/Stop, supervisor close and host kill passed.")


if __name__ == "__main__":
    main()
