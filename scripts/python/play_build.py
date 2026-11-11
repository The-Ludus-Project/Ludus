"""A short build/publication lane, independent of persistent play ownership.

Canonical SDK resolution, File API and build exclusion are shared with E0/CLI.
Only declared watch inputs are checked; this is conservative, not hermetic.
"""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import selectors
from editor_pipe_io import pipe_selector, set_nonblocking
import sys

from editor_tool import (Operation, OperationResult, Plan, configure_argv, FILE_API_CLIENT,
                         ProtocolError, _StageFailed, Cancelled)
from ludus_tools.cmake_setup import validate_project_presets
from ludus_tools.resolve import assert_stamp_unchanged, write_stamp
from play_documents import DocumentError, TuningDocument, encode_authored, read_play_descriptor
from play_probe import ModuleProbe, ProbeError, native_identity
from play_session import PublishError, publish_generation, source_input_digest


def project_identity(descriptor: Path) -> str:
    # Persisted document game IDs remain module-defined. Project identity is a
    # stable local namespace: relocation is an explicit new project/session.
    return hashlib.sha256(str(descriptor.resolve()).encode()).hexdigest()[:16]


def source_inputs(project: Path, source: Path, sidecar, build: Path) -> list[Path]:
    roots = sidecar.watch_roots or (source,)
    files = {project / "ludus.project.json", project / "ludus.play.json",
             source / "CMakeLists.txt", source / "CMakePresets.json"}
    if (project/"ludus.scripts.json").exists(): files.add(project/"ludus.scripts.json")
    for extra in (source / "CMakeUserPresets.json", project / "ludus.lock.json", project / ".ludus/local.json"):
        if extra.exists():
            files.add(extra)
    if sidecar.startup_document is not None:
        files.add(sidecar.startup_document)
    # Include nested CMake inputs even when source watch roots are narrow.
    for root in set(roots) | {source}:
        if not root.exists():
            raise PublishError(f"watch root missing: {root}")
        if root.is_file():
            files.add(root)
            continue
        for folder, dirs, names in os.walk(root, followlinks=False):
            parent = Path(folder)
            dirs[:] = [name for name in dirs if name not in ("out", ".git", ".ludus", "node_modules")
                       and not (parent / name).resolve().is_relative_to(build.resolve())]
            for name in dirs:
                if (parent / name).is_symlink():
                    raise PublishError("directory symlink in declared inputs is unsupported")
            for name in names:
                path = parent / name
                if root in roots or name == "CMakeLists.txt" or path.suffix == ".cmake":
                    if path.is_symlink() or not path.is_file():
                        raise PublishError(f"nonregular declared input: {path}")
                    files.add(path)
                if len(files) > 16384:
                    raise PublishError("declared source inventory exceeds 16384 files")
    if any(path.is_symlink() or not path.is_file() for path in files):
        raise PublishError("declared source inventory contains nonregular files")
    if sum(path.stat().st_size for path in files) > 64*1024*1024:
        raise PublishError("declared source inventory exceeds 64 MiB")
    return sorted(files)


