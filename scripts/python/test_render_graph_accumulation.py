"""Exercise explicit color averaging through production graph validation/lowering."""
import json
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]


class AccumulationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'main.lurender.json'
        self.document = json.loads((ROOT / 'tests/sdk_consumer/rg2/raytracing/main.lurender.json').read_text())
        self.configuration = self.document['configurations'][0]

    def compile(self):
        self.path.write_bytes(graph.canonical_asset(self.document))
        return graph.compile_graph(self.path)

    def add_accumulation(self, samples=8):
        self.configuration['nodes'].append({'id': 'average', 'kind': 'Accumulate', 'samples': samples})
        ray = next(node for node in self.configuration['nodes'] if node['kind'] == 'RayTracingPipeline')
        output = next(port for port in ray['outputs'] if port['type'] == 'ColorTexture')
        source = ray['id'] + '.' + output['id']
        for edge in self.configuration['edges']:
            if edge['from'] == source:
                edge['from'] = 'average.color'
        self.configuration['edges'].append({'from': source, 'to': 'average.color'})

    def test_only_connected_accumulate_enables_mean_in_numeric_plan(self):
        raw = self.compile()
        self.assertEqual(0, execution.lower_plan({'plans': [raw]}, 'Default')[2][0]['AccumulationSamples'])
        self.add_accumulation(8)
        plan = self.compile()
        operations = plan['passes']
        producer = next(operation for operation in operations if operation['kind'] == 'RayTracing')
        average = next(operation for operation in operations if operation['kind'] == 'Accumulate')
        self.assertEqual({'id': 'Main/average', 'samples': 8}, producer['accumulation'])
        self.assertEqual([producer['id']], average['dependencies'])
        self.assertEqual(average['attachments']['color']['output_version'], plan['roots'][0]['version'])
        lowered = execution.lower_plan({'plans': [plan]}, 'Default')[2]
        self.assertEqual(1, len(lowered))  # Fused before display encoding, never average RGBA8 output.
        self.assertEqual(8, lowered[0]['AccumulationSamples'])
        self.assertIn('compensated linear RGB mean; samples: 8', graph.dump_plan(plan))
        encoded = execution.encode_resource_names({'plan': plan, 'generation': 'a' * 64})
        self.assertTrue(any(operation['kind'] == 'Accumulate' for operation in json.loads(encoded)['operations']))

    def test_budget_is_bounded_integer_even_for_unused_drafts(self):
        for value in (0, -1, True, 1.5, '8', graph.MAX_ACCUMULATION_SAMPLES + 1):
            with self.subTest(value=value):
                self.configuration['nodes'] = [node for node in self.configuration['nodes'] if node['kind'] != 'Accumulate']
                self.configuration['nodes'].append({'id': 'draft', 'kind': 'Accumulate', 'samples': value})
                with self.assertRaisesRegex(ToolingError, 'integer must be'):
                    self.compile()

    def test_unused_node_does_not_change_raw_output(self):
        self.configuration['nodes'].append({'id': 'draft', 'kind': 'Accumulate', 'samples': 8})
        plan = self.compile()
        self.assertFalse(any(operation['kind'] == 'Accumulate' for operation in plan['passes']))
        self.assertEqual(0, execution.lower_plan({'plans': [plan]}, 'Default')[2][0]['AccumulationSamples'])

    def test_raw_export_cannot_alias_averaged_export(self):
        self.add_accumulation()
        self.configuration['nodes'].append({'id': 'raw', 'kind': 'Export', 'type': 'ColorTexture'})
        source = next(edge['from'] for edge in self.configuration['edges'] if edge['to'] == 'average.color')
        self.configuration['edges'].append({'from': source, 'to': 'raw.texture'})
        with self.assertRaisesRegex(ToolingError, 'final physical resource version'):
            self.compile()

    def test_already_averaged_color_is_rejected(self):
        self.add_accumulation()
        self.configuration['nodes'].append({'id': 'again', 'kind': 'Accumulate', 'samples': 2})
        sink = next(edge for edge in self.configuration['edges'] if edge['from'] == 'average.color')
        sink['from'] = 'again.color'
        self.configuration['edges'].append({'from': 'average.color', 'to': 'again.color'})
        with self.assertRaisesRegex(ToolingError, 'requires a ray color output'):
            self.compile()

    def test_per_pass_accumulation_is_independent_of_reservoir_storage(self):
        # ReSTIR's reservoir reuse is deliberately separate from the optional RGB mean.
        from ludus_tools import render_graph as authoring
        root = authoring.new_workspace(self.path.parent / 'restir', output='offscreen', renderer_static=True,
                                       profiles=('metal',), template='restir-di')
        asset = json.loads(root.read_text())
        config = asset['configurations'][0]
        config['nodes'].append({'id': 'average', 'kind': 'Accumulate', 'samples': 16})
        sink = next(edge for edge in config['edges'] if edge['to'] == 'tonemap.color')
        source = sink['from']
        sink['from'] = 'average.color'
        config['edges'].append({'from': source, 'to': 'average.color'})
        root.write_bytes(graph.canonical_asset(asset))
        plan = graph.compile_graph(root)
        operations = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
        self.assertEqual([0, 0, 16], [operation.get('accumulation', {}).get('samples', 0) for operation in operations])
        self.assertTrue(all(operation['persistent_buffers'] for operation in operations))
        self.assertEqual(2, len({resource['type'] for resource in plan['resources'].values()}))

    def test_sample_limit_matches_native_policy_owner(self):
        import re
        header = (ROOT / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/plan.h').read_text()
        self.assertEqual(graph.MAX_ACCUMULATION_SAMPLES,
                         int(re.search(r'MAX_ACCUMULATION_SAMPLES = (\d+)', header)[1]))
