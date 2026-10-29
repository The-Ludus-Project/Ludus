"""Independent regressions for fixed ray bindings and raw reference export boundaries."""
import importlib.util
from importlib.machinery import SourceFileLoader
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'cmake/shaders'))
import compile_ray  # noqa: E402

loader = SourceFileLoader('reference_hdr', str(ROOT / 'scripts/reference-hdr'))
spec = importlib.util.spec_from_loader(loader.name, loader)
hdr = importlib.util.module_from_spec(spec)
loader.exec_module(hdr)


class RayCookTests(unittest.TestCase):
    def test_bindings_must_preserve_resource_identity(self):
        source = 'intersection_query q; kernel void main0(' + ','.join(
            f'float4 {name} [[buffer({index})]]' for index, name in enumerate(('scene','rays','hits','opacity','parameters'))) + ') {}'
        self.assertEqual(compile_ray.verify_metal(source), 'main0')
        with self.assertRaises(RuntimeError):
            compile_ray.verify_metal(source.replace('scene [[buffer(0)]]', 'scene [[buffer(4)]]').replace('parameters [[buffer(4)]]','parameters [[buffer(0)]]'))
        with self.assertRaises(RuntimeError):
            compile_ray.verify_metal(source + 'kernel void other() {}')

    def test_malformed_instruction_rejects_before_shader_publication(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'bad.spv'
            path.write_bytes(struct.pack('<6I',0x07230203,0x10500,0,1,0,17))
            with self.assertRaises(RuntimeError):
                compile_ray.verify_spirv(path)


class ReferenceExportTests(unittest.TestCase):
    def capture(self):
        data = bytearray(256)
        data[:8] = b'LUHDR01\0'
        struct.pack_into('<10I',data,8,1,1,1,256,2,1,4,3,2,32)
        return data + struct.pack('<fffIfffI',1,2,3,4,0,0,0,0)

    def test_partial_counts_and_radiance_are_preserved(self):
        metadata, pixels = hdr.decode(self.capture())
        self.assertTrue(metadata['partial'])
        self.assertEqual(metadata['minimum_samples'],0)
        self.assertEqual(pixels[0],(1,2,3,4))

    def test_surface_profile_version_is_preserved(self):
        for profile in (1, 2, 3, 4):
            with self.subTest(profile=profile):
                data = self.capture()
                struct.pack_into('<I', data, 12, profile)
                metadata, pixels = hdr.decode(data)
                self.assertEqual(metadata['profile'], profile)
                self.assertEqual(pixels[0], (1, 2, 3, 4))

    def test_traversal_flags_name_each_implementation(self):
        for flags, traversal in ((32, 'native_queries'), (33, 'cpu_oracle'), (96, 'gpu_compute')):
            with self.subTest(flags=flags):
                data = self.capture()
                struct.pack_into('<I', data, 44, flags)
                metadata, _ = hdr.decode(data)
                self.assertEqual(metadata['traversal'], traversal)
        data = self.capture()
        struct.pack_into('<I', data, 44, 97)
        with self.assertRaises(ValueError): hdr.decode(data)
        struct.pack_into('<I', data, 44, 288)
        with self.assertRaises(ValueError): hdr.decode(data)

    def test_light_sampling_pairs_the_sampler_version_with_its_flag(self):
        for sampler, flags, light_sampling in ((1, 96, 'uniform_area'), (2, 224, 'resampled_projected_solid_angle'),
                                               (2, 161, 'resampled_projected_solid_angle')):
            with self.subTest(sampler=sampler, flags=flags):
                data = self.capture()
                struct.pack_into('<I', data, 16, sampler)
                struct.pack_into('<I', data, 44, flags)
                metadata, _ = hdr.decode(data)
                self.assertEqual(metadata['sampler'], sampler)
                self.assertEqual(metadata['light_sampling'], light_sampling)
        # A flag without its sampler version, the version without its flag, or an unknown version is refused.
        for sampler, flags in ((1, 160), (2, 32), (3, 160)):
            with self.subTest(sampler=sampler, flags=flags):
                data = self.capture()
                struct.pack_into('<I', data, 16, sampler)
                struct.pack_into('<I', data, 44, flags)
                with self.assertRaises(ValueError): hdr.decode(data)

    def test_complete_short_nonfinite_and_unknown_profile_reject(self):
        data = self.capture()
        struct.pack_into('<I',data,44,0)
        with self.assertRaises(ValueError): hdr.decode(data)
        data = self.capture()
        struct.pack_into('<f',data,256,float('nan'))
        with self.assertRaises(ValueError): hdr.decode(data)
        data = self.capture()
        struct.pack_into('<I',data,12,5)
        with self.assertRaises(ValueError): hdr.decode(data)
        with self.assertRaises(ValueError): hdr.decode(self.capture()[:-1])


if __name__ == '__main__':
    unittest.main()
