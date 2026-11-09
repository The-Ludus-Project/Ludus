"""Persistent Qt-free owner of one game host and its immutable generations.

Builds use editor_tool's separate build_generation process. This actor owns no
build-tree lock. All input, host control and frontend output are nonblocking and
bounded. Native hangs produce diagnostics; only explicit Stop/parent loss tears
down the owned process group. No heartbeat kills a debugger-stopped game.
"""
from __future__ import annotations
import argparse
from collections import deque
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import struct
import sys
import time

from editor_tool import OwnedProcess, ProtocolError, _make_context, build_plan
from ludus_tools.descriptor import parse_descriptor_bytes
from ludus_tools.errors import ToolingError
from play_build import project_identity, source_inputs
from play_assets import publish_frame_clear
from play_documents import DocumentError, TuningDocument, decode, read_bounded, read_play_descriptor, digest
from play_probe import ModuleProbe, ProbeError, native_identity, debugger_stopped
from play_session import (GenerationLease, GenerationStoreLease, PublishError, read_manifest,
                          collect_generations, LeaseSet, source_input_digest)

MAX_QUEUE = 1024*1024
MAX_LINE = 256*1024


def hex_id(value):
    if not isinstance(value, str) or len(value) != 16 or any(c not in "0123456789abcdef" for c in value):
        raise ProtocolError("expected a lowercase 16-character hex ID")
    return value


def encode(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)+"\n").encode()


class OutputQueue:
    """Whole log records may drop; critical records have a separate reserve."""
    def __init__(self, fd):
        self.fd = fd
        self.frames = deque()
        self.bytes = 0
        self.dropped = 0
        os.set_blocking(fd, False)

    def emit(self, value, *, log=False, terminal=False):
        value = {"protocol":1, **value}
        data = encode(value)
        bound = MAX_QUEUE if terminal else MAX_QUEUE-65536 if not log else MAX_QUEUE//2
        if len(data) > MAX_LINE or self.bytes+len(data) > bound:
            if log:
                self.dropped += 1
                return
            raise ProtocolError("frontend output backpressure exceeded the bounded queue")
        self.frames.append(data)
        self.bytes += len(data)

    def drain(self):
        for _ in range(64):
            if not self.frames:
                break
            try:
                count = os.write(self.fd, self.frames[0])
            except (BlockingIOError, InterruptedError):
                break
            if count <= 0:
                raise BrokenPipeError("frontend pipe closed")
            self.bytes -= count
            if count == len(self.frames[0]):
                self.frames.popleft()
            else:
                self.frames[0] = self.frames[0][count:]
                break


