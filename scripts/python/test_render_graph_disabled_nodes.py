"""Exercise disabled Accumulate/ToneMap/Export nodes through production graph validation/lowering."""
import json
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_execution as execution
from ludus_tools.errors import ToolingError

ROOT = Path(__file__).resolve().parents[2]


class DisabledNodeTests(unittest.TestCase):
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

    def lowered(self, plan):
        operation = execution.lower_plan({'plans': [plan]}, 'Default')[2][0]
        return operation['AccumulationSamples'], operation['ToneMappingExposure']

    def test_disabled_chain_lowers_like_the_unconnected_graph(self):
        raw = self.compile()
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8, 'enabled': False})
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 2, 'enabled': False})
        plan = self.compile()
        self.assertEqual(raw['passes'], plan['passes'])
        self.assertEqual(raw['roots'], plan['roots'])
        self.assertEqual((0, 0), self.lowered(plan))
        self.assertNotIn('Accumulate', graph.dump_plan(plan))

    def test_disabling_one_node_keeps_the_other(self):
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8, 'enabled': False})
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 2})
        plan = self.compile()
        display = next(operation for operation in plan['passes'] if operation['kind'] == 'ToneMap')
        ray = next(operation for operation in plan['passes'] if operation['kind'] == 'RayTracing')
        self.assertEqual([ray['id']], display['dependencies'])
        self.assertEqual((0, 2), self.lowered(plan))

    def test_disabled_denoise_leaves_accumulation_and_mapping_connected(self):
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8})
        self.insert_before_output({'id': 'denoise', 'kind': 'Denoise', 'quality': 'High', 'enabled': False})
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 2})
        plan = self.compile()
        self.assertEqual(['RayTracing', 'Accumulate', 'ToneMap', 'Export'],
                         [operation['kind'] for operation in plan['passes']])
        ray, average, display = (next(operation for operation in plan['passes'] if operation['kind'] == kind)
                                 for kind in ('RayTracing', 'Accumulate', 'ToneMap'))
        self.assertNotIn('denoising', ray)
        self.assertEqual([average['id']], display['dependencies'])
        self.assertEqual((8, 2), self.lowered(plan))

    def test_enabled_true_and_absent_field_are_equivalent(self):
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8})
        absent = self.compile()
        self.configuration['nodes'][-1]['enabled'] = True
        self.assertEqual(absent['passes'], self.compile()['passes'])
        self.assertEqual((8, 0), self.lowered(absent))

    def test_source_keeps_the_disabled_node_and_its_settings(self):
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 8, 'enabled': False})
        saved = json.loads(graph.canonical_asset(self.document))
        self.assertIn({'id': 'average', 'kind': 'Accumulate', 'samples': 8, 'enabled': False},
                      saved['configurations'][0]['nodes'])
        self.assertIn({'from': 'average.color', 'to': 'output.texture'}, saved['configurations'][0]['edges'])

    def test_consumed_output_needs_a_connected_input(self):
        self.insert_before_output({'id': 'display', 'kind': 'ToneMap', 'exposure': 1, 'enabled': False})
        self.configuration['edges'] = [edge for edge in self.configuration['edges'] if edge['to'] != 'display.color']
        with self.assertRaisesRegex(ToolingError, "Disabled node 'display' would pass its color input") as raised:
            self.compile()
        self.assertEqual('RenderGraphUnboundInput', raised.exception.code)

    def test_unused_disabled_draft_is_accepted(self):
        self.configuration['nodes'].append({'id': 'draft', 'kind': 'ToneMap', 'exposure': 1, 'enabled': False})
        self.assertFalse(any(operation['kind'] == 'ToneMap' for operation in self.compile()['passes']))

    def test_disabled_settings_are_still_validated(self):
        self.insert_before_output({'id': 'average', 'kind': 'Accumulate', 'samples': 0, 'enabled': False})
        with self.assertRaisesRegex(ToolingError, 'integer must be'):
            self.compile()

    def test_only_pass_through_and_export_kinds_accept_the_field(self):
        baseline = list(self.configuration['nodes'])
        for node, message in (({'id': 'average', 'kind': 'Accumulate', 'samples': 8, 'enabled': 0}, 'Boolean required'),
                              ({'id': 'raw', 'kind': 'Export', 'type': 'ColorTexture', 'enabled': 1}, 'Boolean required'),
                              ({'id': 'screen', 'kind': 'Present', 'enabled': False},
                               'only Accumulate, Denoise, ToneMap, Export nodes can be disabled')):
            with self.subTest(kind=node['kind']):
                self.configuration['nodes'] = [*baseline, node]
                with self.assertRaisesRegex(ToolingError, message):
                    self.compile()

    def add_alternative_route(self):
        """Add trace -> ToneMap -> Export beside the raw trace -> Export route; both write one target."""
        self.configuration['nodes'] += [{'id': 'display', 'kind': 'ToneMap', 'exposure': 2},
                                        {'id': 'mapped', 'kind': 'Export', 'type': 'ColorTexture'}]
        self.configuration['edges'] += [{'from': 'trace.color', 'to': 'display.color'},
                                        {'from': 'display.color', 'to': 'mapped.texture'}]

    def disable(self, *identities):
        for node in self.configuration['nodes']:
            node.pop('enabled', None)
            if node['id'] in identities:
                node['enabled'] = False

    def test_disabled_export_selects_the_other_route(self):
        raw = self.compile()
        self.add_alternative_route()
        with self.assertRaises(ToolingError) as raised:
            self.compile()
        self.assertEqual('RenderGraphStaleVersion', raised.exception.code)
        self.disable('output')
        mapped = self.compile()
        self.assertEqual(['RayTracing', 'ToneMap', 'Export'], [operation['kind'] for operation in mapped['passes']])
        self.assertEqual(['Main/mapped'], [root['id'] for root in mapped['roots']])
        self.disable('mapped')
        plan = self.compile()
        self.assertEqual(raw['passes'], plan['passes'])
        self.assertEqual(raw['roots'], plan['roots'])
        self.assertNotIn('ToneMap', graph.dump_plan(plan))

    def test_disabled_export_needs_no_input(self):
        raw = self.compile()
        self.configuration['nodes'].append({'id': 'depth', 'kind': 'Export', 'type': 'DepthTexture'})
        with self.assertRaises(ToolingError) as raised:
            self.compile()
        self.assertEqual('RenderGraphUnboundInput', raised.exception.code)
        self.configuration['nodes'][-1]['enabled'] = False
        plan = self.compile()
        self.assertEqual(raw['passes'], plan['passes'])
        self.assertEqual(raw['roots'], plan['roots'])
        saved = json.loads(graph.canonical_asset(self.document))
        self.assertIn({'id': 'depth', 'kind': 'Export', 'type': 'DepthTexture', 'enabled': False},
                      saved['configurations'][0]['nodes'])

    def test_disabled_export_still_checks_its_wire_type(self):
        self.configuration['nodes'].append({'id': 'depth', 'kind': 'Export', 'type': 'DepthTexture', 'enabled': False})
        self.configuration['edges'].append({'from': 'trace.color', 'to': 'depth.texture'})
        with self.assertRaises(ToolingError) as raised:
            self.compile()
        self.assertEqual('RenderGraphTypeMismatch', raised.exception.code)

    def test_active_camera_still_needs_an_enabled_route(self):
        self.disable('output')
        with self.assertRaisesRegex(ToolingError, 'active camera has no rendered route') as raised:
            self.compile()
        self.assertEqual('RenderGraphCameraRouteMissing', raised.exception.code)
        for node in self.configuration['nodes']:
            if node['kind'] == 'Camera':
                node['active'] = False
        with self.assertRaisesRegex(ToolingError, 'needs an enabled Export or Present root') as raised:
            self.compile()
        self.assertEqual('RenderGraphUnboundOutput', raised.exception.code)

    def test_disabled_library_export_leaves_no_effect(self):
        library = self.path.parent / 'debug.lurender.json'
        export = {'id': 'debug', 'kind': 'Export', 'type': 'ColorTexture', 'enabled': False}
        self.configuration['nodes'].append({'id': 'inspect', 'kind': 'Subgraph', 'asset': 'debug.lurender.json',
                                            'configuration': 'Main'})
        self.configuration['edges'].append({'from': 'trace.color', 'to': 'inspect.color'})

        def roots():
            library.write_bytes(graph.canonical_asset({
                'version': 1, 'id': 'debug', 'default': 'Main', 'configurations': [{
                    'id': 'Main', 'inputs': [{'id': 'color', 'type': 'ColorTexture'}], 'outputs': [],
                    'nodes': [export], 'edges': [{'from': 'input.color', 'to': 'debug.texture'}]}]}))
            return [root['id'] for root in self.compile()['roots']]

        self.assertEqual(['Main/output'], roots())
        del export['enabled']
        self.assertIn('Main/inspect/debug', roots())

    def test_library_output_forwards_through_a_disabled_node(self):
        library = self.path.parent / 'library.lurender.json'
        library.write_bytes(graph.canonical_asset({
            'version': 1, 'id': 'library', 'default': 'Main', 'configurations': [{
                'id': 'Main', 'inputs': [{'id': 'color', 'type': 'ColorTexture'}],
                'outputs': [{'id': 'color', 'type': 'ColorTexture', 'source': 'display.color', 'required': False}],
                'nodes': [{'id': 'display', 'kind': 'ToneMap', 'exposure': 1, 'enabled': False}],
                'edges': [{'from': 'input.color', 'to': 'display.color'}]}]}))
        interface = graph.load_library(library)
        self.assertEqual('color', interface['configurations'][0]['outputs'][0]['id'])
        self.insert_before_output({'id': 'post', 'kind': 'Subgraph', 'asset': 'library.lurender.json',
                                   'configuration': 'Main'})
        self.configuration['edges'] = [{**edge, 'from': 'post.color'} if edge['from'] == 'post.color' else
                                       {**edge, 'to': 'post.color'} if edge['to'] == 'post.color' else edge
                                       for edge in self.configuration['edges']]
        plan = self.compile()
        self.assertFalse(any(operation['kind'] == 'ToneMap' for operation in plan['passes']))
        self.assertEqual((0, 0), self.lowered(plan))


if __name__ == '__main__':
    unittest.main()
