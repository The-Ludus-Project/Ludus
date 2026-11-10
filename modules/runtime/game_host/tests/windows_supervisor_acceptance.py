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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--a", type=Path, required=True)
    parser.add_argument("--b", type=Path, required=True)
    parser.add_argument("--incompatible", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ludus-supervisor-") as directory:
        root = Path(directory)
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
