"""Build-generation publication and leasing for project-live-reload (design 5).

A generation is an immutable directory containing a validated gameplay module
copy, its matching debug symbols, and a bounded manifest. Publication is atomic:
staging copies and verifies into a private temp directory, then a single
rename moves it to a unique, never-reused generation id. Failed/cancelled builds
never activate a stale artifact. Active and debugger-leased generations are
never overwritten or collected; retention keeps active + candidate + previous.

This module is Qt-free and import-safe for tests. The editor supervisor and the
Python play-session adapter call publish_generation() after a verified build and
resolve artifacts through the shared CMake File API resolver (cmake_targets),
never by guessing output paths.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

if os.name == "nt":
    from editor_process_windows import assert_unleased, lock_file, open_guard


else:
    import fcntl

    def lock_file(handle, *, blocking=True):
        fcntl.flock(handle, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))

    def open_guard(path, *, blocking=True):
        handle = path.open("a+b")
        try:
            fcntl.flock(handle, fcntl.LOCK_SH | (0 if blocking else fcntl.LOCK_NB))
        except BaseException:
            handle.close()
            raise
        return handle


# Manifest bounds (design 5): manifest <= 64 KiB, at most 16 payload files; v1
# payload is module + symbols, not a dynamic plugin graph.
MAX_MANIFEST_BYTES = 64 * 1024
MAX_PAYLOAD_FILES = 16
MANIFEST_SCHEMA = 3

# Default retention: active, candidate and previous generation (design 5).
DEFAULT_RETENTION = 3


class PublishError(Exception):
    """A build generation could not be published; the old active stays current."""


@dataclass
class GenerationManifest:
    """The bounded metadata recorded alongside a published generation."""

    schema: int
    generation_id: str
    project_id: str
    game_id: str
    module_target: str
    host_target: str
    module_file: str
    host_file: str
    symbol_file: str | None
    authored_file: str | None
    sdk_identity: str
    build_request_revision: str
    source_input_digest: str
    abi_major: int
    abi_minor: int
    property_schema: int
    checkpoint_schema: int
    capabilities: int
    module_build_id: str
    host_build_id: str
    embedded_symbols: bool
    files: dict[str, str] = field(default_factory=dict)  # relative path -> sha256
    created_unix: int = 0
    host_symbol_file: str | None = None

    def to_json(self) -> str:
        return json.dumps(self.__dict__, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_input_digest(source_files: list[Path]) -> str:
    """Digest of declared source inputs for the superseded-build check (design 5).

    Conservative, not a hermetic snapshot: hashes the sorted (relpath, content)
    of the declared inputs so a change between build and publication is visible.
    """
    digest = hashlib.sha256()
    for path in sorted((p.resolve() for p in source_files), key=lambda p: str(p)):
        try:
            digest.update(str(path).encode("utf-8"))
            digest.update(b"\0")
            digest.update(sha256_file(path).encode("ascii"))
            digest.update(b"\0")
        except OSError as exc:
            raise PublishError(f"source input missing/unreadable: {path}") from exc
    return digest.hexdigest()


_GEN_COUNTER = 0
_GEN_MILLIS = 0


def next_generation_id() -> str:
    """An opaque, strictly-monotonic, never-reused generation id.

    The leading millisecond timestamp plus a process-wide monotonic counter
    guarantees lexical sort order equals publish order (so retention keeps the
    actually-newest generations), and the random suffix keeps ids unguessable.
    """
    global _GEN_COUNTER, _GEN_MILLIS
    _GEN_COUNTER += 1
    _GEN_MILLIS = max(_GEN_MILLIS, int(time.time() * 1000))
    return (
        f"{_GEN_MILLIS:013d}-{_GEN_COUNTER:09d}-"
        f"{os.getpid():06d}-{int.from_bytes(os.urandom(3), 'big'):06d}"
    )


def publish_generation(**kwargs: Any) -> Path:
    """Serialize publication/GC independently of the shared CMake build lock.

    The caller still holds BuildTreeLock while producing/verifying artifacts.
    A persistent root sequence orders publications across processes and clock
    adjustments; directory IDs remain opaque to the editor.
    """
    root = Path(kwargs["generations_root"])
    root.mkdir(parents=True, exist_ok=True)
    before = kwargs.get("pre_build_source_digest")
    if before is not None and source_input_digest(kwargs["declared_source_inputs"]) != before:
        raise PublishError("source inputs changed during build; result is Superseded")
    with (root / ".publication.lock").open("a+b") as lock:
        lock_file(lock)
        sequence_file = root / ".sequence"
        sequence = int(sequence_file.read_text()) + 1 if sequence_file.exists() else 1
        if not 0 < sequence < 2**64:
            raise PublishError("generation sequence exhausted; create a new session/store")
        sequence_file.write_text(str(sequence), encoding="ascii")
        generation_id = f"{sequence:016x}-{os.urandom(8).hex()}"
        return _publish_generation_locked(**kwargs, generation_id=generation_id, publication_fd=lock.fileno())


def _publish_generation_locked(
    *,
    generations_root: Path,
    module_artifact: Path,
    host_artifact: Path,
    symbol_artifact: Path | None,
    manifest_fields: dict[str, Any],
    declared_source_inputs: list[Path],
    pre_build_source_digest: str | None = None,
    generation_id: str,
    publication_fd: int,
    host_symbol_artifact: Path | None = None,
    validate_payloads: Any = None,
    authored_payload: bytes | None = None,
) -> Path:
    """Atomically publish one immutable generation; return its directory.

    Staging copies the module (+symbols) into a private temp dir under the
    generations root, verifies hashes, writes the bounded manifest, then renames
    the staging dir onto a unique generation id. If the declared source inputs
    changed since `pre_build_source_digest` the result is Superseded and nothing
    is published (design 5: never label a changed-input result the latest).
    """
    generations_root.mkdir(parents=True, exist_ok=True)

    if not module_artifact.is_file():
        raise PublishError(f"module artifact missing: {module_artifact}")
    if not host_artifact.is_file():
        raise PublishError(f"host artifact missing: {host_artifact}")
    artifacts = [module_artifact, host_artifact] + [p for p in (symbol_artifact, host_symbol_artifact) if p is not None]
    if any(path.stat().st_size > 512 * 1024 * 1024 for path in artifacts) or \
            sum(path.stat().st_size for path in artifacts) > 1024 * 1024 * 1024:
        raise PublishError("native payload exceeds the bounded generation inventory")

    current_digest = source_input_digest(declared_source_inputs)
    if pre_build_source_digest is not None and current_digest != pre_build_source_digest:
        raise PublishError("source inputs changed during build; result is Superseded")

    gen_id = generation_id
    # Stage into a private temp dir in the SAME filesystem so the publish is an
    # atomic rename (never a cross-device copy that could be half-written).
    staging = Path(tempfile.mkdtemp(prefix=f".staging-{gen_id}-", dir=generations_root))
    try:
        payload_files: dict[str, str] = {}
        module_name = module_artifact.name
        host_name = host_artifact.name
        names = [p.name for p in artifacts]
        if authored_payload is not None:
            names.append("authored.bin")
            if not isinstance(authored_payload, bytes) or len(authored_payload) > 256 * 1024:
                raise PublishError("invalid authored document transport")
        if len(set(names)) != len(names):
            raise PublishError("generation payload filenames collide")
        for name in names:
            if not _payload_name(name):
                raise PublishError("invalid payload filename")
        shutil.copy2(module_artifact, staging / module_name)
        payload_files[module_name] = sha256_file(staging / module_name)
        shutil.copy2(host_artifact, staging / host_name)
        payload_files[host_name] = sha256_file(staging / host_name)

        symbol_name: str | None = None
        if symbol_artifact is not None:
            if not symbol_artifact.is_file():
                raise PublishError("requested symbol artifact is missing")
            symbol_name = symbol_artifact.name
            shutil.copy2(symbol_artifact, staging / symbol_name)
            payload_files[symbol_name] = sha256_file(staging / symbol_name)
        if host_symbol_artifact is not None:
            if not host_symbol_artifact.is_file():
                raise PublishError("requested host symbol artifact is missing")
            shutil.copy2(host_symbol_artifact, staging / host_symbol_artifact.name)
            payload_files[host_symbol_artifact.name] = sha256_file(staging / host_symbol_artifact.name)
        if authored_payload is not None:
            (staging / "authored.bin").write_bytes(authored_payload)
            payload_files["authored.bin"] = sha256_file(staging / "authored.bin")

        if len(payload_files) > MAX_PAYLOAD_FILES:
            raise PublishError(f"too many payload files ({len(payload_files)} > {MAX_PAYLOAD_FILES})")

        # Verify the staged copy byte-for-byte matches the source artifact.
        if payload_files[module_name] != sha256_file(module_artifact):
            raise PublishError("module copy hash mismatch; corrupt staging")
        if payload_files[host_name] != sha256_file(host_artifact):
            raise PublishError("host copy hash mismatch; corrupt staging")
        if symbol_artifact is not None and payload_files[symbol_artifact.name] != sha256_file(symbol_artifact):
            raise PublishError("symbol copy hash mismatch; corrupt staging")

        if host_symbol_artifact is not None and payload_files[host_symbol_artifact.name] != sha256_file(host_symbol_artifact):
            raise PublishError("host symbol copy hash mismatch; corrupt staging")

        # Query the immutable COPIES in an owned process before exposure. A
        # constructor/query crash never executes in the editor or active host.
        if validate_payloads is not None:
            manifest_fields = {**manifest_fields, **validate_payloads(staging, module_name, host_name, publication_fd)}
            for name, expected in payload_files.items():
                if sha256_file(staging / name) != expected:
                    raise PublishError("probe changed a staged artifact")

        manifest = GenerationManifest(
            schema=MANIFEST_SCHEMA,
            generation_id=gen_id,
            project_id=manifest_fields.get("project_id", ""),
            game_id=manifest_fields.get("game_id", ""),
            module_target=manifest_fields.get("module_target", ""),
            host_target=manifest_fields.get("host_target", ""),
            module_file=module_name,
            host_file=host_name,
            symbol_file=symbol_name,
            host_symbol_file=host_symbol_artifact.name if host_symbol_artifact is not None else None,
            authored_file="authored.bin" if authored_payload is not None else None,
            sdk_identity=manifest_fields.get("sdk_identity", ""),
            build_request_revision=manifest_fields.get("build_request_revision", ""),
            source_input_digest=current_digest,
            abi_major=manifest_fields.get("abi_major", 1),
            abi_minor=manifest_fields.get("abi_minor", 0),
            property_schema=manifest_fields.get("property_schema", 0),
            checkpoint_schema=manifest_fields.get("checkpoint_schema", 0),
            capabilities=manifest_fields.get("capabilities"),
            module_build_id=manifest_fields.get("module_build_id"),
            host_build_id=manifest_fields.get("host_build_id"),
            embedded_symbols=manifest_fields.get("embedded_symbols"),
            files=payload_files,
            created_unix=int(time.time()),
        )
        manifest_text = manifest.to_json()
        if len(manifest_text.encode("utf-8")) > MAX_MANIFEST_BYTES:
            raise PublishError("manifest exceeds the 64 KiB bound")
        (staging / "manifest.json").write_text(manifest_text, encoding="utf-8")

        # Validate the closed manifest before exposure and check sources again
        # after all copies/hashes. A copy-time edit must not publish stale code.
        _validate_manifest(staging, json.loads(manifest_text), gen_id)
        if source_input_digest(declared_source_inputs) != current_digest:
            raise PublishError("source inputs changed during publication; result is Superseded")
        for child in staging.iterdir():
            os.chmod(child, 0o444 | (child.stat().st_mode & 0o111))
        if sys.platform not in ("darwin", "win32"):
            os.chmod(staging, 0o555)

        final = generations_root / gen_id
        if final.exists():
            raise PublishError(f"generation id already exists (never reuse): {gen_id}")
        # Darwin requires a writable source directory for rename. Lease acquisition
        # and GC share our publication lock; no consumer receives this path until
        # sealing completes. Rebind staging so a sealing failure removes the moved
        # directory rather than leaving a writable generation behind.
        os.rename(staging, final)
        staging = final
        if sys.platform == "darwin":
            os.chmod(final, 0o555)
        return final
    except Exception as exc:
        # Uncertain native ownership pins its files. Staging directories are
        # deliberately outside the collector's valid-generation inventory.
        if getattr(exc, "cleanup_confirmed", True):
            if staging.exists() and not staging.is_symlink():
                os.chmod(staging, 0o755)
            if os.name == "nt" and staging.is_dir():
                for child in staging.iterdir():
                    os.chmod(child, 0o644)
            shutil.rmtree(staging, ignore_errors=True)
        raise


def _payload_name(name: Any) -> bool:
    return isinstance(name, str) and bool(name) and name not in (".", "..", "manifest.json") and "/" not in name and "\\" not in name and len(name.encode("utf-8")) <= 255


def _validate_manifest(generation_dir: Path, data: Any, generation_id: str) -> None:
    if not isinstance(data, dict) or set(data) != set(GenerationManifest.__dataclass_fields__):
        raise PublishError("unknown or incomplete generation manifest fields")
    if type(data["schema"]) is not int or data["schema"] != MANIFEST_SCHEMA:
        raise PublishError("unsupported generation manifest schema")
    if data["generation_id"] != generation_id:
        raise PublishError("generation identity disagrees with its directory")
    if re.fullmatch(r"[0-9a-f]{16}-[0-9a-f]{16}", generation_id) is None or int(generation_id[:16], 16) == 0:
        raise PublishError("invalid generation identity")
    for key in ("project_id", "game_id"):
        if not isinstance(data[key], str) or re.fullmatch(r"[0-9a-f]{16}", data[key]) is None:
            raise PublishError(f"invalid {key}")
    for key in ("sdk_identity", "build_request_revision", "module_target", "host_target"):
        if not isinstance(data[key], str) or not data[key] or len(data[key].encode("utf-8")) > 4096:
            raise PublishError(f"invalid {key}")
    if not isinstance(data["source_input_digest"], str) or re.fullmatch(r"[0-9a-f]{64}", data["source_input_digest"]) is None:
        raise PublishError("invalid source input digest")
    for key in ("abi_major", "abi_minor", "property_schema", "checkpoint_schema", "capabilities", "created_unix"):
        if type(data[key]) is not int or not 0 <= data[key] < 2**64:
            raise PublishError(f"invalid {key}")
    if data["abi_major"] != 1 or data["abi_minor"] > 1:
        raise PublishError(f"unsupported gameplay ABI: {data['abi_major']}.{data['abi_minor']}; expected 1.0 or 1.1")
    if len(data["sdk_identity"].encode("utf-8")) > 256:
        raise PublishError("SDK identity exceeds gameplay ABI bound")
    if data["capabilities"] & ~15 or (data["capabilities"] & 8 and data["abi_minor"] < 1) or (data["capabilities"] & 1 and not data["checkpoint_schema"]) or \
            (data["capabilities"] & 2 and not data["property_schema"]):
        raise PublishError("invalid capabilities/schema")
    for key in ("module_build_id", "host_build_id"):
        if not isinstance(data[key], str) or re.fullmatch(r"(?:[0-9a-f]{2}){8,64}", data[key]) is None:
            raise PublishError(f"invalid {key}")
    if type(data["embedded_symbols"]) is not bool or (not data["embedded_symbols"] and
            (data["symbol_file"] is None or data["host_symbol_file"] is None)):
        raise PublishError("native live editing requires matching debug symbols")
    files = data["files"]
    if not isinstance(files, dict) or not 2 <= len(files) <= MAX_PAYLOAD_FILES:
        raise PublishError("invalid payload inventory")
    required = {data["module_file"], data["host_file"]}
    if data["symbol_file"] is not None:
        required.add(data["symbol_file"])
    if data["host_symbol_file"] is not None:
        required.add(data["host_symbol_file"])
    if data["authored_file"] is not None:
        required.add(data["authored_file"])
    if set(files) != required or len(required) < 2:
        raise PublishError("payload inventory disagrees with declared artifacts")
    if {p.name for p in generation_dir.iterdir()} != set(files) | {"manifest.json"}:
        raise PublishError("undeclared generation contents")
    for name, digest in files.items():
        if not _payload_name(name) or not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            raise PublishError("invalid payload name/hash")
        payload = generation_dir / name
        if payload.is_symlink() or payload.resolve().parent != generation_dir.resolve() or not payload.is_file():
            raise PublishError("payload escapes generation or is missing")
        if payload.stat().st_size > 512 * 1024 * 1024:
            raise PublishError("native payload exceeds 512 MiB")
    if sum((generation_dir / name).stat().st_size for name in files) > 1024 * 1024 * 1024:
        raise PublishError("generation payload exceeds 1 GiB")


def read_manifest(generation_dir: Path) -> dict[str, Any]:
    manifest_path = generation_dir / "manifest.json"
    if generation_dir.is_symlink() or manifest_path.is_symlink():
        raise PublishError("generation directory/manifest must not be symlinks")
    size = manifest_path.stat().st_size
    if size > MAX_MANIFEST_BYTES:
        raise PublishError(f"manifest exceeds the 64 KiB bound: {size}")
    try:
        from play_documents import decode
        data = decode(manifest_path.read_bytes())
        if isinstance(data, dict) and data.get("schema") == 2 and "host_symbol_file" not in data:
            # Existing ELF generations have embedded symbols and no host sidecar.
            data = {**data, "schema": MANIFEST_SCHEMA, "host_symbol_file": None}
        _validate_manifest(generation_dir, data, generation_dir.name)
    except (ValueError, TypeError) as exc:
        raise PublishError("malformed generation manifest") from exc
    # Re-verify payload hashes before a generation is trusted for load.
    for rel, expected in data.get("files", {}).items():
        actual = sha256_file(generation_dir / rel)
        if actual != expected:
            raise PublishError(f"payload hash mismatch for {rel}; corrupt generation")
    return data


class LeaseSet:
    """Tracks which generations are leased (active/candidate/debugger).

    GC never deletes a leased generation and never deletes the active/candidate/
    previous within the retention window. A crashed session's lease is only
    reclaimable after its process is proven ended (checked by the caller).
    """

    def __init__(self) -> None:
        self._leases: dict[str, int] = {}

    def acquire(self, generation_id: str) -> None:
        self._leases[generation_id] = self._leases.get(generation_id, 0) + 1

    def release(self, generation_id: str) -> None:
        count = self._leases.get(generation_id, 0)
        if count > 1:
            self._leases[generation_id] = count - 1
        else:
            self._leases.pop(generation_id, None)

    def is_leased(self, generation_id: str) -> bool:
        return generation_id in self._leases


class GenerationLease:
    """OS-backed lease shared by supervisors and debugger launchers.

    Acquisition and GC use the publication lock in the same order. The shared
    lease lock remains held until explicit close or process exit; a crashed
    owner releases it automatically, without PID reuse heuristics.
    """
    def __init__(self, generation_dir: Path) -> None:
        self._handle = None
        if generation_dir.is_symlink():
            raise PublishError("generation lease may not follow a directory symlink")
        generation_dir = generation_dir.resolve()
        root = generation_dir.parent
        with (root / ".publication.lock").open("a+b") as publication:
            lock_file(publication)
            read_manifest(generation_dir)
            lease_dir = root / ".leases"
            lease_dir.mkdir(exist_ok=True)
            self._handle = open_guard(lease_dir / generation_dir.name)

    def close(self) -> None:
        if self._handle is not None:
            self._handle.close()
            self._handle = None

    def fileno(self) -> int:
        """Inherit this open description into an owned probe/host.

        Its shared flock then survives supervisor loss until every inheriting
        process exits. Closing one reference does not unlock the others.
        """
        if self._handle is None:
            raise PublishError("generation lease is closed")
        return self._handle.fileno()

    def __enter__(self) -> "GenerationLease":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()


class GenerationStoreLease:
    """Conservative host/debugger lease inherited across all code replacements.

    A process cannot inherit a newly opened per-generation fd after exec without
    descriptor passing. The host therefore also inherits one shared store lock:
    if its supervisor disappears after a reload, every mapped image and its
    source symbols remain available until that host actually exits. Publication
    remains unlocked; collection is deferred for the lifetime of a play host.
    """
    def __init__(self, root: Path) -> None:
        lease_dir = root / ".leases"
        lease_dir.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            with (root / ".publication.lock").open("a+b") as publication:
                lock_file(publication)
                self._handle = open_guard(lease_dir / ".store")
        else:
            self._handle = open_guard(lease_dir / ".store", blocking=False)

    def fileno(self) -> int:
        return self._handle.fileno()

    def close(self) -> None:
        self._handle.close()


def collect_generations(
    generations_root: Path,
    leases: LeaseSet,
    *,
    retention: int = DEFAULT_RETENTION,
) -> list[str]:
    """Collect verified old generations under the publication and lease locks.

    Invalid/unrelated directories are left alone. Deletion errors are failures,
    never reported as successful collection. Retention must keep at least the
    current, candidate and previous published images.
    """
    if retention < DEFAULT_RETENTION:
        raise PublishError("retention must keep at least three generations")
    if not generations_root.is_dir():
        return []
    with (generations_root / ".publication.lock").open("a+b") as publication:
        lock_file(publication)
        gens = sorted(
            p.name for p in generations_root.iterdir()
            if p.is_dir() and not p.is_symlink()
            and re.fullmatch(r"[0-9a-f]{16}-[0-9a-f]{16}", p.name)
        )
        lease_dir = generations_root / ".leases"
        lease_dir.mkdir(exist_ok=True)
        if os.name == "nt":
            try:
                assert_unleased(lease_dir / ".store")
            except BlockingIOError:
                return []
            return _collect_unleased(generations_root, lease_dir, gens[:-retention], leases)
        with (lease_dir / ".store").open("a+b") as store:
            try:
                lock_file(store, blocking=False)
            except BlockingIOError:
                return []
            return _collect_unleased(generations_root, lease_dir, gens[:-retention], leases)


def _collect_unleased(generations_root: Path, lease_dir: Path, generations: list[str], leases: LeaseSet) -> list[str]:
    deleted: list[str] = []
    for gen_id in generations:
        if leases.is_leased(gen_id):
            continue
        lease_path = lease_dir / gen_id
        if os.name == "nt":
            try:
                assert_unleased(lease_path)
            except BlockingIOError:
                continue
            if _delete_generation(generations_root / gen_id):
                lease_path.unlink(missing_ok=True)
                deleted.append(gen_id)
            continue
        with lease_path.open("a+b") as handle:
            try:
                lock_file(handle, blocking=False)
            except BlockingIOError:
                continue
            if _delete_generation(generations_root / gen_id):
                deleted.append(gen_id)
                lease_path.unlink()
    return deleted


def _delete_generation(target: Path) -> bool:
    try:
        read_manifest(target)
    except (PublishError, OSError):
        return False
    # Re-enable writes after claiming the lease and verifying the manifest.
    os.chmod(target, 0o755)
    if os.name == "nt":
        for child in target.iterdir():
            os.chmod(child, 0o644)
    shutil.rmtree(target)
    return True
