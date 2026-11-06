#!/usr/bin/env python3
"""Synthetic PE/PDB images for the Windows live-play symbol check (play_windows)."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True

import play_windows  # noqa: E402
from play_probe import ProbeError, native_identity  # noqa: E402

GUID = bytes(range(1, 17))
OTHER_GUID = bytes(range(17, 33))
SECTION_RVA = 0x1000
SECTION_OFFSET = 0x200
SECTION_BYTES = 0x200


def make_pe(guid=GUID, age=3, *, records=1, characteristics=0x2022, magic=0x20B):
    """One .rdata section holding the debug directory and its RSDS records."""
    image = bytearray(SECTION_OFFSET + SECTION_BYTES)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    optional_size = 112 + 16 * 8
    struct.pack_into("<HHIIIHH", image, 0x44, 0x8664, 1, 0, 0, 0, optional_size, characteristics)
    optional = 0x44 + 20
    struct.pack_into("<H", image, optional, magic)
    struct.pack_into("<I", image, optional + 108, 16)
    struct.pack_into("<II", image, optional + 112 + 6 * 8, SECTION_RVA, 28 * records)
    section = optional + optional_size
    image[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", image, section + 8, SECTION_BYTES, SECTION_RVA, SECTION_BYTES, SECTION_OFFSET)
    record = b"RSDS" + guid + struct.pack("<I", age) + b"C:\\build\\game.pdb\0"
    data = SECTION_OFFSET + 28 * records
    for index in range(records):
        struct.pack_into("<IIHHIIII", image, SECTION_OFFSET + index * 28, 0, 0, 0, 0, 2, len(record),
                         SECTION_RVA + data - SECTION_OFFSET, data)
    image[data:data + len(record)] = record
    return bytes(image)


def make_pdb(guid=GUID, info_age=3, dbi_age=3, *, has_dbi=True):
    """Block size 512: superblock, two map blocks, info, DBI, directory and its block map."""
    block = 512
    blocks = [bytearray(block) for _ in range(7)]
    blocks[3][0:28] = struct.pack("<III", 20000404, 0, info_age) + guid
    blocks[4][0:12] = struct.pack("<iII", -1, 19990903, dbi_age)
    dbi_size = 64 if has_dbi else 0
    directory = struct.pack("<I", 4) + struct.pack("<4I", 0, 28, 0, dbi_size) + struct.pack("<I", 3)
    if has_dbi:
        directory += struct.pack("<I", 4)
    blocks[5][0:len(directory)] = directory
    blocks[6][0:4] = struct.pack("<I", 5)
    superblock = play_windows.MSF_MAGIC + struct.pack("<6I", block, 1, len(blocks), len(directory), 0, 6)
    blocks[0][0:len(superblock)] = superblock
    return b"".join(bytes(item) for item in blocks)


class WindowsSymbolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_matching_pdb_identifies_a_module(self):
        module = self.write("game.dll", make_pe())
        self.write("game.pdb", make_pdb())
        result = play_windows.identity(module)
        self.assertEqual(result["artifact_type"], "module")
        self.assertEqual(result["build_id"], GUID.hex() + "3")
        self.assertTrue(result["matching_symbols"])
        self.assertFalse(result["embedded_symbols"])

    def test_executable_is_a_host(self):
        host = self.write("host.exe", make_pe(characteristics=0x0022))
        self.assertEqual(play_windows.identity(host, self.write("host.pdb", make_pdb()))["artifact_type"], "host")

    def test_dbi_age_is_authoritative_over_the_info_stream(self):
        module = self.write("game.dll", make_pe(age=3))
        self.write("game.pdb", make_pdb(info_age=5, dbi_age=3))
        self.assertTrue(play_windows.identity(module)["matching_symbols"])
        self.write("game.pdb", make_pdb(info_age=3, has_dbi=False))
        self.assertTrue(play_windows.identity(module)["matching_symbols"])

    def test_foreign_or_stale_pdb_is_rejected(self):
        module = self.write("game.dll", make_pe())
        for pdb in (make_pdb(guid=OTHER_GUID), make_pdb(info_age=4, dbi_age=4)):
            self.write("game.pdb", pdb)
            with self.assertRaisesRegex(ProbeError, "GUID/age"):
                play_windows.identity(module)

    def test_missing_pdb_is_rejected(self):
        module = self.write("game.dll", make_pe())
        with self.assertRaisesRegex(ProbeError, "matching PDB"):
            play_windows.identity(module)

    def test_malformed_images_are_rejected(self):
        cases = {"not a PE": b"MZ" + bytes(126),
                 "PE32": make_pe(magic=0x10B),
                 "zero GUID": make_pe(guid=bytes(16)),
                 "two records": make_pe(records=2)}
        for label, data in cases.items():
            with self.subTest(label), self.assertRaises(ProbeError):
                play_windows.inspect(self.write("bad.dll", data))
        pdb = bytearray(make_pdb())
        pdb[0:4] = b"XXXX"
        with self.assertRaisesRegex(ProbeError, "MSF"):
            play_windows.pdb_identity(self.write("bad.pdb", bytes(pdb)))
        truncated = make_pdb()[:512 * 4]
        with self.assertRaises(ProbeError):
            play_windows.pdb_identity(self.write("short.pdb", truncated))

    def test_native_identity_selects_the_windows_reader(self):
        module = self.write("game.dll", make_pe())
        self.write("game.pdb", make_pdb())
        with patch.object(sys, "platform", "win32"):
            self.assertEqual(native_identity(module)["artifact_type"], "module")


if __name__ == "__main__":
    unittest.main()
