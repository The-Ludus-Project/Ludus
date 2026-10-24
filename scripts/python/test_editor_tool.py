"""Editor tooling adapter tests: descriptor contract, shared File API logic,
protocol framing/credit, and REAL process-tree ownership/cancellation.

These tests use real OS pipes and real child processes (not mocked Popen) to
prove argument integrity, descendant cleanup and cancellation on this host. They
synchronize with fixtures through readiness pipes and deadlines, never arbitrary
sleeps tuned to compiler timing. See .kiro/specs/editor-workspace/tasks.md E0.3.
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import cmake_targets
import editor_project
import editor_tool
import engine


# --------------------------------------------------------------------------- #
# Shared descriptor fixtures (consumed by both C++ and Python validation).
# The fixtures live under apps/editor/tests/fixtures; this test loads them and
# asserts the Python validator's verdict matches the recorded expectation.
# --------------------------------------------------------------------------- #
FIXTURE_DIR = engine.repo_root() / "apps" / "editor" / "tests" / "fixtures"


class DescriptorFixtureTests(unittest.TestCase):
    def test_shared_fixtures_match_expected_verdicts(self) -> None:
        manifest_path = FIXTURE_DIR / "cases.json"
        self.assertTrue(manifest_path.is_file(), f"missing shared fixture manifest: {manifest_path}")
        cases = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertTrue(cases, "fixture manifest is empty")
        for case in cases:
            with self.subTest(case=case["file"]):
                data = (FIXTURE_DIR / case["file"]).read_bytes()
                if case["valid"]:
                    descriptor = editor_project.parse_descriptor_bytes(data)
                    self.assertEqual(descriptor.name, case["name"])
                    self.assertEqual(descriptor.provider, case["provider"])
                    self.assertEqual(descriptor.run_args, case["run_args"])
                else:
                    with self.assertRaises(editor_project.ProjectError) as ctx:
                        editor_project.parse_descriptor_bytes(data)
                    self.assertEqual(ctx.exception.code, case["code"])

    def test_empty_and_unicode_and_shell_looking_args_are_valid(self) -> None:
        descriptor = {
            "version": 1, "name": "args", "provider": "cmake", "source_dir": ".",
            "preset": "linux-clang-debug", "target": "app",
            "run": {"cwd": ".", "args": ["", "a b", "日本語", "$(echo hi)", '"quoted"', "--flag"]},
        }
        parsed = editor_project.parse_descriptor_bytes(json.dumps(descriptor).encode("utf-8"))
        self.assertEqual(parsed.run_args, ["", "a b", "日本語", "$(echo hi)", '"quoted"', "--flag"])

    def test_unknown_fields_and_bad_version_rejected(self) -> None:
        for payload, code in [
            ({"version": 2, "name": "n", "provider": "ludus", "source_dir": ".",
              "preset": "linux-clang-debug", "target": "t", "run": {"cwd": ".", "args": []}},
             "UnsupportedVersion"),
            ({"version": True, "name": "n", "provider": "ludus", "source_dir": ".",
              "preset": "linux-clang-debug", "target": "t", "run": {"cwd": ".", "args": []}},
             "UnsupportedVersion"),
            ({"version": 1, "name": "n", "provider": "ludus", "source_dir": ".",
              "preset": "linux-clang-debug", "target": "t", "run": {"cwd": ".", "args": []}, "extra": 1},
             "InvalidProject"),
            ({"version": 1, "name": "n", "provider": "x", "source_dir": ".",
              "preset": "linux-clang-debug", "target": "t", "run": {"cwd": ".", "args": []}},
             "InvalidProject"),
            ({"version": 1, "name": "n", "provider": "ludus", "source_dir": "/abs",
              "preset": "linux-clang-debug", "target": "t", "run": {"cwd": ".", "args": []}},
             "InvalidProject"),
        ]:
            with self.subTest(code=code):
                with self.assertRaises(editor_project.ProjectError) as ctx:
                    editor_project.parse_descriptor_bytes(json.dumps(payload).encode("utf-8"))
                self.assertEqual(ctx.exception.code, code)

    def test_invalid_utf8_and_nul_rejected(self) -> None:
        with self.assertRaises(editor_project.ProjectError):
            editor_project.parse_descriptor_bytes(b"\xff\xfe not utf8")
        payload = json.dumps({"version": 1, "name": "n\u0000", "provider": "ludus", "source_dir": ".",
                              "preset": "linux-clang-debug", "target": "t",
                              "run": {"cwd": ".", "args": []}}).encode("utf-8")
        with self.assertRaises(editor_project.ProjectError):
            editor_project.parse_descriptor_bytes(payload)

    def test_arg_bounds(self) -> None:
        too_many = {"version": 1, "name": "n", "provider": "cmake", "source_dir": ".",
                    "preset": "linux-clang-debug", "target": "t",
                    "run": {"cwd": ".", "args": ["x"] * 65}}
        with self.assertRaises(editor_project.ProjectError):
            editor_project.parse_descriptor_bytes(json.dumps(too_many).encode("utf-8"))


if __name__ == "__main__":
    unittest.main()


# --------------------------------------------------------------------------- #
# Shared File API logic tests (cmake_targets), exercised directly.
# --------------------------------------------------------------------------- #
class FileApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="ludus editor fileapi ")
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / "build"
        self.source = Path(self.temp.name) / "source"
        self.source.mkdir(parents=True)
        self.reply = self.build / ".cmake" / "api" / "v1" / "reply"
        self.reply.mkdir(parents=True)

    def write_reply(self, targets: list[dict], *, major: int = 2, configs: int = 1) -> None:
        documents = {
            "index-1.json": {"reply": {"client-ludus-editor": {"codemodel-v2": {"jsonFile": "model.json"}}}},
            "model.json": {
                "version": {"major": major},
                "paths": {"source": str(self.source.resolve()), "build": str(self.build.resolve())},
                "configurations": [{"targets": [{"name": t["name"], "jsonFile": f"{t['name']}.json"}
                                                 for t in targets]} for _ in range(configs)],
            },
        }
        for target in targets:
            documents[f"{target['name']}.json"] = {
                "type": target.get("type", "EXECUTABLE"),
                "artifacts": target.get("artifacts", [{"path": f"bin/{target['name']}"}]),
            }
        for name, content in documents.items():
            (self.reply / name).write_text(json.dumps(content))

    def test_lists_only_executables_and_resolves_custom_artifact(self) -> None:
        self.write_reply([
            {"name": "app_main"},
            {"name": "libfoo", "type": "STATIC_LIBRARY", "artifacts": []},
            {"name": "tool.v2", "artifacts": [{"path": "custom/out/tool.v2"}]},
        ])
        names = cmake_targets.list_executable_targets(self.build, engine, client="ludus-editor")
        self.assertEqual(names, ["app_main", "tool.v2"])
        resolved = cmake_targets.target_executable(self.build, "tool.v2", engine, client="ludus-editor")
        self.assertEqual(resolved, (self.build / "custom/out/tool.v2").resolve())

    def test_library_and_ambiguous_and_missing_rejected(self) -> None:
        self.write_reply([{"name": "libfoo", "type": "STATIC_LIBRARY", "artifacts": []}])
        with self.assertRaisesRegex(engine.EngineError, "not an executable"):
            cmake_targets.target_executable(self.build, "libfoo", engine, client="ludus-editor")
        with self.assertRaisesRegex(engine.EngineError, "exactly one"):
            cmake_targets.target_executable(self.build, "missing", engine, client="ludus-editor")

    def test_wrong_codemodel_version_rejected(self) -> None:
        self.write_reply([{"name": "app"}], major=3)
        with self.assertRaisesRegex(engine.EngineError, "codemodel version"):
            cmake_targets.list_executable_targets(self.build, engine, client="ludus-editor")

    def test_multi_config_rejected(self) -> None:
        self.write_reply([{"name": "app"}], configs=2)
        with self.assertRaisesRegex(engine.EngineError, "single-config"):
            cmake_targets.list_executable_targets(self.build, engine, client="ludus-editor")

    def test_identity_mismatch_rejected(self) -> None:
        self.write_reply([{"name": "app"}])
        other = Path(self.temp.name) / "elsewhere"
        other.mkdir()
        with self.assertRaisesRegex(engine.EngineError, "source root"):
            cmake_targets.verify_identity(self.build, other, engine, client="ludus-editor")

    def test_reference_traversal_rejected(self) -> None:
        outside = Path(self.temp.name) / "secret.json"
        outside.write_text(json.dumps({"type": "EXECUTABLE", "artifacts": [{"path": "x"}]}))
        (self.reply / "index-1.json").write_text(json.dumps(
            {"reply": {"client-ludus-editor": {"codemodel-v2": {"jsonFile": "../../../../secret.json"}}}}))
        with self.assertRaisesRegex(engine.EngineError, "escapes the reply directory"):
            cmake_targets.list_executable_targets(self.build, engine, client="ludus-editor")

    def test_oversized_file_rejected(self) -> None:
        self.write_reply([{"name": "app"}])
        (self.reply / "model.json").write_text("x" * (cmake_targets.MAX_JSON_FILE_BYTES + 1))
        with self.assertRaisesRegex(engine.EngineError, "1 MiB bound"):
            cmake_targets.list_executable_targets(self.build, engine, client="ludus-editor")


# --------------------------------------------------------------------------- #
# End-to-end protocol harness driving run_stdio over real OS pipes with a fake
# `cmake` that is itself a real child process, and real runtime fixtures.
# --------------------------------------------------------------------------- #
class StubEngineError(RuntimeError):
    pass


class StubEngine:
    """Minimal `engine`-like module for build_plan, backed by a fake cmake."""

    EngineError = StubEngineError

    def __init__(self, cmake_path: Path, build_dir: Path) -> None:
        self._cmake = cmake_path
        self._build_dir = build_dir
        self._ninja = Path(shutil.which("ninja") or "/missing/ninja")

    def cmake(self, _root: Path) -> Path:
        return self._cmake

    def ninja(self, _root: Path) -> Path:
        return self._ninja

    def ensure_bootstrap_for_preset(self, _root: Path, _preset: str) -> None:
        return None

    def build_dir_for_preset(self, _root: Path, _preset: str) -> Path:
        return self._build_dir

    def tool_env(self, _root: Path) -> dict:
        env = os.environ.copy()
        env["LUDUS_EDITOR_TEST"] = "1"
        return env


class ProtocolClient:
    """Drives run_stdio in a thread; sends control, collects events."""

    def __init__(self, context) -> None:
        self._context = context
        self._in_read, self._in_write = os.pipe()   # we write requests here
        self._out_read, self._out_write = os.pipe()  # we read events here
        self.exit_code = None
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        try:
            self.exit_code = editor_tool.run_stdio(self._context, self._in_read, self._out_write)
        finally:
            os.close(self._out_write)

    def send(self, obj: dict) -> None:
        try:
            os.write(self._in_write, (json.dumps(obj) + "\n").encode("utf-8"))
        except (BrokenPipeError, OSError):
            # The adapter may have already finished and closed its stdin read
            # end; a late control write racing a fast operation is harmless.
            pass

    def close_stdin(self) -> None:
        os.close(self._in_write)

    def events(self, timeout: float = 20.0) -> list[dict]:
        """Read all events until the stream closes (adapter finished)."""
        os.set_blocking(self._out_read, False)
        buf = b""
        events: list[dict] = []
        deadline = time.monotonic() + timeout
        import selectors as _sel
        sel = _sel.DefaultSelector()
        sel.register(self._out_read, _sel.EVENT_READ)
        try:
            while time.monotonic() < deadline:
                ready = sel.select(timeout=0.1)
                if not ready and self._thread.is_alive():
                    continue
                # Thread completion does not prove that its output pipe is empty.
                # Drain buffered frames through EOF even if readiness arrives late.
                try:
                    chunk = os.read(self._out_read, 65536)
                except (BlockingIOError, InterruptedError):
                    continue
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.strip():
                        events.append(json.loads(line.decode("utf-8")))
        finally:
            sel.close()
        self._thread.join(timeout=5.0)
        return events

    def cleanup(self) -> None:
        for fd in (self._in_read, self._out_read):
            try:
                os.close(fd)
            except OSError:
                pass


class ProtocolClientTests(unittest.TestCase):
    def test_finished_writer_is_drained_without_readiness_notification(self) -> None:
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        expected = [{"type": "runtime_started", "pid": 123},
                    {"type": "result", "cleanup_confirmed": True}]
        try:
            os.write(write_fd, b"".join((json.dumps(event) + "\n").encode() for event in expected))
        finally:
            os.close(write_fd)
        client = ProtocolClient.__new__(ProtocolClient)
        client._out_read = read_fd
        client._thread = Mock()
        client._thread.is_alive.return_value = False
        selector = Mock()
        selector.select.return_value = []
        with patch("selectors.DefaultSelector", return_value=selector):
            self.assertEqual(client.events(), expected)
        client._thread.join.assert_called_once()


FAKE_CMAKE = r'''#!/usr/bin/env python3
import json, os, sys, time
# A controllable stand-in for the managed cmake. Behavior is driven by files in
# the build dir written by the test (mode markers), so no real toolchain is used.
args = sys.argv[1:]
def build_dir_from(argv):
    if "-B" in argv:
        return argv[argv.index("-B") + 1]
    if "--build" in argv:
        return argv[argv.index("--build") + 1]
    return None
bd = build_dir_from(args)
control = os.environ.get("FAKE_CMAKE_CONTROL", "")
if any(arg.startswith("--list-presets=") for arg in args):
    print('Available presets:\n\n  "linux-clang-debug"\n  "linux-clang-development"\n  "linux-clang-release"')
    sys.exit(0)
if "--preset" in args and "--build" not in args:
    if control == "configure_slow":
        # A readiness flag lets the test synchronize then cancel mid-configure.
        flag = os.environ.get("FAKE_CONFIGURE_READY")
        if flag:
            open(flag, "w").write("configuring")
        time.sleep(10)
    # configure: write a File API reply for client-ludus-editor
    reply = os.path.join(bd, ".cmake", "api", "v1", "reply")
    os.makedirs(reply, exist_ok=True)
    source = os.environ["FAKE_SOURCE"]
    target = os.environ["FAKE_TARGET"]
    artifact = os.environ["FAKE_ARTIFACT"]
    print("configuring with fake cmake", flush=True)
    json.dump({"reply": {"client-ludus-editor": {"codemodel-v2": {"jsonFile": "model.json"}}}},
              open(os.path.join(reply, "index-1.json"), "w"))
    json.dump({"version": {"major": 2}, "paths": {"source": os.path.realpath(source),
              "build": os.path.realpath(bd)},
              "configurations": [{"targets": [{"name": target, "jsonFile": "t.json"}]}]},
              open(os.path.join(reply, "model.json"), "w"))
    json.dump({"type": "EXECUTABLE", "artifacts": [{"path": artifact}]},
              open(os.path.join(reply, "t.json"), "w"))
    if control == "bad_reply":
        # Corrupt the codemodel version so File API reads fail after configure.
        json.dump({"version": {"major": 99}, "paths": {}, "configurations": []},
                  open(os.path.join(reply, "model.json"), "w"))
    sys.exit(0)
elif "--build" in args:
    print("building with fake cmake", flush=True)
    if control == "build_fail":
        sys.stderr.write("fake compile error\n")
        sys.exit(1)
    # Produce the artifact where the codemodel says it is: relative to the build
    # dir (bd), matching cmake_targets.target_executable's resolution.
    artifact = os.path.join(bd, os.environ["FAKE_ARTIFACT"])
    os.makedirs(os.path.dirname(artifact), exist_ok=True)
    import shutil
    shutil.copy(os.environ["FAKE_RUNTIME"], artifact)
    # build_nonexec produces a non-executable artifact so the pre-spawn
    # validation (or the spawn itself) fails; otherwise mark it runnable.
    os.chmod(artifact, 0o644 if control == "build_nonexec" else 0o755)
    sys.exit(0)
sys.exit(0)
'''


class RealProcessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="ludus editor e2e ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text("")
        (self.source / "CMakePresets.json").write_text("{}")
        self.build = self.source / "out" / "build" / "linux-clang-debug"
        self.build.mkdir(parents=True)
        self.project = self.source / "ludus.project.json"
        self.cmake = self.root / "fake-cmake"
        self.cmake.write_text(FAKE_CMAKE)
        self.cmake.chmod(0o755)
        self.artifact_rel = "bin/app"
        self.clients: list[ProtocolClient] = []

    def tearDown(self) -> None:
        for client in self.clients:
            client.cleanup()

    def write_project(self, args: list[str] | None = None) -> str:
        import hashlib
        payload = {
            "version": 1, "name": "e2e", "provider": "cmake", "source_dir": ".",
            "preset": "linux-clang-debug", "target": "app",
            "run": {"cwd": ".", "args": args if args is not None else []},
        }
        data = (json.dumps(payload, indent=2) + "\n").encode("utf-8")
        self.project.write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    def make_runtime(self, body: str) -> Path:
        path = self.root / "runtime.py"
        path.write_text("#!/usr/bin/env python3\n" + body)
        path.chmod(0o755)
        return path

    def context(self, runtime: Path, *, control: str = "", target: str = "app",
                configure_ready: Path | None = None) -> editor_tool.ToolContext:
        os.environ["FAKE_SOURCE"] = str(self.source)
        os.environ["FAKE_TARGET"] = target
        os.environ["FAKE_ARTIFACT"] = self.artifact_rel
        os.environ["FAKE_RUNTIME"] = str(runtime)
        os.environ["FAKE_CMAKE_CONTROL"] = control
        os.environ["FAKE_CONFIGURE_READY"] = str(configure_ready) if configure_ready else ""
        stub = StubEngine(self.cmake, self.build)
        return editor_tool.ToolContext(tooling_root=self.root, engine=stub, cmake_targets=cmake_targets)

    def client(self, context) -> ProtocolClient:
        c = ProtocolClient(context)
        self.clients.append(c)
        return c

    def request(self, operation: str, digest: str, job: str = "0000000000000001") -> dict:
        return {"protocol": 1, "job": job, "operation": operation,
                "project": str(self.project), "expected_sha256": digest}

    # --- configure --------------------------------------------------------- #
    def test_configure_discovers_targets_and_succeeds(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime("import sys; sys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("configure", digest))
        events = client.events()
        types = [e.get("type") for e in events]
        self.assertEqual(types[0], "ready")
        self.assertIn("targets", types)
        targets = next(e for e in events if e.get("type") == "targets")
        self.assertEqual(targets["targets"], ["app"])
        result = events[-1]
        self.assertEqual(result["type"], "result")
        self.assertEqual(result["outcome"], "success")
        self.assertEqual(client.exit_code, 0)

    def test_digest_conflict_fails(self) -> None:
        self.write_project()
        runtime = self.make_runtime("import sys; sys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("configure", "0" * 64))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["code"], "Conflict")
        self.assertEqual(client.exit_code, 1)

    # --- build_run argument integrity -------------------------------------- #
    def test_build_run_preserves_exact_arguments_and_cwd(self) -> None:
        observed = self.root / "observed.json"
        args = ["", "a b", "日本語", "$(echo hi)", '"quoted"', "--flag"]
        digest = self.write_project(args)
        runtime = self.make_runtime(
            "import json,os,sys\n"
            f"open({str(observed)!r},'w').write(json.dumps([sys.argv[1:], os.getcwd()]))\n"
            "sys.exit(0)\n"
        )
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "success", msg=str(events))
        self.assertTrue(result["cleanup_confirmed"])
        self.assertEqual(client.exit_code, 0)
        argv, cwd = json.loads(observed.read_text())
        self.assertEqual(argv, args)  # empty/quoted/Unicode/shell-looking preserved
        self.assertEqual(os.path.realpath(cwd), os.path.realpath(str(self.source)))
        started = next(e for e in events if e.get("type") == "runtime_started")
        self.assertEqual(started["args"], args)

    # --- Play in the editor viewport: hosted-window address ---------------- #
    def test_build_run_gives_only_the_game_its_hosted_window(self) -> None:
        observed = self.root / "hosted.json"
        digest = self.write_project()
        runtime = self.make_runtime(
            "import json,os,sys\n"
            f"open({str(observed)!r},'w').write(json.dumps(os.environ.get('LUDUS_HOSTED_WINDOW')))\n"
            "sys.exit(0)\n"
        )
        address = "1:" + "ab" * 32 + ":/tmp/ludus-hosted-test/window"
        client = self.client(self.context(runtime))
        client.send({**self.request("build_run", digest), "hosted_window": address})
        events = client.events()
        self.assertEqual(events[-1]["outcome"], "success", msg=str(events))
        self.assertEqual(json.loads(observed.read_text()), address)
        # The address is a session secret: no event echoes it.
        self.assertNotIn(address, json.dumps(events))

    def test_build_run_never_inherits_a_stale_hosted_window(self) -> None:
        observed = self.root / "stale.json"
        digest = self.write_project()
        runtime = self.make_runtime(
            "import json,os,sys\n"
            f"open({str(observed)!r},'w').write(json.dumps(os.environ.get('LUDUS_HOSTED_WINDOW')))\n"
            "sys.exit(0)\n"
        )
        stale = "1:" + "cd" * 32 + ":/tmp/ludus-hosted-stale/window"
        with patch.dict(os.environ, {"LUDUS_HOSTED_WINDOW": stale}):
            client = self.client(self.context(runtime))
            client.send(self.request("build_run", digest))
            events = client.events()
        self.assertEqual(events[-1]["outcome"], "success", msg=str(events))
        # Without a request field the game opens its own window.
        self.assertIsNone(json.loads(observed.read_text()))

    # --- failed build never launches an old binary ------------------------- #
    def test_failed_build_does_not_launch_runtime(self) -> None:
        launched = self.root / "launched.flag"
        digest = self.write_project()
        runtime = self.make_runtime(f"open({str(launched)!r},'w').write('x')\n")
        # Pre-existing stale artifact from a prior success, at the codemodel path.
        stale = self.build / self.artifact_rel
        stale.parent.mkdir(parents=True, exist_ok=True)
        import shutil
        shutil.copy(runtime, stale)
        stale.chmod(0o755)
        client = self.client(self.context(runtime, control="build_fail"))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["code"], "BuildFailed")
        self.assertFalse(launched.exists(), "a failed build must not launch any artifact")
        self.assertNotIn("runtime_started", [e.get("type") for e in events])

    # --- real descendant cleanup ------------------------------------------ #
    def test_cancel_cleans_up_ordinary_grandchild(self) -> None:
        # The runtime spawns a grandchild that writes its pid, then both sleep.
        pidfile = self.root / "grandchild.pid"
        ready = self.root / "ready.flag"
        digest = self.write_project()
        grandchild = self.root / "grandchild.py"
        grandchild.write_text(
            "import os, time\n"
            f"with open({str(pidfile.with_suffix('.tmp'))!r}, 'w') as pid: pid.write(str(os.getpid()))\n"
            f"os.replace({str(pidfile.with_suffix('.tmp'))!r}, {str(pidfile)!r})\n"
            "time.sleep(120)\n"
        )
        runtime = self.make_runtime(
            "import subprocess, sys, time\n"
            f"subprocess.Popen([sys.executable, {str(grandchild)!r}])\n"
            f"open({str(ready)!r}, 'w').write('up')\n"
            "time.sleep(120)\n"
        )
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        # Wait for the grandchild to come up using a readiness file (not a sleep).
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not (pidfile.exists() and ready.exists()):
            time.sleep(0.02)
        self.assertTrue(pidfile.exists(), "grandchild did not start")
        grandchild_pid = int(pidfile.read_text())
        self.assertTrue(self._alive(grandchild_pid))
        client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "cancelled")
        self.assertTrue(result["cleanup_confirmed"])
        self.assertEqual(client.exit_code, 130)
        # The ordinary grandchild must be gone.
        self._assert_dead_within(grandchild_pid, 5.0)

    def test_cancel_before_spawn_prevents_launch(self) -> None:
        launched = self.root / "launched.flag"
        configuring = self.root / "configuring.flag"
        digest = self.write_project()
        runtime = self.make_runtime(f"open({str(launched)!r},'w').write('x')\n")
        # A slow configure gives a deterministic window to cancel before the
        # build/launch; we synchronize on the configure readiness flag.
        client = self.client(self.context(runtime, control="configure_slow", configure_ready=configuring))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not configuring.exists():
            time.sleep(0.02)
        self.assertTrue(configuring.exists(), "configure stage did not start")
        client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "cancelled")
        self.assertFalse(launched.exists(), "cancel accepted before spawn must prevent launch")

    def test_stdin_eof_cancels(self) -> None:
        digest = self.write_project()
        ready = self.root / "ready.flag"
        runtime = self.make_runtime(
            f"import time;open({str(ready)!r},'w').write('x');time.sleep(120)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not ready.exists():
            time.sleep(0.02)
        self.assertTrue(ready.exists())
        client.close_stdin()  # parent loss
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "cancelled")

    def test_nonzero_runtime_exit_is_failed_run(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime("import sys; sys.exit(7)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["code"], "RuntimeFailed")
        self.assertEqual(result["exit_code"], 7)
        self.assertEqual(client.exit_code, 1)

    def test_runtime_signal_is_structured(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime("import os,signal;os.kill(os.getpid(), signal.SIGSEGV)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["code"], "RuntimeSignaled")
        self.assertEqual(result["signal"], int(signal.SIGSEGV))

    def test_no_newline_output_is_bounded(self) -> None:
        digest = self.write_project()
        # Emit a large chunk without newlines then exit.
        runtime = self.make_runtime(
            "import sys\nsys.stdout.write('x' * (300*1024))\nsys.stdout.flush()\nsys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "success")
        # Output frames must each respect the protocol line bound; the client
        # parses them without unbounded growth (any single frame < 256 KiB).
        for e in events:
            if e.get("type") == "output":
                self.assertLessEqual(len(json.dumps(e).encode("utf-8")), editor_tool.MAX_PROTOCOL_LINE)

    def test_invalid_utf8_output_replaced(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime(
            "import os,sys\nos.write(1, b'before\\xff\\xfeafter\\n')\nsys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        self.assertEqual(events[-1]["outcome"], "success")

    def test_malformed_file_api_reply_is_reply_invalid(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime("import sys;sys.exit(0)\n")
        client = self.client(self.context(runtime, control="bad_reply"))
        client.send(self.request("configure", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result["code"], "ReplyInvalid")
        self.assertEqual(client.exit_code, 1)

    def test_protocol_version_mismatch_fails(self) -> None:
        self.write_project()
        runtime = self.make_runtime("import sys;sys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send({"protocol": 99, "job": "0000000000000001", "operation": "configure",
                     "project": str(self.project), "expected_sha256": ""})
        events = client.events()
        self.assertEqual(events[-1]["code"], "ProtocolError")
        self.assertEqual(client.exit_code, 2)

    def test_bad_credit_offset_is_protocol_error(self) -> None:
        configuring = self.root / "configuring.flag"
        digest = self.write_project()
        runtime = self.make_runtime("import time;time.sleep(0.2)\n")
        client = self.client(self.context(runtime, control="configure_slow", configure_ready=configuring))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not configuring.exists():
            time.sleep(0.02)
        self.assertTrue(configuring.exists())
        # A future/non-boundary credit offset is a protocol error, handled while
        # the configure stage is draining control I/O.
        client.send({"protocol": 1, "job": "0000000000000001", "type": "credit",
                     "through": "ffffffffffffffff"})
        events = client.events()
        self.assertEqual(events[-1]["code"], "ProtocolError")
        self.assertEqual(client.exit_code, 2)

    # --- additional E0.3 real-process acceptance scenarios ----------------- #
    def test_term_ignoring_child_is_force_killed_and_confirmed(self) -> None:
        # The runtime installs a SIGTERM handler that ignores it, so cancellation
        # must escalate to SIGKILL within the deadline and still confirm cleanup.
        pidfile = self.root / "term_ignore.pid"
        ready = self.root / "ready.flag"
        digest = self.write_project()
        runtime = self.make_runtime(
            "import os, signal, time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            f"open({str(pidfile)!r}, 'w').write(str(os.getpid()))\n"
            f"open({str(ready)!r}, 'w').write('up')\n"
            "while True:\n    time.sleep(0.1)\n"
        )
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not (pidfile.exists() and ready.exists()):
            time.sleep(0.02)
        self.assertTrue(pidfile.exists(), "term-ignoring runtime did not start")
        child_pid = int(pidfile.read_text())
        client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        events = client.events()
        result = events[-1]
        self.assertEqual(result["outcome"], "cancelled")
        self.assertTrue(result["cleanup_confirmed"])  # KILL escalation confirmed
        self.assertEqual(client.exit_code, 130)
        self._assert_dead_within(child_pid, 6.0)

    def test_exiting_leader_with_surviving_writer_is_cleaned_up(self) -> None:
        # The leader spawns a grandchild that keeps a pipe open, then the leader
        # exits. Normal finalize must still clean up the surviving descendant.
        gpidfile = self.root / "writer.pid"
        ready = self.root / "ready.flag"
        digest = self.write_project()
        writer = self.root / "writer.py"
        writer.write_text(
            "import os, time\n"
            f"open({str(gpidfile)!r}, 'w').write(str(os.getpid()))\n"
            # Keep stdout open (inherited) and live well past the leader's exit.
            "time.sleep(120)\n"
        )
        runtime = self.make_runtime(
            "import subprocess, sys, time\n"
            f"subprocess.Popen([sys.executable, {str(writer)!r}])\n"
            f"open({str(ready)!r}, 'w').write('up')\n"
            # Leader exits quickly while the grandchild writer survives.
            "time.sleep(0.3)\n"
            "sys.exit(0)\n"
        )
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not gpidfile.exists():
            time.sleep(0.02)
        self.assertTrue(gpidfile.exists(), "surviving writer did not start")
        writer_pid = int(gpidfile.read_text())
        events = client.events()
        result = events[-1]
        # The run completes (leader exited 0) only after the descendant is gone.
        self.assertEqual(result.get("type"), "result", events)
        self.assertTrue(result["cleanup_confirmed"], events)
        self._assert_dead_within(writer_pid, 6.0)

    def test_simultaneous_stdout_and_stderr_are_both_delivered(self) -> None:
        digest = self.write_project()
        runtime = self.make_runtime(
            "import sys\n"
            "sys.stdout.write('to-out\\n'); sys.stdout.flush()\n"
            "sys.stderr.write('to-err\\n'); sys.stderr.flush()\n"
            "sys.exit(0)\n"
        )
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        streams = {e.get("stream") for e in events if e.get("type") == "output"}
        self.assertIn("stdout", streams)
        self.assertIn("stderr", streams)
        self.assertEqual(events[-1]["outcome"], "success")

    def test_failed_executable_spawn_reports_spawn_failed(self) -> None:
        # Build an artifact without the execute bit so the pre-spawn validation
        # (or the spawn itself) fails and no runtime starts.
        digest = self.write_project()
        runtime = self.make_runtime("import sys; sys.exit(0)\n")
        client = self.client(self.context(runtime, control="build_nonexec"))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertIn(result["code"], ("ArtifactInvalid", "SpawnFailed"))
        self.assertNotIn("runtime_started", [e.get("type") for e in events])

    def test_repeated_stop_is_idempotent(self) -> None:
        ready = self.root / "ready.flag"
        digest = self.write_project()
        runtime = self.make_runtime(
            f"import time;open({str(ready)!r},'w').write('x');time.sleep(120)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not ready.exists():
            time.sleep(0.02)
        self.assertTrue(ready.exists())
        # Multiple cancels must not error or produce a second terminal result.
        for _ in range(3):
            client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        events = client.events()
        results = [e for e in events if e.get("type") == "result"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["outcome"], "cancelled")

    def test_build_success_racing_stop_does_not_launch(self) -> None:
        # Cancel is latched during a slow configure; the subsequent build success
        # must not launch the runtime (stop wins before spawn).
        configuring = self.root / "configuring.flag"
        launched = self.root / "launched.flag"
        digest = self.write_project()
        runtime = self.make_runtime(f"open({str(launched)!r},'w').write('x')\n")
        client = self.client(self.context(runtime, control="configure_slow", configure_ready=configuring))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not configuring.exists():
            time.sleep(0.02)
        self.assertTrue(configuring.exists())
        client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        events = client.events()
        self.assertEqual(events[-1]["outcome"], "cancelled")
        self.assertFalse(launched.exists(), "a late build success must not launch after Stop")

    def test_duplicate_credit_is_idempotent(self) -> None:
        # A credit acknowledging an already-acknowledged boundary is idempotent,
        # not a protocol error. 0 is always a valid (initial) boundary.
        digest = self.write_project()
        runtime = self.make_runtime("import sys;sys.exit(0)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        client.send({"protocol": 1, "job": "0000000000000001", "type": "credit",
                     "through": "0000000000000000"})
        client.send({"protocol": 1, "job": "0000000000000001", "type": "credit",
                     "through": "0000000000000000"})
        events = client.events()
        self.assertEqual(events[-1]["outcome"], "success")
        self.assertEqual(client.exit_code, 0)

    def test_unrelated_process_is_never_signaled(self) -> None:
        # A process started OUTSIDE the adapter's owned group must survive a full
        # cancel; the adapter only signals its own start_new_session group.
        import subprocess as sp

        marker = self.root / "bystander_done.flag"
        bystander = sp.Popen(
            [sys.executable, "-c",
             f"import time;time.sleep(2.5);open({str(marker)!r},'w').write('alive')"])
        self.addCleanup(lambda: (bystander.poll() is None and bystander.kill()))
        ready = self.root / "ready.flag"
        digest = self.write_project()
        runtime = self.make_runtime(
            f"import time;open({str(ready)!r},'w').write('x');time.sleep(120)\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not ready.exists():
            time.sleep(0.02)
        self.assertTrue(ready.exists())
        client.send({"protocol": 1, "job": "0000000000000001", "type": "cancel"})
        client.events()
        # The bystander must run to completion and write its marker.
        self.assertEqual(bystander.wait(timeout=10), 0)
        self.assertTrue(marker.exists(), "an unrelated process was signaled by cleanup")

    def test_second_instance_on_same_build_tree_is_busy(self) -> None:
        # A separate editor instance holding the per-build-tree lock forces Busy.
        import selectors
        import subprocess

        digest = self.write_project()
        runtime = self.make_runtime("import sys;sys.exit(0)\n")
        lock_dir = self.build / ".cmake"
        lock_dir.mkdir(parents=True, exist_ok=True)
        # The protocol harness runs in a thread; a separate child holds the
        # competing lock so this fixture proves interprocess exclusion.
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import fcntl,sys\n"
             "with open(sys.argv[1], 'a') as lock:\n"
             " fcntl.flock(lock, fcntl.LOCK_EX)\n"
             " print('locked', flush=True)\n"
             " sys.stdin.read(1)\n",
             str(lock_dir / ".ludus-editor.lock")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)

        def stop_holder():
            holder.stdin.close()
            try:
                holder.wait(timeout=5)
            except subprocess.TimeoutExpired:
                holder.kill()
                holder.wait(timeout=5)
            holder.stdout.close()
            holder.stderr.close()

        self.addCleanup(stop_holder)
        with selectors.DefaultSelector() as selector:
            selector.register(holder.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(timeout=5), "lock holder did not become ready")
            self.assertEqual(holder.stdout.readline(), "locked\n")
        client = self.client(self.context(runtime))
        client.send(self.request("build_run", digest))
        events = client.events()
        result = events[-1]
        self.assertEqual(result.get("code"), "Busy", events)
        self.assertEqual(client.exit_code, 1)

    # --- helpers ----------------------------------------------------------- #
    @staticmethod
    def _alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except (ProcessLookupError, PermissionError):
            return False

    def _assert_dead_within(self, pid: int, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self._alive(pid):
                return
            time.sleep(0.02)
        self.fail(f"process {pid} was not cleaned up within {timeout}s")


class HostedWindowRequestTests(unittest.TestCase):
    """Only build_run carries a well-formed hosted-window channel address."""

    def base(self, operation: str = "build_run") -> dict:
        return {"protocol": 1, "job": "0123456789abcdef", "operation": operation,
                "project": "/tmp/project/ludus.project.json", "expected_sha256": "a" * 64}

    def test_accepts_a_channel_address(self) -> None:
        address = "1:" + "0f" * 32 + ":/tmp/ludus-hosted-x/window"
        editor_tool._validate_request({**self.base(), "hosted_window": address})

    def test_rejects_malformed_or_misplaced_addresses(self) -> None:
        good = "1:" + "0f" * 32 + ":/tmp/ludus-hosted-x/window"
        for message in (
            {**self.base("build"), "hosted_window": good},
            {**self.base(), "hosted_window": 7},
            {**self.base(), "hosted_window": "2:" + "0f" * 32 + ":/tmp/w"},
            {**self.base(), "hosted_window": "1:" + "0F" * 32 + ":/tmp/w"},
            {**self.base(), "hosted_window": "1:" + "0f" * 32 + ":relative/window"},
            {**self.base(), "hosted_window": "1:" + "0f" * 32 + ":/tmp/w\nX"},
            {**self.base(), "hosted_window": "1:" + "0f" * 32 + ":/" + "a" * 300},
        ):
            with self.subTest(message=message), self.assertRaises(editor_tool.ProtocolError):
                editor_tool._validate_request(message)


class GitHubCiRequestTests(unittest.TestCase):
    """Only project_create carries the boolean GitHub CI choice."""

    def base(self, operation: str = "project_create") -> dict:
        return {"protocol": 1, "job": "0123456789abcdef", "operation": operation, "project": "/tmp/new-game",
                "expected_sha256": "", "sdk": "", "web_sdk": "", "name": "game", "prepare_engine": False}

    def test_accepts_a_boolean_choice(self) -> None:
        for choice in (True, False):
            editor_tool._validate_request({**self.base(), "github_ci": choice})
        editor_tool._validate_request(self.base())

    def test_rejects_malformed_or_misplaced_choices(self) -> None:
        for message in ({**self.base(), "github_ci": "yes"}, {**self.base("project_setup"), "github_ci": True}):
            with self.subTest(message=message), self.assertRaises(editor_tool.ProtocolError):
                editor_tool._validate_request(message)


class SetupDetailsProtocolTests(unittest.TestCase):
    def test_details_keep_long_unicode_paths_out_of_reserved_terminal(self):
        frames = []
        writer = editor_tool.ProtocolWriter("0000000000000001", frames.append)
        details = "Selected engine: Ludus 0.1.0\nInstallation: /" + "日本語/" * 100
        writer.setup_details(details)
        writer.result("success", "configuring", "Ok", "Setup checked", True, 0, None)
        self.assertEqual(details, json.loads(frames[0])["text"])
        self.assertEqual("Setup checked", json.loads(frames[1])["message"])
        self.assertLess(sum(map(len, frames)), editor_tool.CONTROL_RESERVE_BYTES)

    def test_details_bound_and_terminal_reserve(self):
        frames = []
        writer = editor_tool.ProtocolWriter("0000000000000001", frames.append)
        for text in ("", "日" * 6000):
            with self.assertRaises(editor_tool.ProtocolError):
                writer.setup_details(text)
        writer._control_total = editor_tool.CONTROL_AGGREGATE_CAP - editor_tool.CONTROL_RESERVE_BYTES
        with self.assertRaises(editor_tool.ProtocolError):
            writer.setup_details("identity")
        writer.result("failed", "configuring", "ProtocolError", "Details unavailable", True, None, None)
        self.assertEqual(1, len(frames))
