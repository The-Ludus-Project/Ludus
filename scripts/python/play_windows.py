"""Bounded PE CodeView and PDB identity checks for native live play on Windows.

A Windows generation carries its symbols in a detached PDB beside the DLL. The PE's
CodeView RSDS record names the GUID and age that its PDB must carry; a stale or foreign
PDB would let a debugger show the wrong source, so publication requires an exact match.
"""
from __future__ import annotations
import struct
from pathlib import Path

# Thanks to Microsoft, "PE Format" (https://learn.microsoft.com/windows/win32/debug/pe-format),
# sections "Optional Header Data Directories", "Section Table" and "The .debug Section": the
# headers, section mapping and IMAGE_DEBUG_TYPE_CODEVIEW directory read here.
# Thanks to the LLVM project, "The PDB File Format" (https://llvm.org/docs/PDB/index.html),
# "The MSF File Format", "The PDB Info Stream" and "The DBI Stream": the superblock, stream
# directory and the GUID/age records compared here. These layouts inform an independent reader.

MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
MAX_PDB_BYTES = 4 * 1024 * 1024 * 1024
MAX_SECTIONS = 96
MAX_DEBUG_ENTRIES = 64
MAX_PDB_STREAMS = 65536
MAX_PDB_DIRECTORY_BYTES = 64 * 1024 * 1024
PE_SIGNATURE = b"PE\0\0"
PE32_MAGIC = 0x10B
PE32_PLUS_MAGIC = 0x20B
MACHINE_X64 = 0x8664
MACHINE_ARM64 = 0xAA64
IMAGE_FILE_DLL = 0x2000
IMAGE_DIRECTORY_ENTRY_DEBUG = 6
IMAGE_DEBUG_TYPE_CODEVIEW = 2
COFF_HEADER_BYTES = 20
SECTION_HEADER_BYTES = 40
DEBUG_ENTRY_BYTES = 28
RSDS_HEADER_BYTES = 24
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
MSF_SUPERBLOCK_BYTES = 56
MSF_BLOCK_SIZES = (512, 1024, 2048, 4096)
PDB_INFO_STREAM = 1
DBI_STREAM = 3
NIL_STREAM_SIZE = 0xFFFFFFFF


def _error(message: str):
    from play_errors import ProbeError
    return ProbeError(message)


class _Reader:
    def __init__(self, handle, size: int, label: str):
        self.handle, self.size, self.label = handle, size, label

    def read(self, offset: int, count: int) -> bytes:
        if offset < 0 or count < 0 or offset + count > self.size:
            raise _error(f"{self.label} reference outside artifact")
        self.handle.seek(offset)
        data = self.handle.read(count)
        if len(data) != count:
            raise _error(f"{self.label} changed or truncated during inspection")
        return data


