"""Denoising policy survives authoring, lowering and binary publication."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools import render_graph_quality as quality
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]


class DenoisingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'main.lurender.json'
        self.document = json.loads((ROOT / 'tests/sdk_consumer/rg2/raytracing/main.lurender.json').read_text())
        self.configuration = self.document['configurations'][0]

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def insert(self, node):
        self.configuration['nodes'].append(node)
        sink = next(edge for edge in self.configuration['edges'] if edge['to'] == 'output.texture')
        source = sink['from']
        sink['from'] = node['id'] + '.color'
        self.configuration['edges'].append({'from': source, 'to': node['id'] + '.color'})

    def test_all_quality_modes_reach_numeric_and_binary_plans(self):
        self.insert({'id': 'mean', 'kind': 'Accumulate', 'samples': 8})
        node = {'id': 'denoise', 'kind': 'Denoise', 'quality': quality.DEFAULT_QUALITY}
        self.insert(node)
        self.insert({'id': 'display', 'kind': 'ToneMap', 'exposure': 2.25})
        for name, code in quality.QUALITY_CODES.items():
            with self.subTest(quality=name):
                node['quality'] = name
                plan = self.compile()
                ray = next(operation for operation in plan['passes'] if operation['kind'] == 'RayTracing')
                self.assertEqual({'id': 'Main/denoise', 'quality': name}, ray['denoising'])
                passes = execution.lower_plan({'plans': [plan]}, 'Default')[2]
                self.assertEqual((8, 2.25, code), (passes[0]['AccumulationSamples'], passes[0]['ToneMappingExposure'],
                                                 passes[0]['DenoisingQuality']))
                contents = {'plan': plan, 'generation': 'a' * 64, 'interface_identity': 'b' * 64,
                            'resources': [], 'camera_targets': [], 'passes': passes, 'bindings': [],
                            'blocks': [], 'fields': [], 'artifacts': {}, 'uniform_values': 0, 'storage_buffers': []}
                data = execution.encode_runtime_package(contents)
                self.assertEqual(execution.PACKAGE_VERSION, struct.unpack_from('<I', data, 4)[0])
                self.assertEqual(code, struct.unpack_from('<I', data, 104 + 18 * 4)[0])
                names = json.loads(execution.encode_resource_names({'plan': plan, 'generation': 'a' * 64}))
                self.assertTrue(any(operation['kind'] == 'Denoise' for operation in names['operations']))

    def test_no_node_and_unused_draft_do_not_enable_denoising(self):
        self.configuration['nodes'].append({'id': 'draft', 'kind': 'Denoise', 'quality': 'High'})
        passes = execution.lower_plan({'plans': [self.compile()]}, 'Default')[2]
        self.assertEqual(0, passes[0]['DenoisingQuality'])

    def test_invalid_quality_rejects_even_in_unused_draft(self):
        node = {'id': 'draft', 'kind': 'Denoise', 'quality': None}
        self.configuration['nodes'].append(node)
        for value in (None, True, 1, 'Auto', 'fast', 'Ultra'):
            with self.subTest(value=value):
                node['quality'] = value
                with self.assertRaises(ToolingError):
                    self.compile()

    def test_rejects_display_input_and_reordered_accumulation(self):
        self.insert({'id': 'display', 'kind': 'ToneMap', 'exposure': 1})
        self.insert({'id': 'denoise', 'kind': 'Denoise', 'quality': 'Balanced'})
        with self.assertRaisesRegex(ToolingError, 'requires linear ray color'):
            self.compile()

    def test_double_denoising_and_accumulating_denoised_input_reject(self):
        self.insert({'id': 'denoise', 'kind': 'Denoise', 'quality': 'Fast'})
        self.insert({'id': 'again', 'kind': 'Denoise', 'quality': 'High'})
        with self.assertRaisesRegex(ToolingError, 'already denoised'):
            self.compile()
        self.configuration['nodes'][-1] = {'id': 'again', 'kind': 'Accumulate', 'samples': 8}
        with self.assertRaisesRegex(ToolingError, 'requires a ray color output'):
            self.compile()

    def test_raster_input_is_not_misrepresented_as_hdr(self):
        self.path = graph.new_workspace(self.path.parent / 'raster', output='offscreen', renderer_static=True,
                                        profiles=('metal',))
        self.document = json.loads(self.path.read_text())
        self.configuration = self.document['configurations'][0]
        self.insert({'id': 'denoise', 'kind': 'Denoise', 'quality': 'Balanced'})
        with self.assertRaisesRegex(ToolingError, 'requires linear ray color'):
            self.compile()
