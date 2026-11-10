"""Read-only play metadata and explicitly saved tuning documents.

Opening these files executes no project code. IDs are stable 64-bit hex values;
JSON is a document format, never a dump of the module's in-memory records.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

if os.name == "nt":
    from editor_process_windows import lock_file
else:
    import fcntl

MAX_DOCUMENT_BYTES = 256 * 1024
MAX_PATH_BYTES = 4096
TARGET = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]{0,127}\Z")
ID = re.compile(r"[0-9a-f]{16}\Z")
KINDS = ("bool", "int32", "float32", "enum", "string")


class DocumentError(ValueError):
    """Invalid metadata or a conflicting save; existing documents remain intact."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise DocumentError(f"duplicate field: {key}")
        result[key] = value
    return result


def _reject_constant(_text: str) -> None:
    raise DocumentError("nonfinite number")


def read_bounded(path: Path) -> bytes:
    with path.open("rb") as handle:
        data = handle.read(MAX_DOCUMENT_BYTES + 1)
    if len(data) > MAX_DOCUMENT_BYTES:
        raise DocumentError("document exceeds 256 KiB")
    return data


def decode(data: bytes) -> dict:
    if len(data) > MAX_DOCUMENT_BYTES:
        raise DocumentError("document exceeds 256 KiB")
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
    except (UnicodeError, ValueError) as exc:
        raise DocumentError(f"invalid document: {exc}") from exc
    if not isinstance(value, dict):
        raise DocumentError("document must be an object")
    return value


def _fields(value: Any, required: set[str], optional: set[str] | None = None) -> None:
    if not isinstance(value, dict) or not required <= value.keys() or value.keys() - required - (optional or set()):
        raise DocumentError(f"expected fields {sorted(required)}, optional {sorted(optional or set())}")


def _version(value: Any) -> None:
    if type(value) is not int or value != 1:
        raise DocumentError("unsupported document version")


def contained_path(root: Path, text: Any) -> Path:
    if not isinstance(text, str) or not text or "\0" in text or len(text.encode("utf-8")) > MAX_PATH_BYTES:
        raise DocumentError("invalid project-relative path")
    path = Path(text)
    if path.is_absolute() or ".." in path.parts:
        raise DocumentError("path must remain inside the project")
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise DocumentError("path or symlink escapes the project")
    return resolved


@dataclass(frozen=True)
class PlayDescriptor:
    host_target: str
    module_target: str
    startup_document: Path | None
    watch_roots: tuple[Path, ...]


def read_play_descriptor(project_dir: Path) -> PlayDescriptor | None:
    path = project_dir / "ludus.play.json"
    if not path.exists():
        return None
    if path.is_symlink():
        raise DocumentError("play descriptor may not be a symlink")
    obj = decode(read_bounded(path))
    _fields(obj, {"version", "host_target", "module_target"}, {"startup_document", "watch_roots"})
    _version(obj["version"])
    for key in ("host_target", "module_target"):
        if not isinstance(obj[key], str) or not TARGET.fullmatch(obj[key]):
            raise DocumentError(f"invalid {key}")
    if obj["host_target"] == obj["module_target"]:
        raise DocumentError("host and module targets must be distinct")
    roots = obj.get("watch_roots", [])
    if not isinstance(roots, list) or len(roots) > 16:
        raise DocumentError("at most 16 watch roots are supported")
    paths = tuple(contained_path(project_dir, text) for text in roots)
    if len(set(paths)) != len(paths):
        raise DocumentError("duplicate watch root")
    startup = contained_path(project_dir, obj["startup_document"]) if "startup_document" in obj else None
    return PlayDescriptor(obj["host_target"], obj["module_target"], startup, paths)


def _id(value: Any) -> str:
    if not isinstance(value, str) or not ID.fullmatch(value) or value == "0000000000000000":
        raise DocumentError("expected a nonzero 16-character lowercase hex id")
    return value


def validate_value(kind: str, value: Any) -> None:
    if kind == "bool":
        valid = type(value) is bool
    elif kind == "int32":
        valid = type(value) is int and -(2**31) <= value < 2**31
    elif kind == "enum":
        valid = type(value) is int and 0 <= value < 2**31
    elif kind == "float32":
        valid = type(value) in (int, float) and abs(value) <= 3.4028234663852886e38 and math.isfinite(value)
    elif kind == "string":
        try:
            valid = isinstance(value, str) and "\0" not in value and len(value.encode("utf-8")) <= 256
        except UnicodeError:
            valid = False
    else:
        valid = False
    if not valid:
        raise DocumentError(f"invalid {kind} value")


