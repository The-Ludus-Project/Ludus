"""Checkpoint adapter admission, serialization and independent BatchNorm parity."""
import importlib.util
import pickle
import struct
import tempfile
import unittest
from pathlib import Path

from ludus_tools import light_transformer as adapter

ROOT = Path(__file__).resolve().parents[2]
LAYOUT = ROOT / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/detail/light_transformer_layout.h'
HAS_TORCH = importlib.util.find_spec('torch') is not None and importlib.util.find_spec('numpy') is not None


class LayoutTests(unittest.TestCase):
    def test_shared_layout_is_available_without_ml_dependencies(self):
        layout = adapter.read_layout(LAYOUT)
        self.assertEqual(layout['DIMENSION'], 128)
        self.assertEqual(layout['DEPTH'], 6)
        self.assertEqual(layout['NEIGHBORS'], 32)
        self.assertGreater(layout['BATCH_NORM_EPSILON'], layout['QUERY_NORM_EPSILON'])


@unittest.skipUnless(HAS_TORCH, 'optional offline checkpoint qualification requires PyTorch and NumPy')
class AdapterTests(unittest.TestCase):
    def setUp(self):
        import torch
        import numpy
        self.torch = torch
        self.numpy = numpy
        self.layout = adapter.read_layout(LAYOUT)
        torch.manual_seed(42)
        dimension = self.layout['DIMENSION']
        attention = self.layout['ATTENTION_CHANNELS']
        hidden = self.layout['FEED_FORWARD_CHANNELS']
        self.state = {}

        def affine(name, inputs, outputs, bias=True):
            self.state[name + '.weight'] = torch.randn(outputs, inputs) / inputs ** 0.5
            if bias:
                self.state[name + '.bias'] = torch.randn(outputs) / outputs ** 0.5

        for convolution, normalization, inputs in ((0, 1, self.layout['QUERY_CHANNELS']), (3, 4, dimension)):
            self.state[f'pre_fc_query.{convolution}.weight'] = torch.randn(dimension, inputs, 1) / inputs ** 0.5
            prefix = f'pre_fc_query.{normalization}'
            for name in ('weight', 'bias', 'running_mean'):
                self.state[f'{prefix}.{name}'] = torch.randn(dimension)
            self.state[f'{prefix}.running_var'] = torch.rand(dimension) + 0.5
            self.state[f'{prefix}.num_batches_tracked'] = torch.tensor(1)
        for block in range(self.layout['DEPTH']):
            prefix = f'dec_blocks.{block}'
            for name in ('attn_norm', 'ff_norm'):
                self.state[f'{prefix}.{name}.weight'] = torch.ones(dimension)
                self.state[f'{prefix}.{name}.bias'] = torch.randn(dimension) / dimension ** 0.5
            for name in ('q_fc', 'k_fc', 'v_fc'):
                affine(f'{prefix}.attn.{name}', dimension, dimension, False)
            affine(f'{prefix}.attn.pos_fc.0', 3, attention)
            affine(f'{prefix}.attn.pos_fc.2', attention, dimension)
            affine(f'{prefix}.attn.attn_fc.0', dimension, attention)
            affine(f'{prefix}.attn.attn_fc.2', attention, dimension)
            affine(f'{prefix}.attn.out_fc', dimension, dimension)
            affine(f'{prefix}.ff.fc1', dimension, hidden)
            affine(f'{prefix}.ff.fc2', hidden, dimension)
        affine('out_fc.0', dimension, 3)

    def pack(self):
        return adapter.pack_decoder(self.state, self.layout, self.numpy)

    def test_packed_query_mlp_matches_unfolded_pytorch_inference(self):
        torch = self.torch
        functional = torch.nn.functional
        packed = torch.from_numpy(self.pack())
        dimension = self.layout['DIMENSION']
        values = torch.randn(17, self.layout['QUERY_CHANNELS'])
        expected = values
        actual = values
        cursor = 0
        for convolution, normalization in ((0, 1), (3, 4)):
            source = self.state[f'pre_fc_query.{convolution}.weight'][:, :, 0]
            prefix = f'pre_fc_query.{normalization}'
            expected = functional.linear(expected, source)
            expected = functional.batch_norm(expected, self.state[f'{prefix}.running_mean'],
                                             self.state[f'{prefix}.running_var'], self.state[f'{prefix}.weight'],
                                             self.state[f'{prefix}.bias'], training=False,
                                             eps=self.layout['BATCH_NORM_EPSILON']).relu()
            size = source.numel()
            matrix = packed[cursor:cursor + size].reshape_as(source)
            cursor += size
            bias = packed[cursor:cursor + dimension]
            cursor += dimension
            actual = functional.linear(actual, matrix, bias).relu()
        torch.testing.assert_close(actual, expected, rtol=0.00001, atol=0.00001)

    def test_rejects_incompatible_shapes_missing_extra_and_nonfinite_tensors(self):
        original = self.state['out_fc.0.weight']
        self.state['out_fc.0.weight'] = original[:, :-1]
        with self.assertRaisesRegex(ValueError, 'expected floating-point shape'):
            self.pack()
        del self.state['out_fc.0.weight']
        with self.assertRaisesRegex(ValueError, 'missing tensor'):
            self.pack()
        self.state['out_fc.0.weight'] = original
        self.state['dec_blocks.6.extra'] = self.torch.ones(1)
        with self.assertRaisesRegex(ValueError, 'incompatible decoder'):
            self.pack()
        del self.state['dec_blocks.6.extra']
        self.state['out_fc.0.weight'][0, 0] = float('nan')
        with self.assertRaisesRegex(ValueError, 'nonfinite'):
            self.pack()

    def test_negative_batch_variance_is_rejected(self):
        self.state['pre_fc_query.1.running_var'][0] = -1
        with self.assertRaisesRegex(ValueError, 'negative running variance'):
            self.pack()

    def test_state_dict_and_uniform_module_prefix_are_supported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'weights.pt'
            self.torch.save({'model': {'module.' + name: value for name, value in self.state.items()}}, path)
            state, digest = adapter.load_checkpoint(path, self.torch, self.numpy)
            self.assertEqual(set(state), set(self.state))
            self.assertEqual(len(digest), 64)
            state['module.extra'] = self.torch.ones(1)
            self.torch.save(state, path)
            with self.assertRaisesRegex(ValueError, 'mixed module prefixes'):
                adapter.load_checkpoint(path, self.torch, self.numpy)

    def test_unknown_pickle_globals_are_never_executed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'untrusted.pt'
            self.torch.save({'model': self.state, 'payload': Path(directory)}, path)
            with self.assertRaises(pickle.UnpicklingError):
                adapter.load_checkpoint(path, self.torch, self.numpy)

    def test_lfs_pointer_reports_the_download_repair(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'pointer.ckpt'
            path.write_bytes(b'version https://git-lfs.github.com/spec/v1\n')
            with self.assertRaisesRegex(ValueError, 'git lfs pull'):
                adapter.load_checkpoint(path, self.torch, self.numpy)

    def test_binary_header_and_atomic_publication(self):
        values = self.pack()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'decoder.lltd'
            path.write_bytes(b'old asset')
            adapter.write_decoder(path, values, self.layout)
            data = path.read_bytes()
            header_size = self.layout['FILE_HEADER_WORDS'] * 4
            header = struct.unpack('<' + 'I' * self.layout['FILE_HEADER_WORDS'], data[:header_size])
            self.assertEqual(header[:3], (self.layout['FILE_MAGIC'], self.layout['FILE_VERSION'], len(values)))
            self.assertEqual(set(header[3:]), {0})
            self.assertEqual(data[header_size:], values.tobytes())
            self.assertEqual(list(Path(directory).iterdir()), [path])



class EncoderLayoutTests(unittest.TestCase):
    def test_direct_entrypoints_follow_all_test_classes(self):
        import ast
        for name in ('test_light_transformer.py', 'test_render_graph_templates.py'):
            tree = ast.parse(Path(__file__).with_name(name).read_text())
            entrypoint = next(node for node in tree.body if isinstance(node, ast.If) and
                              any(isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute) and
                                  call.func.attr == 'main' and isinstance(call.func.value, ast.Name) and
                                  call.func.value.id == 'unittest' for call in ast.walk(node)))
            self.assertFalse([node.name for node in tree.body if isinstance(node, ast.ClassDef) and
                              node.lineno > entrypoint.lineno], name)

    def test_encoder_schema_matches_shared_asset_counts(self):
        from ludus_tools import light_transformer_encoder as encoder
        schema = list(encoder.encoder_schema())
        count = sum(outputs * 2 if kind == encoder.NORMALIZE else inputs * outputs * kernel ** 3 +
                    outputs * (3 if kind == encoder.POOL else 1)
                    for kind, _, inputs, outputs, kernel, _, _ in schema)
        self.assertEqual(len(schema), encoder.ENCODER_LAYOUT['LAYER_COUNT'])
        self.assertEqual(count, encoder.ENCODER_LAYOUT['WEIGHT_COUNT'])
        self.assertEqual(sum('.attn.qkv' in name for _, name, *_ in schema), 22)
        # A negative pool BN scale must stay after the maximum reduction.
        self.assertEqual(sum(kind == encoder.POOL for kind, *_ in schema), 4)

    def test_training_scale_and_reciprocal_must_fit_gpu_binary32(self):
        for scale in (None, 0, -1, float('nan'), float('inf'), 1e308, 1e-308, 1e-40):
            self.assertFalse(adapter.is_representable_training_scale(scale))
        for scale in (0.5, 1, 100, 1e20):
            self.assertTrue(adapter.is_representable_training_scale(scale))


if __name__ == '__main__':
    unittest.main()