class PlaySupervisor:
    def __init__(self, context, in_fd, out_fd):
        self.context = context
        self.in_fd = in_fd
        self.output = OutputQueue(out_fd)
        os.set_blocking(in_fd, False)
        self.input = bytearray()
        self.host_input = bytearray()
        self.host_frames = deque()
        self.host_bytes = 0
        self.child = None
        self.control = None
        self.probe = None
        self.candidate = None
        self.active = None
        self.previous = None
        self.leases = {}
        self.store_lease = None
        self.session = None
        self.epoch = "0000000000000000"
        self.project = None
        self.plan = None
        self.tuning_document = None
        self.state = "Stopped"
        self.pending = {}  # frontend requests/outcomes remain until explicit ack
        self.highest_request = 0
        self.internal_request = 2**63
        self.closing = False
        self.signal_stop = False
        self.stop_deadline = None
        self.last_host_event = time.monotonic()
        self.hang_reported = False
        self.debugger_paused = False
        self.heartbeat_request = None
        self.hello_request = None
        self.load_request = None
        self.next_heartbeat = time.monotonic()+1
        self.stop_request = None
        self.exit_after_flush = False
        self.flush_deadline = None
        self.cleanup_confirmed = True
        self.output.emit({"type":"ready"})

    def emit(self, kind, **fields):
        self.output.emit({"type":kind, "epoch":self.epoch, **fields})

    def result(self, request, status, message="", *, retain=True, **extra):
        value = {"type":"result", "epoch":self.epoch, "request":request,
                 "status":status, "message":message[:256], **extra}
        pending = self.pending.get(request)
        if pending is not None and retain:
            pending["result"] = value
        self.output.emit(value, terminal=status == "CleanupUnknown")

    def internal(self, command, **fields):
        self.internal_request += 1
        request = f"{self.internal_request:016x}"
        self.send_host({"protocol":1, "session":self.session, "epoch":self.epoch,
                        "request":request, "command":command, **fields}, priority=command == "Stop")
        return request

    def send_host(self, command, *, priority=False):
        payload = encode(command)[:-1]
        data = struct.pack("<I", len(payload)) + payload
        if len(data) > MAX_LINE or self.host_bytes+len(data) > MAX_LINE or len(self.host_frames) >= 128:
            raise ProtocolError("host command queue is full")
        # Do not interleave a Stop into a partially sent JSON line.
        if priority and self.host_frames:
            self.host_frames.insert(1, data)
        elif priority:
            self.host_frames.appendleft(data)
        else:
            self.host_frames.append(data)
        self.host_bytes += len(data)

    def verify_generation(self, path):
        if not isinstance(path, str) or len(path.encode()) > 4096:
            raise ProtocolError("invalid generation path")
        generation = Path(path)
        expected = self.project.parent / ".ludus/generations" / self.plan.descriptor.preset
        if generation.is_symlink() or generation.parent.resolve() != expected.resolve():
            raise ProtocolError("generation is outside this project's immutable store")
        lease = GenerationLease(generation)
        try:
            manifest = read_manifest(generation)
            sidecar = read_play_descriptor(self.project.parent)
            if manifest["project_id"] != project_identity(self.project) or sidecar is None or \
                    manifest["module_target"] != sidecar.module_target or manifest["host_target"] != sidecar.host_target:
                raise ProtocolError("generation/project targets changed; build again")
            if sidecar.startup_document is not None and \
                    TuningDocument(sidecar.startup_document).saved["game"] != manifest["game_id"]:
                raise ProtocolError("startup tuning document game identity changed; build again")
            inputs = source_inputs(self.project.parent, self.plan.source_dir, sidecar, self.plan.build_dir)
            if source_input_digest(inputs) != manifest["source_input_digest"]:
                raise ProtocolError("project source inputs changed after this generation was published")
            for key, file in (("module_build_id", "module_file"), ("host_build_id", "host_file")):
                symbols = manifest["symbol_file" if file == "module_file" else "host_symbol_file"]
                identity = native_identity(generation / manifest[file], generation / symbols if symbols else None)
                if identity["build_id"] != manifest[key]:
                    raise ProtocolError("native build ID disagrees with manifest")
            if self.active is not None and manifest["sdk_identity"] != self.active["manifest"]["sdk_identity"]:
                raise ProtocolError("SDK identity changed; stop and restart instead of reload")
            if self.plan.runtime_identity is not None and manifest["sdk_identity"] != self.plan.runtime_identity:
                raise ProtocolError("generation disagrees with the resolved SDK; build again")
            return dict(path=generation, manifest=manifest, lease=lease,
                        generation=generation.name[:16])
        except BaseException:
            lease.close()
            raise

    def begin_probe(self, candidate, request, kind):
        if self.probe is not None:
            raise ProtocolError("another candidate query is in progress")
        manifest = candidate["manifest"]
        try:
            self.probe = ModuleProbe(candidate["path"] / manifest["host_file"],
                                     candidate["path"] / manifest["module_file"], cwd=self.plan.run_cwd,
                                     env=self.plan.env, lease_fd=candidate["lease"].fileno(), expected=manifest)
        except BaseException:
            candidate["lease"].close()
            raise
        candidate["request"] = request
        candidate["kind"] = kind
        self.candidate = candidate
        self.leases[candidate["path"].name] = candidate["lease"]
        self.emit("phase", phase="Query", request=request)

    def start(self, message, request):
        if self.state != "Stopped" or self.child is not None or self.probe is not None or not self.cleanup_confirmed:
            self.result(request, "Busy", "stop and confirm cleanup before Play")
            return
        project = Path(message["project"])
        if not project.is_absolute():
            raise ProtocolError("project descriptor path must be absolute")
        data = read_bounded(project)
        if digest(data) != message["expected_sha256"]:
            self.result(request, "Conflict", "saved project descriptor changed")
            return
        descriptor = parse_descriptor_bytes(data)
        self.project = project
        self.plan = build_plan(self.context, descriptor, project.parent, descriptor_path=project)
        sidecar = read_play_descriptor(project.parent)
        self.tuning_document = TuningDocument(sidecar.startup_document) if sidecar and sidecar.startup_document else None
        reserved = {"--module", "--inspect-module", "--control-fd", "--project-id", "--game-id", "--project-epoch",
                    "--generation", "--authored-document", "--await-load", "--identity"}
        if any(arg.split("=", 1)[0] in reserved for arg in descriptor.run_args):
            raise ProtocolError("Play run arguments override a reserved host control/identity option")
        self.epoch = hex_id(message["epoch"])
        if int(self.epoch, 16) == 0:
            raise ProtocolError("project epoch must be nonzero")
        self.highest_request = int(request, 16)
        self.state = "Starting"
        if self.tuning_document is not None:
            self.emit("document", available=True, dirty=self.tuning_document.dirty,
                      digest=digest(self.tuning_document.encoded()), can_undo=self.tuning_document.can_undo,
                      can_redo=self.tuning_document.can_redo)
        else:
            self.emit("document", available=False, dirty=False, digest="", can_undo=False, can_redo=False)
        try:
            self.begin_probe(self.verify_generation(message["generation_path"]), request, "start")
        except BaseException:
            self.state = "Stopped"
            raise

    def read_frontend(self):
        for _ in range(64):
            try:
                data = os.read(self.in_fd, 4096)
            except (BlockingIOError, InterruptedError):
                break
            if not data:
                self.closing = True
                self.stop(None)
                break
            self.input.extend(data)
            if len(self.input) > MAX_LINE:
                raise ProtocolError("frontend line exceeds 256 KiB")
            while b"\n" in self.input:
                line, _, tail = self.input.partition(b"\n")
                self.input = bytearray(tail)
                self.command(decode(bytes(line)))

    def command(self, message):
        if type(message.get("protocol")) is not int or message["protocol"] != 1:
            raise ProtocolError("unsupported frontend protocol")
        kind = message.get("type")
        if kind == "close":
            if set(message) != {"protocol", "type"}:
                raise ProtocolError("unknown close fields")
            self.closing = True
            self.stop(None)
            return
        if kind == "ack":
            if set(message) != {"protocol", "type", "epoch", "request"} or message["epoch"] != self.epoch:
                raise ProtocolError("invalid outcome acknowledgement")
            request = hex_id(message["request"])
            retained = self.pending.get(request)
            if retained is not None and retained["result"] is not None:
                if self.child is not None and self.session is not None:
                    self.internal("Ack", acknowledge=request)
                del self.pending[request]
            return
        allowed = {"start": {"project", "expected_sha256", "generation_path"},
                   "asset": {"source", "expected_generation"},
                   "reload": {"generation_path", "expected_generation"},
                   "command": {"command"}}.get(kind)
        if allowed is None:
            raise ProtocolError("unsupported frontend command")
        common = {"protocol", "type", "epoch", "request"}
        if set(message) != common | allowed:
            raise ProtocolError("unknown or incomplete frontend command fields")
        request = hex_id(message["request"])
        number = int(request, 16)
        if not 0 < number < 2**63:
            raise ProtocolError("frontend request ID outside its namespace")
        is_stop = kind == "command" and isinstance(message["command"], dict) and message["command"].get("command") == "Stop"
        if kind == "start" and self.state == "Stopped" and self.child is None and self.probe is None and self.cleanup_confirmed:
            epoch = hex_id(message["epoch"])
            if int(epoch, 16) == 0:
                raise ProtocolError("project epoch must be nonzero")
            if epoch != self.epoch:
                self.pending.clear()
                self.highest_request = 0
            self.epoch = epoch
        if kind != "start" and message["epoch"] != self.epoch:
            self.result(request, "StaleEpoch", "project epoch changed", retain=False)
            return
        payload = encode(message)
        retained = self.pending.get(request)
        if retained is not None:
            if retained["payload"] != payload:
                self.result(request, "InvalidRequest", "request ID payload mismatch", retain=False)
            elif retained["result"] is not None:
                self.output.emit(retained["result"])
            return
        retained_bytes = sum(len(entry["payload"]) + (len(encode(entry["result"])) if entry["result"] else 0)
                             for entry in self.pending.values())
        if (number <= self.highest_request or len(self.pending) >= 128 or retained_bytes+len(payload) > MAX_QUEUE-65536) and not is_stop:
            self.result(request, "Busy", "old outcome or full ledger; acknowledge/refresh Status")
            return
        self.highest_request = max(number, self.highest_request)
        if not is_stop:
            self.pending[request] = dict(payload=payload, result=None)
        try:
            if kind == "start":
                self.start(message, request)
            elif is_stop:
                self.stop_request = request
                self.stop(request)
            elif kind == "asset":
                if self.state not in ("Running", "Paused") or self.active is None or self.probe is not None:
                    self.result(request, "Busy", "configuration import requires an active session")
                    return
                if hex_id(message["expected_generation"]) != self.active["generation"]:
                    self.result(request, "SchemaChanged", "generation changed; refresh")
                    return
                cooked = publish_frame_clear(self.project.parent, message["source"])
                self.emit("asset", request=request, source=message["source"],
                          source_sha256=cooked.pop("source_sha256"), cooked_sha256=cooked.pop("cooked_sha256"))
                self.send_host(dict(command="ReloadAsset", protocol=1, session=self.session,
                                    epoch=self.epoch, request=request,
                                    expected_generation=message["expected_generation"], **cooked))
            elif kind == "reload":
                if self.state not in ("Running", "Paused") or self.probe is not None:
                    self.result(request, "Busy", "reload requires an active session with no pending query")
                    return
                if self.child is not None and debugger_stopped(self.child.pid):
                    self.result(request, "Busy", "debugger stopped in the host; continue to a safe boundary before reload")
                    return
                if hex_id(message["expected_generation"]) != self.active["generation"]:
                    self.result(request, "SchemaChanged", "generation changed; refresh")
                    return
                candidate = self.verify_generation(message["generation_path"])
                if int(candidate["generation"], 16) <= int(self.active["generation"], 16):
                    candidate["lease"].close()
                    self.result(request, "Superseded", "candidate is not newer than active generation")
                    return
                self.begin_probe(candidate, request, "reload")
            else:
                command = message["command"]
                if not isinstance(command, dict) or any(key in command for key in ("protocol", "request", "session", "epoch")):
                    raise ProtocolError("invalid host command envelope")
                document_command = command.get("command")
                if document_command in ("ApplyToDocument", "UndoDocument", "RedoDocument", "SaveDocument", "DiscardDocument"):
                    self.document_command(request, command)
                    return
                if self.child is None or self.session is None:
                    self.result(request, "Busy", "game host is not ready")
                    return
                if command.get("command") not in (
                    "Hello", "Status", "Pause", "Resume", "Step", "SetFastForward",
                    "ReadProperties", "ApplyEdits", "ReloadAsset", "ScriptDebug",
                ):
                    raise ProtocolError("unsupported host command")
                self.send_host({**command, "protocol":1, "session":self.session,
                                "epoch":self.epoch, "request":request})
        except (ValueError, OSError, ProtocolError, PublishError, ToolingError, DocumentError) as exc:
            self.result(request, "InvalidRequest", str(exc))

    def document_command(self, request, command):
        if self.tuning_document is None:
            self.result(request, "NotFound", "project has no startup tuning document")
            return
        action = command["command"]
        try:
            if action == "ApplyToDocument":
                if set(command) != {"command", "expected_digest", "edits"}:
                    raise ProtocolError("invalid ApplyToDocument fields")
                self.tuning_document.apply(command["edits"], expected_digest=command["expected_digest"])
            elif action == "UndoDocument":
                if set(command) != {"command"}:
                    raise ProtocolError("invalid UndoDocument fields")
                if not self.tuning_document.undo():
                    self.result(request, "NotFound", "no tuning document edit to undo")
                    return
            elif action == "RedoDocument":
                if set(command) != {"command"}:
                    raise ProtocolError("invalid RedoDocument fields")
                if not self.tuning_document.redo():
                    self.result(request, "NotFound", "no tuning document edit to redo")
                    return
            elif action == "SaveDocument":
                if set(command) != {"command"}:
                    raise ProtocolError("invalid SaveDocument fields")
                self.tuning_document.save()
            elif action == "DiscardDocument":
                if set(command) != {"command"}:
                    raise ProtocolError("invalid DiscardDocument fields")
                self.tuning_document.discard()
            self.result(request, "Ok", action, document_dirty=self.tuning_document.dirty,
                        document_digest=digest(self.tuning_document.encoded()),
                        can_undo=self.tuning_document.can_undo, can_redo=self.tuning_document.can_redo)
        except DocumentError as exc:
            status = "Conflict" if "changed on disk" in str(exc) or "draft changed" in str(exc) else "InvalidRequest"
            self.result(request, status, str(exc), document_dirty=self.tuning_document.dirty,
                        document_digest=digest(self.tuning_document.encoded()),
                        can_undo=self.tuning_document.can_undo, can_redo=self.tuning_document.can_redo)

    def poll_probe(self):
        if self.probe is None:
            return
        try:
            metadata = self.probe.poll()
        except ProbeError as exc:
            candidate = self.candidate
            self.probe = None
            self.candidate = None
            if exc.cleanup_confirmed:
                self.leases.pop(candidate["path"].name).close()
            else:
                self.cleanup_confirmed = False
                self.state = "CleanupUnknown"
            if candidate["kind"] == "start" and self.cleanup_confirmed:
                self.state = "Stopped"
            self.result(candidate["request"], "GenerationInvalid" if exc.cleanup_confirmed else "CleanupUnknown", str(exc))
            return
        if metadata is None:
            return
        self.probe = None
        candidate = self.candidate
        if candidate["kind"] == "start":
            try:
                self.spawn(candidate)
            except OSError as exc:
                self.leases.pop(candidate["path"].name).close()
                if self.store_lease is not None:
                    self.store_lease.close()
                    self.store_lease = None
                self.candidate = None
                self.state = "Stopped"
                self.result(candidate["request"], "SpawnFailed", str(exc))
        else:
            # A debugger may have stopped A while the separate Query ran.
            # Do not leave a reload queued to unload it on a later Continue.
            if self.child is not None and debugger_stopped(self.child.pid):
                self.leases.pop(candidate["path"].name).close()
                self.candidate = None
                self.result(candidate["request"], "Busy", "debugger stopped during candidate query; continue and reload again")
                return
            self.send_host({"protocol":1, "session":self.session, "epoch":self.epoch,
                            "request":candidate["request"], "command":"Reload",
                            "expected_generation":self.active["generation"],
                            "generation":candidate["generation"],
                            "module_path":str(candidate["path"] / candidate["manifest"]["module_file"])})
            self.emit("phase", phase="Reload", request=candidate["request"])

    def spawn(self, candidate):
        parent, child = socket.socketpair()
        try:
            self.store_lease = GenerationStoreLease(candidate["path"].parent)
            manifest = candidate["manifest"]
            argv = [str(candidate["path"] / manifest["host_file"]), "--module",
                    str(candidate["path"] / manifest["module_file"]), "--control-fd", str(child.fileno()),
                    "--project-id", manifest["project_id"], "--game-id", manifest["game_id"],
                    "--project-epoch", self.epoch, "--generation", candidate["generation"], "--await-load",
                    *self.plan.descriptor.run_args]
            if manifest["authored_file"] is not None:
                argv.extend(["--authored-document", str(candidate["path"] / manifest["authored_file"])])
            self.child = OwnedProcess(argv, str(self.plan.run_cwd), self.plan.env,
                                      pass_fds=(child.fileno(), candidate["lease"].fileno(), self.store_lease.fileno()))
            self.control = parent
            parent.setblocking(False)
            os.set_blocking(self.child.stdout_fd, False)
            os.set_blocking(self.child.stderr_fd, False)
            self.active = candidate
            self.emit("host_started", pid=self.child.pid, argv=argv, cwd=str(self.plan.run_cwd),
                      generation=candidate["generation"], manifest=manifest)
            self.last_host_event = time.monotonic()
            self.hang_reported = False
        except BaseException:
            parent.close()
            raise
        finally:
            child.close()

    def pump_host(self):
        if self.child is None:
            return
        stopped = debugger_stopped(self.child.pid)
        if stopped != self.debugger_paused:
            self.debugger_paused = stopped
            self.emit("debugger_state", stopped=stopped, pid=self.child.pid)
        for fd, stream in ((self.child.stdout_fd, "stdout"), (self.child.stderr_fd, "stderr")):
            for _ in range(16):
                try:
                    data = os.read(fd, 4096)
                except (BlockingIOError, InterruptedError):
                    break
                if not data:
                    break
                self.output.emit({"type":"output", "epoch":self.epoch, "stream":stream,
                                  "text":data.decode("utf-8", "replace")}, log=True)
        if self.control is not None:
            for _ in range(64):
                if not self.host_frames:
                    break
                try:
                    count = self.control.send(self.host_frames[0])
                except (BlockingIOError, InterruptedError):
                    break
                except (BrokenPipeError, ConnectionResetError):
                    self.control.close()
                    self.control = None
                    self.host_frames.clear()
                    self.host_bytes = 0
                    break
                if count <= 0:
                    break
                self.host_bytes -= count
                if count == len(self.host_frames[0]):
                    self.host_frames.popleft()
                else:
                    self.host_frames[0] = self.host_frames[0][count:]
                    break
            for _ in range(64):
                if self.control is None:
                    break
                try:
                    data = self.control.recv(4096)
                except (BlockingIOError, InterruptedError):
                    break
                except ConnectionResetError:
                    data = b""
                if not data:
                    self.control.close()
                    self.control = None
                    break
                self.host_input.extend(data)
                if len(self.host_input) > MAX_LINE:
                    raise ProtocolError("host line exceeds 256 KiB")
                while len(self.host_input) >= 4:
                    length = struct.unpack_from("<I", self.host_input)[0]
                    if not 0 < length <= MAX_LINE-4:
                        raise ProtocolError("invalid host frame length")
                    if len(self.host_input) < length+4:
                        break
                    payload = bytes(self.host_input[4:length+4])
                    del self.host_input[:length+4]
                    self.host_event(decode(payload))
        observed = self.child.wait_exit_nowait()
        if observed is not None:
            self.finish_host(self.child.finalize_normal())
        elif self.stop_deadline is not None and time.monotonic() >= self.stop_deadline:
            self.finish_host(self.child.cleanup(cancelled=True))
        elif time.monotonic()-self.last_host_event > 5 and not self.hang_reported:
            if not stopped:
                self.emit("diagnostic", code="HostUnresponsive", message="No host event for 5 seconds; inspect debugger or use Stop/Restart")
                self.hang_reported = True
        if self.child is not None and self.control is not None and self.session is not None and \
                self.stop_deadline is None and self.heartbeat_request is None and time.monotonic() >= self.next_heartbeat:
            self.heartbeat_request = self.internal("Status")
            self.next_heartbeat = time.monotonic()+1

    def host_event(self, event):
        if event.get("protocol") != 1 or event.get("epoch") != self.epoch:
            raise ProtocolError("host event protocol/epoch mismatch")
        self.last_host_event = time.monotonic()
        self.hang_reported = False
        kind = event.get("event")
        if kind == "SessionReady":
            if self.session is not None:
                raise ProtocolError("duplicate host ready event")
            self.session = hex_id(event["session"])
            if event.get("identity") != self.active["manifest"]["sdk_identity"]:
                raise ProtocolError("host identity disagrees with generation manifest")
            self.hello_request = self.internal("Hello")
        elif self.session is None or event.get("session") != self.session:
            raise ProtocolError("host event session mismatch")
        elif kind == "ModuleReady" and self.candidate is not None and self.candidate["kind"] == "start":
            if event.get("generation") != self.active["generation"]:
                raise ProtocolError("host loaded unexpected initial generation")
            self.state = "Running"
            self.result(self.candidate["request"], "Ok", "game host ready")
            self.candidate = None
        elif kind == "CommandResult":
            request = hex_id(event["request"])
            if int(request, 16) >= 2**63:
                if request == self.hello_request:
                    self.hello_request = None
                    if event.get("status") != "Ok" or event.get("identity") != self.active["manifest"]["sdk_identity"]:
                        raise ProtocolError("host Hello rejected or changed identity")
                    self.load_request = self.internal("Load", generation=self.active["generation"])
                elif request == self.load_request:
                    self.load_request = None
                    if event.get("status") != "Ok" and self.candidate is not None:
                        self.result(self.candidate["request"], event.get("status", "InvalidRequest"), "host initial Load rejected")
                        self.stop(None)
                if request == self.heartbeat_request:
                    self.heartbeat_request = None
                return
            if self.candidate is not None and request == self.candidate["request"]:
                # Generation is the commit outcome. Retirement can require a
                # restart AFTER B became active; retain B's identity/lease then.
                if event.get("generation") == self.candidate["generation"] and \
                        (event.get("status") == "Ok" or event.get("state") == "CleanupUnknown"):
                    if self.previous is not None:
                        self.leases.pop(self.previous["path"].name).close()
                    self.previous = self.active
                    self.active = self.candidate
                elif event.get("status") != "RestartRequired":
                    self.leases.pop(self.candidate["path"].name).close()
                self.candidate = None
            self.state = event.get("state", self.state)
            self.result(request, event.get("status", "InvalidRequest"), event.get("message", ""), host=event)
        self.emit("host", event=event)

    def stop(self, request):
        if self.probe is not None:
            candidate = self.candidate
            confirmed = self.probe.cancel()
            self.probe = None
            self.candidate = None
            self.cleanup_confirmed &= confirmed
            if confirmed:
                self.leases.pop(candidate["path"].name).close()
            self.result(candidate["request"], "Cancelled" if confirmed else "CleanupUnknown", "candidate query stopped")
        if self.child is None:
            self.state = "Stopped" if self.cleanup_confirmed else "CleanupUnknown"
            if request is not None:
                self.result(request, "Ok" if self.cleanup_confirmed else "CleanupUnknown", cleanup_confirmed=self.cleanup_confirmed)
            if self.closing:
                self.emit("ended", reason="Stopped", cleanup_confirmed=self.cleanup_confirmed, state=self.state)
                self.begin_exit()
            return
        self.state = "Stopping"
        if self.stop_deadline is None:
            self.stop_deadline = time.monotonic()+3
            if self.session is not None and self.control is not None:
                self.internal("Stop")
            else:
                self.child.request_terminate()
        self.emit("phase", phase="Stopping")

    def finish_host(self, report):
        # The active host can crash while an independent candidate Query is
        # still running. Retire that owner before clearing its candidate/lease.
        if self.probe is not None:
            self.cleanup_confirmed &= self.probe.cancel()
            self.probe = None
        self.child = None
        if self.control is not None:
            self.control.close()
            self.control = None
        self.cleanup_confirmed &= report.confirmed
        self.state = "Stopped" if self.cleanup_confirmed else "CleanupUnknown"
        if report.confirmed:
            for lease in self.leases.values():
                lease.close()
            self.leases.clear()
            if self.store_lease is not None:
                self.store_lease.close()
                self.store_lease = None
            collect_generations(self.project.parent / ".ludus/generations" / self.plan.descriptor.preset, LeaseSet())
        self.active = self.previous = self.candidate = self.session = None
        self.host_frames.clear()
        self.host_bytes = 0
        self.host_input.clear()
        self.stop_deadline = None
        self.heartbeat_request = None
        self.hello_request = self.load_request = None
        self.emit("ended", reason="HostSignaled" if report.signal is not None else "HostFailed" if report.exit_code else "Stopped",
                  cleanup_confirmed=self.cleanup_confirmed, exit_code=report.exit_code,
                  signal=report.signal, dropped_output=self.output.dropped, state=self.state)
        for request, pending in list(self.pending.items()):
            if pending["result"] is None:
                self.result(request, "OutcomeUnknown", "host exited before a confirmed outcome; refresh/restart explicitly")
        if self.stop_request is not None:
            self.result(self.stop_request, "Ok" if self.cleanup_confirmed else "CleanupUnknown",
                        cleanup_confirmed=self.cleanup_confirmed)
            self.stop_request = None
        if self.closing:
            self.begin_exit()

    def begin_exit(self):
        self.exit_after_flush = True
        self.flush_deadline = time.monotonic()+2

    def run(self):
        selector = selectors.DefaultSelector()
        selector.register(self.in_fd, selectors.EVENT_READ)
        try:
            while True:
                if self.signal_stop and not self.closing:
                    self.closing = True
                    self.stop(None)
                self.output.drain()
                for _ in selector.select(0.01):
                    if not self.closing:
                        self.read_frontend()
                self.poll_probe()
                self.pump_host()
                if self.exit_after_flush and (not self.output.frames or time.monotonic() >= self.flush_deadline):
                    return 0 if self.cleanup_confirmed else 1
        except (ValueError, KeyError, OSError, ProtocolError, PublishError, ToolingError) as exc:
            # Parent loss/malformed input cannot orphan a native process. We
            # report the cleanup truth if the output pipe still accepts it.
            if self.probe is not None:
                self.cleanup_confirmed &= self.probe.cancel()
            if self.child is not None:
                self.cleanup_confirmed &= self.child.cleanup(cancelled=True).confirmed
            if self.cleanup_confirmed:
                for lease in self.leases.values():
                    lease.close()
                if self.store_lease is not None:
                    self.store_lease.close()
            try:
                self.output.emit({"type":"fatal", "epoch":self.epoch, "message":str(exc)[:256],
                                  "cleanup_confirmed":self.cleanup_confirmed}, terminal=True)
                deadline = time.monotonic()+0.2
                while self.output.frames and time.monotonic() < deadline:
                    self.output.drain()
            except (OSError, ProtocolError):
                pass
            return 1
        finally:
            selector.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stdio", action="store_true", required=True)
    parser.add_argument("--tooling-root", type=Path, required=True)
    args = parser.parse_args()
    supervisor = PlaySupervisor(_make_context(args.tooling_root), sys.stdin.fileno(), sys.stdout.fileno())
    def stop_on_signal(_number, _frame):
        supervisor.signal_stop = True
    signal.signal(signal.SIGTERM, stop_on_signal)
    signal.signal(signal.SIGINT, stop_on_signal)
    return supervisor.run()


if __name__ == "__main__":
    sys.exit(main())
