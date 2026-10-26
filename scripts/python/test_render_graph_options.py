"""Per-instance finite choices use the production graph validator and lowering."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from ludus_tools import render_graph as graph, render_graph_pipeline as pipeline
from ludus_tools.errors import ToolingError

FIXTURE = Path(__file__).resolve().parents[2] / 'tests/sdk_consumer/rg2/offscreen'


def option(identity='Cull', target='state.cull', values=None, default='None', kind='Enum'):
    return {'id': identity, 'label': identity, 'type': kind, 'target': target,
            'values': ['None', 'Back', 'Front'] if values is None else values, 'default': default}


class OptionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.main = json.loads((FIXTURE / 'main.lurender.json').read_text())
        self.library = json.loads((FIXTURE / 'rasterize.lurender.json').read_text())
        manifest = json.loads((FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        self.library = pipeline.migrate_document(self.library, manifest)
        self.configuration = self.library['configurations'][0]
        self.draw = self.configuration['nodes'][0]
        self.draw['options'] = [option()]
        self.configuration['options'] = [option(target='draw.Cull')]
        self.path = self.root / 'main.lurender.json'

    def write(self):
        self.path.write_bytes(graph.canonical_asset(self.main))
        (self.root / 'rasterize.lurender.json').write_bytes(graph.canonical_asset(self.library))

    def passes(self):
        self.write()
        return [item for item in graph.compile_graph(self.path)['passes'] if 'pipeline' in item]

    def test_two_uses_are_independent_and_defaults_follow_author(self):
        configuration = self.main['configurations'][0]
        reference = next(node for node in configuration['nodes'] if node['id'] == 'raster')
        reference['overrides'] = {'Cull': 'Back'}
        # A second independent camera/resource route calls the exact same library.
        other = copy.deepcopy(configuration)
        configuration['nodes'].extend({**node, 'id': node['id'] + 'Other'} for node in other['nodes'])
        for node in configuration['nodes'][len(other['nodes']):]:
            if node['kind'] == 'Subgraph':
                node['overrides'] = {'Cull': 'Front'}
        configuration['edges'].extend({'from': edge['from'].replace('.', 'Other.', 1),
                                       'to': edge['to'].replace('.', 'Other.', 1)} for edge in other['edges'])
        before = copy.deepcopy(self.library)
        operations = self.passes()
        self.assertEqual(['Back', 'Front'], [item['pipeline']['state']['cull'] for item in operations])
        self.assertEqual(before, self.library)
        reference.pop('overrides')
        self.configuration['options'][0]['default'] = 'Front'
        self.assertEqual('Front', self.passes()[0]['pipeline']['state']['cull'])
        self.assertEqual('None', self.draw['state']['cull'])

    def test_nested_exposure_and_shader_program_selection(self):
        self.draw['options'] = [option('Program', 'shaders.fragment.program', ['ColorMain', 'Alternate'], 'Alternate')]
        self.configuration['options'] = [option('Program', 'draw.Program', ['ColorMain', 'Alternate'], 'ColorMain')]
        parent = self.main['configurations'][0]
        parent['options'] = [option('Quality', 'raster.Program', ['ColorMain', 'Alternate'], 'Alternate')]
        self.assertEqual('Alternate', self.passes()[0]['pipeline']['shaders'][1]['program'])
        self.assertEqual('ColorMain', self.configuration['options'][0]['default'])

    def test_invalid_overrides_bindings_and_types_fail_without_writes(self):
        base = copy.deepcopy(self.library)
        changes = [lambda: self.configuration['options'][0].update(target='missing.Cull'),
                   lambda: self.configuration['options'][0].update(values=['Unsupported']),
                   lambda: self.draw['options'][0].update(target='id'),
                   lambda: self.draw.update(overrides={'Unknown': 'None'}),
                   lambda: self.draw['options'][0].update(default='Hidden'),
                   lambda: self.draw['options'][0].update(type='Bool'),
                   lambda: self.draw['options'][0].update(values=['None', 'None'])]
        for change in changes:
            self.library = copy.deepcopy(base)
            self.configuration = self.library['configurations'][0]
            self.draw = self.configuration['nodes'][0]
            change()
            # Write malformed drafts as text to exercise library validation, too.
            self.path.write_text(json.dumps(self.main))
            child = self.root / 'rasterize.lurender.json'
            child.write_text(json.dumps(self.library))
            before = child.read_bytes()
            with self.assertRaises(ToolingError) as failure:
                graph.load_library(self.path)
            self.assertEqual('RenderGraphOption', failure.exception.code)
            self.assertEqual(before, child.read_bytes())

    def test_bool_integer_and_float_allowed_sets(self):
        for declaration, field, expected in (
                (option('WriteDepth', 'state.depth_write', [False, True], False, 'Bool'), 'depth_write', False),
                (option('Samples', 'state.samples', [1, 2], 1, 'UInt32'), 'samples', 1)):
            self.draw['options'] = [declaration]
            self.configuration.pop('options', None)
            self.assertEqual(expected, self.passes()[0]['pipeline']['state'][field])
        with self.assertRaises(ToolingError):
            graph.options.validate([option('Value', 'state.samples', [True, 1], 1, 'UInt32')], {}, 'test', graph)
        graph.options.validate([option('Value', 'state.samples', [0.5, 1], 1.0, 'Float32')], {}, 'test', graph)

    def test_independent_choices_have_a_bounded_combination_space(self):
        declarations = [option('Switch' + str(index), 'state.switch' + str(index), [False, True], False, 'Bool')
                        for index in range(7)]
        with self.assertRaises(ToolingError) as failure:
            graph.options.validate(declarations, {}, 'test', graph)
        self.assertEqual('RenderGraphOption', failure.exception.code)
        self.assertIn('64 combinations', str(failure.exception))

    def test_effective_values_still_obey_pipeline_schema_and_capabilities(self):
        self.draw['options'] = [option(values=['Unsupported'], default='Unsupported')]
        self.configuration['options'] = [option(target='draw.Cull', values=['Unsupported'], default='Unsupported')]
        self.write()
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual('RenderGraphSchema', failure.exception.code)


if __name__ == '__main__':
    unittest.main()