class GenerationOperation(Operation):
    def _execute_locked(self, descriptor, plan: Plan) -> OperationResult:
        try:
            return self._build_generation(descriptor, plan)
        except (PublishError, DocumentError, ProbeError, OSError) as exc:
            confirmed = getattr(exc, "cleanup_confirmed", True)
            code = "CleanupUnknown" if not confirmed else "Superseded" if "Superseded" in str(exc) else "GenerationInvalid"
            return OperationResult("failed", self._stage, code, str(exc), cleanup_confirmed=confirmed)

    def _probe(self, staging, module_name, host_name, lease_fd):
        module = native_identity(staging / module_name, staging / (module_name + ".dwarf") if sys.platform == "darwin" else None)
        host = native_identity(staging / host_name, staging / (host_name + ".dwarf") if sys.platform == "darwin" else None)
        if module["artifact_type"] != "module" or (
                sys.platform in ("darwin", "win32") and host["artifact_type"] != "host"):
            raise ProbeError("generation requires a gameplay module and a native host executable")
        machine_field = {"darwin": "cpu", "win32": "machine"}.get(sys.platform)
        if machine_field is not None and module[machine_field] != host[machine_field]:
            raise ProbeError("gameplay module and host architectures disagree")
        probe = ModuleProbe(staging / host_name, staging / module_name, cwd=self._plan.run_cwd,
                            env=self._plan.env, lease_fd=lease_fd)
        selector = pipe_selector()
        set_nonblocking(self._supervisor._control_fd)
        selector.register(self._supervisor._control_fd, selectors.EVENT_READ)
        try:
            while True:
                self._check_cancel()
                for _ in selector.select(0.02):
                    self._supervisor._read_control()
                metadata = probe.poll()
                if metadata is not None:
                    break
        except (Cancelled, ProtocolError, OSError) as exc:
            confirmed = probe.cancel()
            if not confirmed:
                raise ProbeError("probe cleanup could not be confirmed", cleanup_confirmed=False) from exc
            raise
        finally:
            selector.close()
        if self._plan.runtime_identity is not None and metadata["sdk_identity"] != self._plan.runtime_identity:
            raise ProbeError("module/host identity disagrees with the resolved SDK")
        return {**metadata, "module_build_id": module["build_id"], "host_build_id": host["build_id"],
                "embedded_symbols": module["embedded_symbols"] and host["embedded_symbols"]}

    def _symbols(self, module, host, plan):
        if sys.platform == "win32":
            return module.with_suffix(".pdb"), host.with_suffix(".pdb")
        if sys.platform != "darwin":
            return None, None
        # Prefer dsymutil from the selected pinned compiler, never an unrelated Homebrew LLVM.
        compiler = Path(plan.compiler or self._context.engine.clang_cxx(self._context.tooling_root)).resolve()
        tool = compiler.parent / "dsymutil"
        if not tool.is_file():
            raise PublishError("pinned dsymutil is missing; run init before live play")
        symbols = []
        for artifact in (module, host):
            output = artifact.with_name(artifact.name + ".dwarf")
            self._run_stage("publishing", [str(tool), "--flat", "--out", str(output), str(artifact)],
                            plan.source_dir, plan.env, "SymbolsFailed")
            symbols.append(output)
        return tuple(symbols)

    def _build_generation(self, descriptor, plan):
        self._plan = plan
        sidecar = read_play_descriptor(self._project_path.parent)
        if sidecar is None:
            raise PublishError("project has no ludus.play.json; executable Run remains available")
        if plan.descriptor.preset not in (
                "linux-clang-debug", "linux-clang-development", "macos-clang-debug", "macos-clang-development",
                "windows-msvc-debug", "windows-msvc-development"):
            raise PublishError("live editing requires a Debug or Development native build")
        try:
            validate_project_presets(plan.cmake[0], plan.source_dir, plan.env, plan.descriptor.preset)
        except ValueError as exc:
            raise PublishError(str(exc)) from exc
        inputs = source_inputs(self._project_path.parent, plan.source_dir, sidecar, plan.build_dir)
        before = source_input_digest(inputs)
        document = TuningDocument(sidecar.startup_document).saved if sidecar.startup_document else None
        game_id = document["game"] if document else "0000000000000001"
        api = self._context.cmake_targets
        api.query_codemodel(plan.build_dir, FILE_API_CLIENT)
        self._run_stage("configuring", configure_argv(plan), plan.source_dir, plan.env, "ConfigureFailed")
        if plan.resolution is not None:
            assert_stamp_unchanged(plan.build_dir, plan.resolution)
            write_stamp(plan.build_dir, plan.resolution)
        api.verify_identity(plan.build_dir, plan.source_dir, self._context.engine, client=FILE_API_CLIENT)
        for target, kind in ((sidecar.module_target, "MODULE_LIBRARY"), (sidecar.host_target, "EXECUTABLE")):
            api.target_artifact(plan.build_dir, target, self._context.engine, expected_type=kind)
        if plan.resolution is not None:
            assert_stamp_unchanged(plan.build_dir, plan.resolution)
        self._run_stage("building", [*plan.cmake, "--build", str(plan.build_dir), "--target",
                                    sidecar.module_target, sidecar.host_target], plan.source_dir, plan.env, "BuildFailed")
        if plan.resolution is not None:
            assert_stamp_unchanged(plan.build_dir, plan.resolution)
        api.verify_identity(plan.build_dir, plan.source_dir, self._context.engine, client=FILE_API_CLIENT)
        module = api.target_artifact(plan.build_dir, sidecar.module_target, self._context.engine, expected_type="MODULE_LIBRARY")
        host = api.target_artifact(plan.build_dir, sidecar.host_target, self._context.engine, expected_type="EXECUTABLE")
        if source_inputs(self._project_path.parent, plan.source_dir, sidecar, plan.build_dir) != inputs:
            raise PublishError("source input inventory changed; result is Superseded")
        self._stage = "publishing"
        self._writer.phase(self._stage)
        self._check_cancel()
        symbols = self._symbols(module, host, plan)
        generation = publish_generation(
            generations_root=self._project_path.parent / ".ludus/generations" / descriptor.preset,
            module_artifact=module, host_artifact=host, symbol_artifact=symbols[0], host_symbol_artifact=symbols[1],
            declared_source_inputs=inputs, pre_build_source_digest=before,
            authored_payload=encode_authored(document) if document else None,
            manifest_fields=dict(project_id=project_identity(self._project_path), game_id=game_id,
                                 module_target=sidecar.module_target, host_target=sidecar.host_target,
                                 build_request_revision=self._job), validate_payloads=self._probe)
        # Cancellation/newest ordering wins before exposing an activation token.
        self._check_cancel()
        if source_inputs(self._project_path.parent, plan.source_dir, sidecar, plan.build_dir) != inputs or source_input_digest(inputs) != before:
            raise PublishError("source inputs changed after publication; result is Superseded")
        self._writer.control({"type":"generation", "path":str(generation), "source_input_digest":before})
        return OperationResult("success", self._stage, "Ok", "immutable generation published")
