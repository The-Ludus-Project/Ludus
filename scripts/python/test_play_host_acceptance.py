"""Real native host supervised over framed control and JSON-lines editor I/O.

CTest supplies mandatory built artifacts; no mocks and no optional skip. This
is headless lifecycle evidence, separate from native window/editor acceptance.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

from play_build import project_identity, source_inputs
from play_assets import artifact_digest, cook_frame_clear
from play_documents import TuningDocument, digest, encode_authored, read_play_descriptor
from play_probe import ModuleProbe, native_identity
from play_session import publish_generation


class Client:
    def __init__(self, root, *, env=None):
        self.proc = subprocess.Popen([sys.executable, str(root / "scripts/python/play_tool.py"), "--stdio",
                                      "--tooling-root", str(root)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.selector = selectors.DefaultSelector()
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.buffer = bytearray()
        self.events = []
        self.wait(lambda e:e["type"] == "ready")

    def send(self, obj):
        self.proc.stdin.write((json.dumps(obj, separators=(",",":"))+"\n").encode())
        self.proc.stdin.flush()

    def wait(self, predicate):
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, _, tail = self.buffer.partition(b"\n")
                self.buffer = bytearray(tail)
                event = json.loads(line)
                self.events.append(event)
                if event["type"] == "fatal":
                    raise AssertionError(event)
                if predicate(event):
                    return event
            for _ in self.selector.select(0.05):
                chunk = os.read(self.proc.stdout.fileno(), 65536)
                if not chunk:
                    raise AssertionError(f"premature supervisor EOF: {self.proc.stderr.read()}")
                self.buffer.extend(chunk)
        raise AssertionError(f"event deadline: {self.events[-5:]}")

    def command(self, request, **fields):
        obj = dict(protocol=1, type="command", epoch="0000000000000001", request=f"{request:016x}", command=fields)
        self.send(obj)
        return self.wait(lambda e:e["type"] == "result" and e.get("request") == obj["request"])

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                assert self.proc.wait(timeout=8) == 0
            except BaseException:
                self.proc.kill()
                self.proc.wait()
                raise
        self.proc.stdout.close()
        self.proc.stderr.close()
        self.selector.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--a", type=Path, required=True)
    parser.add_argument("--b", type=Path, required=True)
    parser.add_argument("--crash", type=Path, required=True)
    parser.add_argument("--hang", type=Path, required=True)
    parser.add_argument("--destructor-crash", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ludus play actor ") as td:
        project = Path(td).resolve()
        descriptor = project / "ludus.project.json"
        descriptor.write_text(json.dumps(dict(version=1, name="actor", provider="cmake", source_dir=".",
                                             preset="linux-clang-development", target="host",
                                             run=dict(cwd=".", args=["--headless"]))))
        (project / "CMakeLists.txt").write_text("# Metadata-only open; no configure\n")
        (project / "CMakePresets.json").write_text(json.dumps(dict(
            version=3,
            cmakeMinimumRequired=dict(major=3, minor=29, patch=0),
            configurePresets=[dict(name="linux-clang-development", generator="Ninja",
                                   binaryDir="${sourceDir}/out/build/linux-clang-development")],
            buildPresets=[dict(name="linux-clang-development", configurePreset="linux-clang-development")],
            testPresets=[dict(name="linux-clang-development", configurePreset="linux-clang-development")])))
        (project / "src").mkdir()
        (project / "src/game.cpp").write_text("first source revision\n")
        tuning = {"version":1, "game":"0000000000000001", "objects":[
            {"id":"0000000000001001", "properties":[
                {"id":"0000000000002001", "kind":"float32", "value":1.0}]}]}
        tuning_path = project / "game.tuning.json"
        tuning_path.write_text(json.dumps(tuning))
        (project / "ludus.play.json").write_text(json.dumps(dict(
            version=1, host_target="host", module_target="game", startup_document="game.tuning.json",
            watch_roots=["src"])))
        sidecar = read_play_descriptor(project)
        inputs = source_inputs(project, project, sidecar, project / "out/build/linux-clang-development")
        generations = project / ".ludus/generations/linux-clang-development"
        def verify(staging, module, host, fd):
            probe = ModuleProbe(staging / host, staging / module, cwd=project, env=dict(os.environ), lease_fd=fd)
            try:
                while True:
                    metadata = probe.poll()
                    if metadata is not None:
                        break
                    time.sleep(0.005)
            finally:
                assert probe.cancel()
            return {**metadata, "module_build_id":native_identity(staging / module, staging / (module + ".dwarf") if sys.platform == "darwin" else None)["build_id"],
                    "host_build_id":native_identity(staging / host, staging / (host + ".dwarf") if sys.platform == "darwin" else None)["build_id"], "embedded_symbols":sys.platform != "darwin"}
        symbols = {}
        def publish_native(**kwargs):
            if sys.platform == "darwin":
                for role in ("module", "host"):
                    artifact = kwargs[role + "_artifact"]
                    if artifact not in symbols:
                        output = project / (artifact.name + ".dwarf")
                        subprocess.run([str(Path(os.environ["LUDUS_DSYMUTIL"])), "--flat", "--out", str(output), str(artifact)], check=True)
                        symbols[artifact] = output
                kwargs["symbol_artifact"] = symbols[kwargs["module_artifact"]]
                kwargs["host_symbol_artifact"] = symbols[kwargs["host_artifact"]]
            return publish_generation(**kwargs)
        published = [publish_native(
            generations_root=generations, module_artifact=module, host_artifact=args.host,
            symbol_artifact=None, declared_source_inputs=inputs, authored_payload=encode_authored(tuning),
            validate_payloads=verify,
            manifest_fields=dict(project_id=project_identity(descriptor), game_id="0000000000000001",
                                 host_target="host", module_target="game", build_request_revision="acceptance"))
                     for module in (args.a, args.b, args.crash, args.hang)]
        client = Client(args.root)
        try:
            start = dict(protocol=1, type="start", epoch="0000000000000001", request="0000000000000001",
                         project=str(descriptor), expected_sha256=hashlib.sha256(descriptor.read_bytes()).hexdigest(),
                         generation_path=str(published[0]))
            client.send(start)
            result = client.wait(lambda e:e["type"] == "result" and e.get("request") == start["request"])
            assert result["status"] == "Ok", result
            document = next((event for event in client.events if event["type"] == "document"), None)
            assert document is not None, client.events
            assert document["available"] and not document["dirty"] and len(document["digest"]) == 64
            generation = published[0].name[:16]
            assert client.command(6, command="Pause", expected_generation=generation)["status"] == "Ok"
            status = client.command(7, command="Status")["host"]
            assert status["state"] == "Paused" and status["generation"] == generation, status
            ticks = status["sim_ticks"]
            assert client.command(8, command="Step", expected_generation=generation)["status"] == "Ok"
            assert client.command(9, command="Status")["host"]["sim_ticks"] == ticks+1
            reload = dict(protocol=1, type="reload", epoch="0000000000000001", request="000000000000000a",
                          generation_path=str(published[1]), expected_generation=generation)
            client.send(reload)
            result = client.wait(lambda e:e["type"] == "result" and e.get("request") == reload["request"])
            assert result["status"] == "Ok", result
            # Same request/payload returns retained result; native code runs once.
            client.send(reload)
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == reload["request"])["status"] == "Ok"
            status = client.command(11, command="Status")["host"]
            assert status["state"] == "Paused" and status["sim_ticks"] == ticks+1, status
            assert status["generation"] == published[1].name[:16], status
            # Runtime timing crosses the supervisor unchanged: host-computed
            # intervals and counts, no absolute timestamps. The committed reload
            # dropped the frame baseline, so startup and reload are both counted.
            assert len(status["timing"]) == 1, status
            timing = status["timing"][0]
            assert timing["pacing_interval_ns"] == 16_666_667 and timing["frames"] > 0, timing
            assert timing["baselines"] >= 2 and timing["simulation_ticks"] == status["sim_ticks"], timing
            assert 0 < timing["window_frames"] <= min(120, timing["frames"]), timing
            assert not any(key.endswith(("_at", "timestamp", "wake_ns", "target_ns")) for key in timing), timing
            assert client.command(12, command="ReadProperties", expected_generation=status["generation"])["status"] == "Ok"
            assert any(e["type"] == "host" and e["event"]["event"] == "PropertiesChanged" for e in client.events)
            assert client.command(13, command="Resume", expected_generation=status["generation"])["status"] == "Ok"
            assert client.command(14, command="Stop")["cleanup_confirmed"] is True
            for index, generation_path in enumerate(published[2:]):
                request = f"{15+index*2:016x}"
                client.send({**start, "request":request, "generation_path":str(generation_path)})
                assert client.wait(lambda e:e["type"] == "result" and e.get("request") == request)["status"] == "Ok"
                if index == 0:
                    ended = client.wait(lambda e:e["type"] == "ended")
                    assert ended["reason"] == "HostSignaled" and ended["signal"] == 6 and ended["cleanup_confirmed"], ended
                else:
                    diagnostic = client.wait(lambda e:e["type"] == "diagnostic")
                    assert diagnostic["code"] == "HostUnresponsive", diagnostic
                    # A hang is diagnosed without killing the game. Stop is an
                    # explicit bounded recovery request, including native TERM.
                    assert client.command(18, command="Stop")["cleanup_confirmed"] is True
                assert descriptor.read_bytes() == json.dumps(dict(version=1, name="actor", provider="cmake", source_dir=".",
                                                                  preset="linux-clang-development", target="host",
                                                                  run=dict(cwd=".", args=["--headless"]))).encode()
            edited_value = {"object":"0000000000001001", "property":"0000000000002001",
                            "kind":"float32", "value":0.75}
            applied = client.command(19, command="ApplyToDocument", expected_digest=document["digest"],
                                     edits=[edited_value])
            assert applied["status"] == "Ok" and applied["document_dirty"] and applied["can_undo"], applied
            undone = client.command(20, command="UndoDocument")
            assert undone["status"] == "Ok" and not undone["document_dirty"] and undone["can_redo"], undone
            redone = client.command(21, command="RedoDocument")
            assert redone["status"] == "Ok" and redone["document_dirty"], redone
            saved = client.command(22, command="SaveDocument")
            assert saved["status"] == "Ok" and not saved["document_dirty"], saved
            assert json.loads(tuning_path.read_bytes())["objects"][0]["properties"][0]["value"] == 0.75
            # A second start cannot reuse old replies/session identity.
            inputs = source_inputs(project, project, sidecar, project / "out/build/linux-clang-development")
            fresh = publish_native(
                generations_root=generations, module_artifact=args.a, host_artifact=args.host,
                symbol_artifact=None, declared_source_inputs=inputs,
                authored_payload=encode_authored(TuningDocument(tuning_path).saved), validate_payloads=verify,
                manifest_fields=dict(project_id=project_identity(descriptor), game_id="0000000000000001",
                                     host_target="host", module_target="game", build_request_revision="reopen"))
            watched = project / "src/game.cpp"
            watched.write_text("source changed after publication\n")
            stale = {**start, "request":"0000000000000017", "generation_path":str(fresh)}
            client.send(stale)
            rejected = client.wait(lambda e:e["type"] == "result" and e.get("request") == stale["request"])
            assert rejected["status"] == "InvalidRequest" and "source inputs changed" in rejected["message"], rejected
            watched.write_text("first source revision\n")
            current = {**start, "request":"0000000000000018", "generation_path":str(fresh)}
            client.send(current)
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == current["request"])["status"] == "Ok"
            assert client.command(25, command="ReadProperties", expected_generation=fresh.name[:16])["status"] == "Ok"
            assert any(e["type"] == "host" and e["event"]["event"] == "PropertiesChanged" and
                       any(p["property"] == "0000000000002001" and p["bits"] == 0x3F400000 for p in e["event"]["properties"])
                       for e in client.events[-5:])
            clear_source = project / "clear.json"
            clear_source.write_text('{"version":1,"kind":"frame_clear","rgba":[0.1,0.65,0.25,1]}')
            cooked = cook_frame_clear(clear_source.read_bytes())
            asset = dict(protocol=1, type="asset", epoch="0000000000000001", request="000000000000001a",
                         source="clear.json", expected_generation=fresh.name[:16])
            client.send(asset)
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == asset["request"])["status"] == "Ok"
            status = client.command(27, command="Status")["host"]
            assert status["clear_asset_generation"] == artifact_digest(cooked), status
            clear_source.write_text('{"version":1,"kind":"frame_clear","rgba":[0,0,2,1]}')
            asset["request"] = "000000000000001c"
            client.send(asset)
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == asset["request"])["status"] == "InvalidRequest"
            assert client.command(29, command="Status")["host"]["clear_asset_generation"] == status["clear_asset_generation"]
            invalid = client.command(30, command="ReloadAsset", expected_generation=fresh.name[:16],
                                     asset_id="1000000000000001", artifact=cooked.hex(), digest="0000000000000001")
            assert invalid["status"] == "ReloadRejected", invalid
            assert client.command(31, command="Status")["host"]["clear_asset_generation"] == status["clear_asset_generation"]
            dying = publish_native(
                generations_root=generations, module_artifact=args.destructor_crash, host_artifact=args.host,
                symbol_artifact=None, declared_source_inputs=inputs,
                authored_payload=encode_authored(TuningDocument(tuning_path).saved), validate_payloads=verify,
                manifest_fields=dict(project_id=project_identity(descriptor), game_id="0000000000000001",
                                     host_target="host", module_target="game", build_request_revision="destructor"))
            assert client.command(32, command="Stop")["cleanup_confirmed"] is True
            client.send({**start, "request":"0000000000000021", "generation_path":str(dying)})
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == "0000000000000021")["status"] == "Ok"
            assert client.command(34, command="Stop")["cleanup_confirmed"] is True
            assert any(e["type"] == "ended" and e["reason"] == "HostSignaled" and e["signal"] == 6 for e in client.events[-8:])
            assert json.loads(tuning_path.read_bytes())["objects"][0]["properties"][0]["value"] == 0.75
            client.send({**start, "request":"0000000000000023", "generation_path":str(fresh)})
            assert client.wait(lambda e:e["type"] == "result" and e.get("request") == "0000000000000023")["status"] == "Ok"
            client.close()  # Parent EOF stops and observes the real native host.
            print(json.dumps(dict(status="PASS", scope="real native headless supervisor lifecycle/reload/EOF",
                                  events=client.events), separators=(",", ":")))
        finally:
            if client.proc.poll() is None:
                client.close()
        retired_paths = [publish_native(
            generations_root=generations, module_artifact=module, host_artifact=args.host,
            symbol_artifact=None, declared_source_inputs=inputs,
            authored_payload=encode_authored(TuningDocument(tuning_path).saved), validate_payloads=verify,
            manifest_fields=dict(project_id=project_identity(descriptor), game_id="0000000000000001",
                                 host_target="host", module_target="game", build_request_revision="retirement"))
                         for module in (args.a, args.b)]
        retired = Client(args.root, env={**os.environ, "LUDUS_FIXTURE_FAIL":"retire"})
        try:
            retired.send({**start, "generation_path":str(retired_paths[0])})
            assert retired.wait(lambda e:e["type"] == "result" and e.get("request") == start["request"])["status"] == "Ok"
            paused = retired.command(2, command="Pause", expected_generation=retired_paths[0].name[:16])
            assert paused["status"] == "Ok", paused
            request = "0000000000000003"
            retired.send(dict(protocol=1, type="reload", epoch="0000000000000001", request=request,
                              generation_path=str(retired_paths[1]), expected_generation=retired_paths[0].name[:16]))
            result = retired.wait(lambda e:e["type"] == "result" and e.get("request") == request)
            assert result["status"] == "RestartRequired", result
            assert result["host"]["state"] == "CleanupUnknown", result
            assert result["host"]["generation"] == retired_paths[1].name[:16], result
            assert retired.command(4, command="Status")["host"]["state"] == "CleanupUnknown"
            assert retired.command(5, command="Resume", expected_generation=retired_paths[1].name[:16])["status"] == "RestartRequired"
            assert retired.command(6, command="Stop")["cleanup_confirmed"] is True
            assert json.loads(tuning_path.read_bytes())["objects"][0]["properties"][0]["value"] == 0.75
            print("PASS actual post-commit retirement uncertainty: B is active, mutations disabled, explicit Stop observes cleanup")
        finally:
            retired.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
