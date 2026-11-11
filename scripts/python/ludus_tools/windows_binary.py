"""Bounded, non-executing Windows x64 image and matching PDB inspection."""
from __future__ import annotations

import struct
from pathlib import Path

from .release_model import fail

# Thanks to Microsoft, "PE Format", Optional Header, Import/Delay Import and
# Debug Directory: https://learn.microsoft.com/windows/win32/debug/pe-format
# and LLVM contributors, "The MSF File Format" and "The PDB Info Stream":
# https://llvm.org/docs/PDB/MsfFile.html and https://llvm.org/docs/PDB/PdbStream.html
# We read only bounded loader metadata and the PDB identity, never load an image.
MAX_METADATA = 16 * 1024 * 1024
MAX_NAME = 4096
MAX_SECTIONS = 96
MAX_IMPORTS = 4096
MAX_DEBUG_RECORDS = 256
DEBUG_RECORD_BYTES = 28
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


class BinaryReader:
    def __init__(self, handle):
        self.handle = handle
        self.size = handle.seek(0, 2)

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or size > MAX_METADATA or offset + size > self.size:
            fail("invalid Windows binary extent; rebuild the Release artifact", "InvalidPackage")
        self.handle.seek(offset)
        data = self.handle.read(size)
        if len(data) != size:
            fail("truncated Windows binary; rebuild the Release artifact", "InvalidPackage")
        return data


def inspect_image(path: Path) -> dict:
    """Return imports and optional RSDS identity from an AMD64 PE32+ image."""
    with path.open("rb") as handle:
        reader = BinaryReader(handle)
        if reader.read(0, 2) != b"MZ":
            fail(f"{path.name} is not a PE image; rebuild for Windows x64", "InvalidPackage")
        offset = struct.unpack("<I", reader.read(60, 4))[0]
        header = reader.read(offset, 24)
        machine, count = struct.unpack_from("<HH", header, 4)
        optional_size, flags = struct.unpack_from("<HH", header, 20)
        if (header[:4] != b"PE\0\0" or machine != 0x8664 or not flags & 2
                or not 1 <= count <= MAX_SECTIONS or optional_size < 240):
            fail("expected an executable AMD64 PE32+ image", "InvalidPackage")
        optional = reader.read(offset + 24, optional_size)
        if struct.unpack_from("<H", optional)[0] != 0x20b:
            fail("expected PE32+ optional header", "InvalidPackage")
        directory_count = struct.unpack_from("<I", optional, 108)[0]
        if directory_count < 14 or 112 + directory_count * 8 > optional_size:
            fail("invalid PE directory table", "InvalidPackage")
        sections = []
        for index in range(count):
            section = reader.read(offset + 24 + optional_size + index * 40, 40)
            virtual_size, address, size, raw = struct.unpack_from("<IIII", section, 8)
            if raw + size > reader.size:
                fail("PE section exceeds file", "InvalidPackage")
            sections.append((address, virtual_size, raw, size))

        def locate(address, size):
            matches = [raw + address - start for start, _, raw, extent in sections
                       if start <= address and address + size <= start + extent]
            if len(matches) != 1:
                fail("PE RVA is unmapped or ambiguous", "InvalidPackage")
            return matches[0]

        def name_at(address):
            data = bytearray()
            for index in range(MAX_NAME):
                value = reader.read(locate(address + index, 1), 1)
                if value == b"\0":
                    try:
                        return data.decode("ascii").lower()
                    except UnicodeError:
                        break
                data.extend(value)
            fail("invalid PE import name", "InvalidPackage")

        imports = set()
        for directory_index, stride, name_offset in ((1, 20, 12), (13, 32, 4)):
            address, size = struct.unpack_from("<II", optional, 112 + directory_index * 8)
            if address == 0 and size == 0:
                continue
            if size < stride or size > (MAX_IMPORTS + 1) * stride:
                fail("invalid PE import directory", "InvalidPackage")
            table = reader.read(locate(address, size), size)
            terminated = False
            for start in range(0, size - stride + 1, stride):
                record = table[start:start + stride]
                if record == bytes(stride):
                    terminated = True
                    break
                if directory_index == 13 and struct.unpack_from("<I", record)[0] != 1:
                    fail("delay imports must use RVAs", "InvalidPackage")
                imports.add(name_at(struct.unpack_from("<I", record, name_offset)[0]))
            if not terminated:
                fail("unterminated PE imports", "InvalidPackage")
        debug_address, debug_size = struct.unpack_from("<II", optional, 112 + 6 * 8)
        identity = None
        if debug_size:
            if debug_size % DEBUG_RECORD_BYTES or debug_size > MAX_DEBUG_RECORDS * DEBUG_RECORD_BYTES:
                fail("invalid PE debug directory", "InvalidPackage")
            debug = reader.read(locate(debug_address, debug_size), debug_size)
            for start in range(0, debug_size, DEBUG_RECORD_BYTES):
                kind, size, _, raw = struct.unpack_from("<IIII", debug, start + 12)
                if kind != 2:
                    continue
                value = reader.read(raw, size)
                if size < 25 or size > MAX_NAME or value[:4] != b"RSDS" or value[-1:] != b"\0" or identity is not None:
                    fail("expected one bounded RSDS debug identity", "InvalidPackage")
                identity = {"guid": value[4:20].hex(), "age": struct.unpack_from("<I", value, 20)[0]}
        return {"imports": sorted(imports), "pdb": identity, "isDll": bool(flags & 0x2000)}


def inspect_pdb(path: Path) -> dict:
    """Read stream 1's GUID/age from an MSF 7 PDB without reading symbol data."""
    with path.open("rb") as handle:
        reader = BinaryReader(handle)
        header = reader.read(0, 56)
        if header[:32] != MSF_MAGIC:
            fail("expected an MSF 7 PDB; link with /DEBUG:FULL", "InvalidPackage")
        block_size, _, block_count, directory_size, _, block_map = struct.unpack_from("<IIIIII", header, 32)
        if (block_size not in (512, 1024, 2048, 4096) or block_count * block_size != reader.size
                or not 12 <= directory_size <= MAX_METADATA):
            fail("invalid PDB superblock", "InvalidPackage")
        directory_blocks = (directory_size + block_size - 1) // block_size
        if directory_blocks * 4 > block_size:
            fail("PDB directory exceeds supported block map", "InvalidPackage")

        def block(number):
            if number >= block_count:
                fail("PDB block exceeds file", "InvalidPackage")
            return reader.read(number * block_size, block_size)

        mapping = block(block_map)
        directory = b"".join(block(struct.unpack_from("<I", mapping, index * 4)[0])
                             for index in range(directory_blocks))[:directory_size]
        count = struct.unpack_from("<I", directory)[0]
        if not 2 <= count <= (len(directory) - 4) // 4:
            fail("invalid PDB stream count", "InvalidPackage")
        sizes = struct.unpack_from(f"<{count}I", directory, 4)
        cursor = 4 + count * 4
        for size in sizes[:1]:
            if size != 0xffffffff:
                cursor += ((size + block_size - 1) // block_size) * 4
        if not 28 <= sizes[1] <= MAX_METADATA or cursor + 4 > len(directory):
            fail("invalid PDB info stream", "InvalidPackage")
        info = block(struct.unpack_from("<I", directory, cursor)[0])
        return {"guid": info[12:28].hex(), "age": struct.unpack_from("<I", info, 8)[0]}
