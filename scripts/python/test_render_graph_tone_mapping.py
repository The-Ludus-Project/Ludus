"""Exercise authored tone mapping through graph validation and numeric lowering."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]


class ToneMappingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'main.lurender.json'
        self.document = json.loads((ROOT / 'tests/sdk_consumer/rg2/raytracing/main.lurender.json').read_text())
        self.configuration = self.document['configurations'][0]

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def insert_before_output(self, node):
        self.configuration['nodes'].append(node)
        sink = next(edge for edge in self.configuration['edges'] if edge['to'] == 'output.texture')
        source = sink['from']
        sink['from'] = node['id'] + '.color'
        self.configuration['edges'].append({'from': source, 'to': node['id'] + '.color'})

    def test_connected_node_owns_conversion_and_exposure_in_plan_and_package(self):
        self.assertEqual(0, execution.lower_plan({'plans': [self.compile()]}, 'Default')[2][0]['ToneMappingExposure'])
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8})
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 2.25})
        plan = self.compile()
        ray, average, display = (next(operation for operation in plan['passes'] if operation['kind'] == kind)
                                 for kind in ('RayTracing', 'Accumulate', 'ToneMap'))
        self.assertEqual({'id': 'Main/display', 'exposure': 2.25}, ray['tone_mapping'])
        self.assertEqual([average['id']], display['dependencies'])
        self.assertEqual(plan['roots'][0]['version'], display['attachments']['color']['output_version'])
        lowered = execution.lower_plan({'plans': [plan]}, 'Default')[2]
        self.assertEqual(1, len(lowered))  # Apply the mean and mapping before the one RGBA8 write.
        self.assertEqual((8, 2.25), (lowered[0]['AccumulationSamples'], lowered[0]['ToneMappingExposure']))
        self.assertIn('Reinhard + sRGB; exposure multiplier: 2.25', graph.dump_plan(plan))
        names = json.loads(execution.encode_resource_names({'plan': plan, 'generation': 'a' * 64}))
        self.assertTrue(any(operation['kind'] == 'ToneMap' for operation in names['operations']))
        contents = {'plan': plan, 'generation': 'a' * 64, 'interface_identity': 'b' * 64,
                    'resources': [], 'camera_targets': [], 'passes': lowered, 'bindings': [],
                    'blocks': [], 'fields': [], 'artifacts': {}, 'uniform_values': 0, 'storage_buffers': []}
        # encode_runtime uses the same production pass writer as real shader packages.
        runtime = execution.encode_runtime_package(contents)
        self.assertEqual(2.25, struct.unpack_from('<f', runtime, 104 + 16 * 4)[0])

    def test_unused_draft_and_disconnection_bypass_mapping(self):
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 1})
        source = next(edge['from'] for edge in self.configuration['edges'] if edge['to'] == 'display.color')
        self.configuration['edges'] = [edge for edge in self.configuration['edges'] if edge['to'] != 'display.color']
        next(edge for edge in self.configuration['edges'] if edge['to'] == 'output.texture')['from'] = source
        plan = self.compile()
        self.assertFalse(any(operation['kind'] == 'ToneMap' for operation in plan['passes']))
        self.assertEqual(0, execution.lower_plan({'plans': [plan]}, 'Default')[2][0]['ToneMappingExposure'])

    def test_exposure_is_bounded_even_in_unused_drafts(self):
        for exposure in (0, -1, True, '1', .001, 65, 10**400):
            with self.subTest(exposure=exposure):
                self.configuration['nodes'] = [node for node in self.configuration['nodes'] if node['id'] != 'draft']
                self.configuration['nodes'].append({'id': 'draft', 'kind': 'ToneMap', 'exposure': exposure})
                with self.assertRaisesRegex(ToolingError, 'exposure must be a finite multiplier'):
                    self.compile()

    def test_accumulating_display_encoded_colors_is_rejected(self):
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 1})
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8})
        with self.assertRaisesRegex(ToolingError, 'requires a ray color output'):
            self.compile()

    def test_double_mapping_is_rejected(self):
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 1})
        self.insert_before_output({'id': 'again', 'kind': 'ToneMap', 'exposure': 1})
        with self.assertRaisesRegex(ToolingError, 'already tone-mapped'):
            self.compile()

    def test_raster_input_rejects_before_execution(self):
        root = graph.new_workspace(self.path.parent / 'raster', output='offscreen', renderer_static=True,
                                   profiles=('metal',))
        document = json.loads(root.read_text())
        configuration = document['configurations'][0]
        configuration['nodes'].append({'id': 'display', 'kind': 'ToneMap', 'exposure': 1})
        sink = next(edge for edge in configuration['edges'] if edge['to'] == 'output.texture')
        source = sink['from']
        sink['from'] = 'display.color'
        configuration['edges'].append({'from': source, 'to': 'display.color'})
        root.write_bytes(graph.canonical_asset(document))
        with self.assertRaisesRegex(ToolingError, 'Raster and already tone-mapped images are unsupported'):
            graph.compile_graph(root)

    def test_shipped_ray_templates_expose_display_conversion(self):
        for template in ('restir-di', 'visibility-secondary-rays', 'tiled-vbuffer-nee', 'compute-ray-tracing'):
            with self.subTest(template=template):
                root = graph.new_workspace(self.path.parent / template, output='offscreen', renderer_static=True,
                                           profiles=('metal',), template=template)
                plan = graph.compile_graph(root)
                final = next(operation for operation in reversed(plan['passes']) if operation['kind'] == 'RayTracing')
                self.assertEqual(1, final['tone_mapping']['exposure'])
                self.assertEqual(1, sum(operation['kind'] == 'ToneMap' for operation in plan['passes']))