def validate_tuning(obj: dict) -> dict:
    _fields(obj, {"version", "game", "objects"})
    _version(obj["version"])
    _id(obj["game"])
    objects = obj["objects"]
    if not isinstance(objects, list) or len(objects) > 256:
        raise DocumentError("at most 256 objects are supported")
    seen_objects = set()
    total = 0
    for item in objects:
        _fields(item, {"id", "properties"})
        object_id = _id(item["id"])
        if object_id in seen_objects:
            raise DocumentError("duplicate object id")
        seen_objects.add(object_id)
        properties = item["properties"]
        if not isinstance(properties, list):
            raise DocumentError("properties must be a list")
        total += len(properties)
        if total > 4096:
            raise DocumentError("at most 4096 properties are supported")
        seen_properties = set()
        for prop in properties:
            _fields(prop, {"id", "kind", "value"})
            property_id = _id(prop["id"])
            if property_id in seen_properties:
                raise DocumentError("duplicate property id")
            seen_properties.add(property_id)
            if prop["kind"] not in KINDS:
                raise DocumentError("unsupported property kind")
            validate_value(prop["kind"], prop["value"])
    return copy.deepcopy(obj)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class TuningDocument:
    """Independent document undo; runtime edits reach this owner only explicitly."""

    def __init__(self, path: Path) -> None:
        self.path = path
        if path.is_symlink():
            raise DocumentError("tuning document may not be a symlink")
        data = read_bounded(path)
        self.saved_digest = digest(data)
        self.saved = validate_tuning(decode(data))
        self.draft = copy.deepcopy(self.saved)
        self._undo: list[dict] = []
        self._redo: list[dict] = []

    @property
    def dirty(self) -> bool:
        return self.draft != self.saved

    @property
    def can_undo(self) -> bool:
        return bool(self._undo)

    @property
    def can_redo(self) -> bool:
        return bool(self._redo)

    def apply(self, edits: list[dict], *, expected_digest: str) -> None:
        if expected_digest != digest(self.encoded()):
            raise DocumentError("document draft changed; refresh before applying")
        if not 0 < len(edits) <= 64:
            raise DocumentError("edit batch must contain 1..64 properties")
        candidate = copy.deepcopy(self.draft)
        seen = set()
        for edit in edits:
            _fields(edit, {"object", "property", "kind", "value"})
            pair = (_id(edit["object"]), _id(edit["property"]))
            if pair in seen:
                raise DocumentError("duplicate edit")
            seen.add(pair)
            found = [p for o in candidate["objects"] if o["id"] == pair[0]
                     for p in o["properties"] if p["id"] == pair[1]]
            if len(found) != 1 or found[0]["kind"] != edit["kind"]:
                raise DocumentError("object/property/schema changed")
            validate_value(edit["kind"], edit["value"])
            found[0]["value"] = edit["value"]
        if candidate != self.draft:
            self._undo.append(self.draft)
            self._undo = self._undo[-128:]
            self._redo.clear()
            self.draft = candidate

    def undo(self) -> bool:
        if not self._undo:
            return False
        self._redo.append(self.draft)
        self.draft = self._undo.pop()
        return True

    def redo(self) -> bool:
        if not self._redo:
            return False
        self._undo.append(self.draft)
        self.draft = self._redo.pop()
        return True

    def discard(self) -> None:
        self.draft = copy.deepcopy(self.saved)
        self._undo.clear()
        self._redo.clear()

    def encoded(self) -> bytes:
        data = (json.dumps(self.draft, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")
        if len(data) > MAX_DOCUMENT_BYTES:
            raise DocumentError("document exceeds 256 KiB")
        return data

    def save(self) -> None:
        # Cooperative per-document lock + exact disk revision; external writes
        # that ignore this lock remain a documented non-hermetic limitation.
        if self.path.is_symlink():
            raise DocumentError("tuning document may not be a symlink")
        with self.path.with_name(self.path.name + ".lock").open("a+b") as lock:
            if os.name == "nt":
                lock_file(lock)
            else:
                fcntl.flock(lock, fcntl.LOCK_EX)
            if digest(read_bounded(self.path)) != self.saved_digest:
                raise DocumentError("document changed on disk; reload or resolve the conflict")
            data = self.encoded()
            fd, name = tempfile.mkstemp(prefix=".tuning-", dir=self.path.parent)
            try:
                with os.fdopen(fd, "wb") as handle:
                    handle.write(data)
                    handle.flush()
                    os.fsync(handle.fileno())
                os.replace(name, self.path)
                if os.name != "nt":
                    directory_fd = os.open(self.path.parent, os.O_DIRECTORY)
                    try:
                        os.fsync(directory_fd)
                    finally:
                        os.close(directory_fd)
            finally:
                if os.path.exists(name):
                    os.unlink(name)
            self.saved = copy.deepcopy(self.draft)
            self.saved_digest = digest(data)


def encode_authored(obj: dict) -> bytes:
    """Encode a saved, validated document without native structure layout."""
    import struct
    obj = validate_tuning(obj)
    records = [(o["id"], p) for o in obj["objects"] for p in o["properties"]]
    payload = bytearray(b"LTUN" + struct.pack("<IQI", 1, int(obj["game"], 16), len(records)))
    for object_id, prop in records:
        kind = KINDS.index(prop["kind"])
        value = prop["value"]
        encoded = (bytes([value]) if kind == 0 else struct.pack("<i", value) if kind in (1, 3)
                   else struct.pack("<f", value) if kind == 2 else value.encode("utf-8"))
        payload.extend(struct.pack("<QQII", int(object_id, 16), int(prop["id"], 16), kind, len(encoded)))
        payload.extend(encoded)
    if len(payload) > MAX_DOCUMENT_BYTES:
        raise DocumentError("authored transport exceeds 256 KiB")
    return bytes(payload)
