"""Canonical SDK planning, source supersession, output bounds and outcomes."""
from __future__ import annotations
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import cmake_targets
import editor_tool
import engine
from ludus_tools import create, descriptor
from ludus_tools.errors import ToolingError
from play_build import source_inputs
from play_documents import read_play_descriptor
from play_session import PublishError, source_input_digest
from play_tool import OutputQueue, PlaySupervisor
from test_ludus_tools import _manifest_json, _write_sdk_prefix


class ToolingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_post_commit_uncertainty_keeps_candidate_identity_and_symbol_lease(self):
        in_fd, input_write = os.pipe()
        out_read, out_fd = os.pipe()
        for fd in (in_fd, input_write, out_read, out_fd):
            self.addCleanup(os.close, fd)
        supervisor = PlaySupervisor(None, in_fd, out_fd)
        supervisor.epoch = "0000000000000001"
        supervisor.session = "0000000000000002"
        closed = []
        previous = dict(path=Path("A"), generation="0000000000000001")
        candidate = dict(path=Path("B"), generation="0000000000000002", request="0000000000000003")
        supervisor.active, supervisor.candidate = previous, candidate
        supervisor.leases = {name:SimpleNamespace(close=lambda name=name:closed.append(name)) for name in ("A", "B")}
        supervisor.host_event(dict(protocol=1, epoch=supervisor.epoch, session=supervisor.session,
                                   event="CommandResult", request=candidate["request"], generation=candidate["generation"],
                                   state="CleanupUnknown", status="RestartRequired"))
        self.assertIs(candidate, supervisor.active)
        self.assertIs(previous, supervisor.previous)
        self.assertIsNone(supervisor.candidate)
        self.assertEqual("CleanupUnknown", supervisor.state)
        self.assertEqual([], closed)
        self.assertEqual({"A", "B"}, set(supervisor.leases))

    def test_debugger_stop_before_or_during_query_does_not_queue_reload(self):
        in_fd, input_write = os.pipe()
        out_read, out_fd = os.pipe()
        for fd in (in_fd, input_write, out_read, out_fd):
            self.addCleanup(os.close, fd)
        supervisor = PlaySupervisor(None, in_fd, out_fd)
        supervisor.epoch = "0000000000000001"
        supervisor.state = "Paused"
        supervisor.child = SimpleNamespace(pid=123)
        supervisor.active = dict(generation="0000000000000001")
        message = dict(protocol=1, type="reload", epoch=supervisor.epoch,
                       request="0000000000000001", generation_path="unused",
                       expected_generation=supervisor.active["generation"])
        with patch("play_tool.debugger_stopped", return_value=True), \
             patch.object(supervisor, "verify_generation") as verify:
            supervisor.command(message)
            verify.assert_not_called()
            self.assertEqual("Busy", supervisor.pending[message["request"]]["result"]["status"])
            closed = []
            supervisor.candidate = dict(kind="reload", path=Path("B"), request=message["request"])
            supervisor.leases["B"] = SimpleNamespace(close=lambda:closed.append("B"))
            supervisor.probe = SimpleNamespace(poll=lambda:{"validated":True})
            supervisor.poll_probe()
            self.assertEqual(["B"], closed)
            self.assertIsNone(supervisor.candidate)
            self.assertIsNone(supervisor.probe)
            self.assertFalse(supervisor.host_frames)

    def test_v2_uses_canonical_saved_sdk_and_exact_compiler(self):
        sdk = _write_sdk_prefix(self.root / "sdk", _manifest_json())
        project = self.root / "game with spaces"
        create.create_project(project, name="Game", template_id="minimal", engine_version="", local_sdk_prefix=sdk)
        compiler = self.root / "out/host-tools/bin/clang++"
        compiler.parent.mkdir(parents=True)
        compiler.write_text("#!/bin/sh\necho 'clang version 18.1.8'\n")
        compiler.chmod(0o755)
        context = editor_tool.ToolContext(self.root, engine, cmake_targets)
        with patch.object(editor_tool, "_resolve_managed_cmake", return_value="/managed/cmake"):
            plan = editor_tool.build_plan(context, descriptor.parse_descriptor_file(project / "ludus.project.json"), project)
            self.assertEqual(plan.resolution.prefix, sdk.resolve())
            self.assertEqual(plan.env["LUDUS_SDK_PREFIX"], str(sdk.resolve()))
            argv = editor_tool.configure_argv(plan)
            self.assertIn(f"-DCMAKE_CXX_COMPILER={compiler}", argv)
            self.assertEqual(argv[argv.index("-S")+1], str(project.resolve()))
            compiler.write_text("#!/bin/sh\necho 'clang version 19.0.0'\n")
            with self.assertRaisesRegex(ToolingError, "compiler_version"):
                editor_tool.build_plan(context, plan.descriptor, project)
            compiler.write_text("#!/bin/sh\nexit 1\n")
            with self.assertRaisesRegex(ToolingError, "cannot identify"):
                editor_tool.build_plan(context, plan.descriptor, project)

    def test_source_inventory_detects_add_remove_modify_and_excludes_outputs(self):
        project = self.root
        for name in ("ludus.project.json", "CMakeLists.txt", "CMakePresets.json"):
            (project / name).write_text("{}")
        (project / "src").mkdir()
        source = project / "src/game.cpp"
        source.write_text("A")
        (project / "ludus.play.json").write_text(json.dumps(dict(version=1, host_target="host", module_target="game", watch_roots=["src"])))
        sidecar = read_play_descriptor(project)
        build = project / "out/build/native"
        build.mkdir(parents=True)
        first = source_inputs(project, project, sidecar, build)
        before = source_input_digest(first)
        (build / "generated.cpp").write_text("generated")
        self.assertEqual(first, source_inputs(project, project, sidecar, build))
        source.write_text("B")
        self.assertNotEqual(before, source_input_digest(first))
        added = project / "src/new.cpp"
        added.write_text("C")
        self.assertNotEqual(first, source_inputs(project, project, sidecar, build))
        source.unlink()
        with self.assertRaises(PublishError):
            source_input_digest(first)
        (project / "src/link.cpp").symlink_to(project / "CMakeLists.txt")
        with self.assertRaisesRegex(PublishError, "nonregular"):
            source_inputs(project, project, sidecar, build)

    def test_output_short_write_preserves_whole_frame_and_log_drop(self):
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        queue = OutputQueue(write_fd)
        queue.emit(dict(type="output", text="é"*1024), log=True)
        original = queue.frames[0]
        with patch("play_tool.os.write", return_value=7):
            queue.drain()
        self.assertEqual(queue.frames[0], original[7:])
        self.assertEqual(queue.bytes, len(original)-7)
        for _ in range(1000):
            queue.emit(dict(type="output", text="x"*4096), log=True)
        self.assertGreater(queue.dropped, 0)
        self.assertLessEqual(queue.bytes, 512*1024)
        queue.emit(dict(type="ended", cleanup_confirmed=True), terminal=True)
        self.assertEqual(json.loads(queue.frames[-1])["type"], "ended")

    def test_fast_forward_is_forwarded_with_the_current_session_envelope(self):
        in_fd, input_write = os.pipe()
        out_read, out_fd = os.pipe()
        for fd in (in_fd, input_write, out_read, out_fd):
            self.addCleanup(os.close, fd)
        supervisor = PlaySupervisor(None, in_fd, out_fd)
        supervisor.epoch = "0000000000000001"
        supervisor.session = "0000000000000002"
        supervisor.child = object()
        request = "0000000000000003"
        command = dict(command="SetFastForward", expected_generation="0000000000000004", fast_forward=True)
        with patch.object(supervisor, "send_host") as send_host:
            supervisor.command(dict(protocol=1, type="command", epoch=supervisor.epoch,
                                    request=request, command=command))
        send_host.assert_called_once_with({**command, "protocol": 1, "session": supervisor.session,
                                          "epoch": supervisor.epoch, "request": request})

    def test_duplicate_mismatch_does_not_overwrite_retained_outcome(self):
        in_fd, input_write = os.pipe()
        out_read, out_fd = os.pipe()
        for fd in (in_fd, input_write, out_read, out_fd):
            self.addCleanup(os.close, fd)
        supervisor = PlaySupervisor(None, in_fd, out_fd)
        supervisor.epoch = "0000000000000001"
        obj = dict(protocol=1, type="command", epoch=supervisor.epoch,
                   request="0000000000000001", command=dict(command="Pause", expected_generation="0000000000000001"))
        supervisor.command(obj)
        original = supervisor.pending[obj["request"]]["result"].copy()
        supervisor.command({**obj, "command":dict(command="Resume")})
        self.assertEqual(supervisor.pending[obj["request"]]["result"], original)
        supervisor.command(obj)
        self.assertEqual(json.loads(supervisor.output.frames[-1]), original | {"protocol":1})
        supervisor.command(dict(protocol=1, type="ack", epoch=supervisor.epoch, request=obj["request"]))
        self.assertFalse(supervisor.pending)


if __name__ == "__main__":
    unittest.main()