def inspect(path: Path) -> dict:
    """Return the PE machine, DLL flag and CodeView GUID/age without loading the image."""
    size = path.stat().st_size
    if not 64 <= size <= MAX_ARTIFACT_BYTES:
        raise _error("PE artifact exceeds 512 MiB or is truncated")
    with path.open("rb") as handle:
        reader = _Reader(handle, size, "PE")
        if reader.read(0, 2) != b"MZ":
            raise _error("expected a PE executable or DLL")
        header = struct.unpack("<I", reader.read(0x3C, 4))[0]
        if reader.read(header, 4) != PE_SIGNATURE:
            raise _error("missing PE signature")
        machine, sections, _, _, _, optional_size, characteristics = struct.unpack(
            "<HHIIIHH", reader.read(header + 4, COFF_HEADER_BYTES))
        if machine not in (MACHINE_X64, MACHINE_ARM64) or not 0 < sections <= MAX_SECTIONS:
            raise _error("expected a 64-bit x64/ARM64 PE image with bounded sections")
        optional = header + 4 + COFF_HEADER_BYTES
        magic = struct.unpack("<H", reader.read(optional, 2))[0]
        if magic != PE32_PLUS_MAGIC:
            raise _error("expected a PE32+ optional header")
        # PE32+: NumberOfRvaAndSizes at 108, data directories from 112.
        directory_count = struct.unpack("<I", reader.read(optional + 108, 4))[0]
        if directory_count <= IMAGE_DIRECTORY_ENTRY_DEBUG or 112 + directory_count * 8 > optional_size:
            raise _error("PE image has no debug directory")
        debug_rva, debug_size = struct.unpack("<II", reader.read(optional + 112 + IMAGE_DIRECTORY_ENTRY_DEBUG * 8, 8))
        table = optional + optional_size
        mapped = []
        for index in range(sections):
            entry = reader.read(table + index * SECTION_HEADER_BYTES, SECTION_HEADER_BYTES)
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", entry, 8)
            mapped.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

        def file_offset(rva: int, count: int) -> int:
            for virtual_address, extent, raw_offset, raw_size in mapped:
                if virtual_address <= rva and rva + count <= virtual_address + min(extent, raw_size):
                    return raw_offset + rva - virtual_address
            raise _error("PE debug reference outside its sections")

        if debug_rva == 0 or debug_size == 0 or debug_size % DEBUG_ENTRY_BYTES or \
                debug_size // DEBUG_ENTRY_BYTES > MAX_DEBUG_ENTRIES:
            raise _error("PE image carries no bounded debug directory")
        directory = reader.read(file_offset(debug_rva, debug_size), debug_size)
        records = []
        for index in range(debug_size // DEBUG_ENTRY_BYTES):
            kind, data_size, _, data_offset = struct.unpack_from("<IIII", directory, index * DEBUG_ENTRY_BYTES + 12)
            if kind != IMAGE_DEBUG_TYPE_CODEVIEW:
                continue
            if not RSDS_HEADER_BYTES < data_size <= RSDS_HEADER_BYTES + 4096:
                raise _error("invalid CodeView record size")
            data = reader.read(data_offset, data_size)
            if data[:4] != b"RSDS":
                raise _error("CodeView record is not RSDS (PDB 7.0)")
            name = data[RSDS_HEADER_BYTES:].split(b"\0", 1)[0]
            records.append((data[4:20], struct.unpack_from("<I", data, 20)[0], name.decode("utf-8", "replace")))
        if len(records) != 1:
            raise _error("PE image must carry exactly one CodeView record")
        guid, age, pdb_name = records[0]
        if guid == bytes(16):
            raise _error("invalid zero CodeView GUID")
        return dict(guid=guid.hex(), age=age, pdb_name=pdb_name, machine=machine,
                    kind="module" if characteristics & IMAGE_FILE_DLL else "host")


def pdb_identity(path: Path) -> dict:
    """Return the GUID from the PDB info stream and the age the linker recorded for its image."""
    size = path.stat().st_size
    if not MSF_SUPERBLOCK_BYTES <= size <= MAX_PDB_BYTES:
        raise _error("PDB exceeds 4 GiB or is truncated")
    with path.open("rb") as handle:
        reader = _Reader(handle, size, "PDB")
        superblock = reader.read(0, MSF_SUPERBLOCK_BYTES)
        if superblock[:32] != MSF_MAGIC:
            raise _error("expected an MSF 7.0 program database")
        block_size, _, block_count, directory_bytes, _, block_map = struct.unpack_from("<6I", superblock, 32)
        if block_size not in MSF_BLOCK_SIZES or block_count * block_size > size or \
                not 0 < directory_bytes <= MAX_PDB_DIRECTORY_BYTES:
            raise _error("invalid MSF superblock")

        def block(index: int) -> bytes:
            if not 0 < index < block_count:
                raise _error("MSF block outside the file")
            return reader.read(index * block_size, block_size)

        def blocks_for(byte_count: int) -> int:
            return (byte_count + block_size - 1) // block_size

        directory_blocks = blocks_for(directory_bytes)
        if directory_blocks * 4 > block_size:
            raise _error("MSF stream directory map exceeds one block")
        indices = struct.unpack_from(f"<{directory_blocks}I", block(block_map))
        directory = b"".join(block(index) for index in indices)[:directory_bytes]
        stream_count = struct.unpack_from("<I", directory)[0]
        if not DBI_STREAM < stream_count <= MAX_PDB_STREAMS or 4 + stream_count * 4 > directory_bytes:
            raise _error("invalid MSF stream count")
        sizes = struct.unpack_from(f"<{stream_count}I", directory, 4)
        offset = 4 + stream_count * 4
        streams = []
        for stream_size in sizes:
            count = 0 if stream_size == NIL_STREAM_SIZE else blocks_for(stream_size)
            if offset + count * 4 > directory_bytes:
                raise _error("MSF stream directory is truncated")
            streams.append((stream_size, struct.unpack_from(f"<{count}I", directory, offset)))
            offset += count * 4

        def stream_prefix(index: int, count: int) -> bytes:
            stream_size, stream_blocks = streams[index]
            if stream_size == NIL_STREAM_SIZE or stream_size < count:
                raise _error(f"PDB stream {index} is missing or truncated")
            return b"".join(block(item) for item in stream_blocks[:blocks_for(count)])[:count]

        # PDB info stream: version, signature, age, GUID.
        _, _, info_age = struct.unpack("<III", stream_prefix(PDB_INFO_STREAM, 12))
        guid = stream_prefix(PDB_INFO_STREAM, 28)[12:28]
        # The DBI header's age is the one written with the image's CodeView record; the info
        # stream age may advance when the PDB alone is rewritten.
        dbi_size = streams[DBI_STREAM][0]
        age = struct.unpack_from("<I", stream_prefix(DBI_STREAM, 12), 8)[0] if dbi_size not in (0, NIL_STREAM_SIZE) \
            else info_age
        return dict(guid=guid.hex(), age=age)


def identity(path: Path, symbols: Path | None = None) -> dict:
    """Validate a PE image against its detached PDB (default: the sibling .pdb)."""
    image = inspect(path)
    symbols = symbols if symbols is not None else path.with_suffix(".pdb")
    if not symbols.is_file():
        raise _error("Windows artifact requires its matching PDB beside it")
    debug = pdb_identity(symbols)
    if debug["guid"] != image["guid"] or debug["age"] != image["age"]:
        raise _error("PDB GUID/age disagrees with the PE CodeView record")
    return {"build_id": image["guid"] + format(image["age"], "x"), "machine": image["machine"],
            "embedded_symbols": False, "matching_symbols": True, "artifact_type": image["kind"]}
