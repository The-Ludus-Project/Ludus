"""Execution reachability, nested effects and draft preservation regressions."""
import copy
import unittest

from ludus_tools import render_graph as graph, render_graph_pipeline as pipeline
import test_render_graph as fixtures


class ExecutionSelectionTests(unittest.TestCase):
    setUp = fixtures.RenderGraphTests.setUp
    read = fixtures.RenderGraphTests.read
    write = fixtures.RenderGraphTests.write
    config = fixtures.RenderGraphTests.config
    node = fixtures.RenderGraphTests.node
    fails = fixtures.RenderGraphTests.fails

    def test_unbound_subgraph_and_unsupported_drafts_do_not_block_live_route(self):
        before = graph.compile_graph(self.path)
        value = self.read()
        draft = copy.deepcopy(self.node(value, 'raster'))
        draft['id'] = 'draft_raster'
        self.config(value)['nodes'].append(draft)
        value['version'] = 2
        for kind in pipeline.KINDS:
            node = pipeline.new_node(kind, kind)
            node['inputs'] = [{'id': 'unbound', 'type': 'DepthTexture', 'required': True}]
            self.config(value)['nodes'].append(node)
        self.write(value)
        sources = {path.name: path.read_bytes() for path in self.directory.iterdir()}
        after = graph.compile_graph(self.path)
        for key in ('passes', 'resources', 'roots', 'cameras', 'techniques'):
            self.assertEqual(before[key], after[key])
        self.assertEqual(sources, {path.name: path.read_bytes() for path in self.directory.iterdir()})

    def test_connected_unused_writer_does_not_overwrite_exported_version(self):
        value = self.read()
        draft = copy.deepcopy(self.node(value, 'raster'))
        draft['id'] = 'draft_raster'
        self.config(value)['nodes'].append(draft)
        self.config(value)['edges'].extend(
            {'from': edge['from'], 'to': edge['to'].replace('raster.', 'draft_raster.')}
            for edge in list(self.config(value)['edges']) if edge['to'].startswith('raster.'))
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(['Main/raster/draw', 'Main/output'], [entry['id'] for entry in plan['passes']])

    def test_unused_nested_output_does_not_admit_unsupported_pipeline(self):
        child = self.read('rasterize.lurender.json')
        child['version'] = 2
        draft = pipeline.new_node('ComputePipeline', 'draft')
        draft['inputs'] = [{'id': 'color', 'type': 'ColorTexture', 'required': True}]
        draft['outputs'] = [{'id': 'color', 'type': 'ColorTexture', 'target': 'color',
                             'format': 'RGBA8Unorm', 'load': 'Clear', 'store': 'Store'}]
        self.config(child)['nodes'].append(draft)
        self.config(child)['edges'].append({'from': 'input.color', 'to': 'draft.color'})
        self.config(child)['outputs'].append({'id': 'draft', 'type': 'ColorTexture',
                                              'source': 'draft.color', 'required': False})
        self.write(child, 'rasterize.lurender.json')
        self.assertEqual(2, len(graph.compile_graph(self.path)['passes']))
        value = self.read()
        next(edge for edge in self.config(value)['edges'] if edge['to'] == 'output.color')['from'] = 'raster.draft'
        self.write(value)
        self.fails('IncompletePipeline')

    def test_explicit_nested_present_is_retained_without_an_output_consumer(self):
        child = self.read('rasterize.lurender.json')
        self.config(child)['outputs'] = []
        self.config(child)['nodes'].append({'id': 'present', 'kind': 'Present'})
        self.config(child)['edges'].append({'from': 'draw.color', 'to': 'present.color'})
        self.write(child, 'rasterize.lurender.json')
        value = self.read()
        self.config(value)['nodes'] = [node for node in self.config(value)['nodes'] if node['id'] != 'output']
        self.config(value)['edges'] = [edge for edge in self.config(value)['edges'] if edge['to'] != 'output.color']
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(['Main/raster/draw', 'Main/raster/present'], [entry['id'] for entry in plan['passes']])
        self.assertEqual('Present', plan['roots'][0]['kind'])

    def test_structurally_invalid_unused_wires_still_reject(self):
        value = self.read()
        self.config(value)['nodes'].append({'id': 'draft', 'kind': 'Camera', 'active': False,
                                           'role': 'Offscreen', 'target_type': 'ColorTexture'})
        self.config(value)['edges'].append({'from': 'scene.scene', 'to': 'draft.target'})
        self.write(value)
        self.fails('TypeMismatch')

    def test_required_output_needs_a_live_consumer(self):
        child = self.read('rasterize.lurender.json')
        self.config(child)['outputs'][0]['required'] = True
        self.write(child, 'rasterize.lurender.json')
        value = self.read()
        self.config(value)['nodes'].append({'id': 'unused', 'kind': 'Camera', 'active': False,
                                           'role': 'Offscreen', 'target_type': 'DepthTexture'})
        self.config(value)['edges'].append({'from': 'raster.depth', 'to': 'unused.target'})
        self.write(value)
        self.fails('UnusedRequiredOutput')


if __name__ == '__main__':
    unittest.main()
