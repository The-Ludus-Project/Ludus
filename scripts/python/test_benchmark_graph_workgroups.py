"""Reject unavailable/misidentified timings and incomplete raw graph output."""
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import benchmark_graph_workgroups as benchmark


class GraphWorkgroupReaderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'sample.bin'

    def metadata(self, **changes):
        record = dict(kind=1, identity=1, logical_count=1, scope_index=0,
                      bits=64, tick=1.0, name=b'graph/primary'.ljust(128, b'\0'))
        record.update(changes)
        return benchmark.METADATA_HEADER.pack(1, 1) + benchmark.METADATA_RECORD.pack(*record.values())

    def test_valid_precision_and_scope_survive(self):
        self.path.write_bytes(self.metadata())
        self.assertEqual('graph/primary', benchmark.read_metadata(self.path, 1)[0]['name'])
        # Zero precision means unavailable precision, while duration availability is independent.
        self.path.write_bytes(self.metadata(bits=0, tick=0))
        self.assertEqual(0, benchmark.read_metadata(self.path, 1)[0]['valid_bits'])

    def test_invalid_metadata_rejects(self):
        for changes in ({'kind': 0}, {'identity': 0}, {'logical_count': 0}, {'scope_index': 1},
                        {'bits': 65}, {'tick': -1.0}, {'tick': math.nan},
                        {'name': b'x' * 128}, {'name': b'a\0b'.ljust(128, b'\0')}):
            with self.subTest(changes=changes):
                self.path.write_bytes(self.metadata(**changes))
                with self.assertRaises(ValueError):
                    benchmark.read_metadata(self.path, 1)

    def test_truncated_and_trailing_metadata_reject(self):
        for data in (self.metadata()[:-1], self.metadata() + b'\0', b''):
            self.path.write_bytes(data)
            with self.assertRaises(ValueError):
                benchmark.read_metadata(self.path, 1)

    def test_raw_sample_count_is_integer_bits_and_rgb_must_be_finite(self):
        with patch.object(benchmark, 'EXTENT', 1), patch.object(benchmark, 'SAMPLES', 4):
            data = struct.pack('<fffIffff', 1, 2, 3, 4, 0, 0, 0, 0)
            self.path.write_bytes(data)
            self.assertEqual(data, benchmark.read_raw(self.path)[0])
            for invalid in (struct.pack('<fffIffff', 1, 2, 3, 3, 0, 0, 0, 0),
                            struct.pack('<fffIffff', math.inf, 2, 3, 4, 0, 0, 0, 0), data[:-1]):
                self.path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    benchmark.read_raw(self.path)


if __name__ == '__main__':
    unittest.main()
