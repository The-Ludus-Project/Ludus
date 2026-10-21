"""Shared fixed sizes, texture-derived extents and instance-local contracts."""
import copy
import unittest

from ludus_tools import render_graph as graph
import test_render_graph as fixtures


class RenderSizeTests(unittest.TestCase):
    setUp = fixtures.RenderGraphTests.setUp
    read = fixtures.RenderGraphTests.read
    write = fixtures.RenderGraphTests.write
    config = fixtures.RenderGraphTests.config
    node = fixtures.RenderGraphTests.node
    fails = fixtures.RenderGraphTests.fails

    def share_size(self, extent):
        value = self.read()
        value['version'] = 2
        self.config(value)['nodes'].append({'id': 'size', 'kind': 'RenderSize', 'extent': extent})
        for identity in ('color', 'depth'):
            self.node(value, identity)['extent'] = 'RenderSize'
            self.config(value)['edges'].append({'from': 'size.size', 'to': identity + '.size'})
        return value

    def test_one_size_drives_color_and_depth_without_creating_a_gpu_resource(self):
        value = self.share_size([97, 65])
        self.write(value)
        before = self.path.read_bytes()
        plan = graph.compile_graph(self.path)
        self.assertEqual([97, 65], plan['resources']['Main/color']['extent'])
        self.assertEqual([97, 65], plan['resources']['Main/depth']['extent'])
        self.assertNotIn('Main/size', plan['resources'])
        self.assertEqual(['Raster', 'Present'], [operation['kind'] for operation in plan['passes']])
        self.assertEqual(before, self.path.read_bytes())

    def test_match_accepts_color_or_depth_texture_without_requiring_initialized_pixels(self):
        for source, destination in (('color', 'depth'), ('depth', 'color')):
            value = self.read()
            value['version'] = 2
            self.config(value)['nodes'].append({'id': 'size', 'kind': 'RenderSize', 'extent': 'Texture'})
            self.node(value, source)['extent'] = [101, 67]
            self.node(value, destination)['extent'] = 'RenderSize'
            self.config(value)['edges'].extend([
                {'from': source + '.texture', 'to': 'size.texture'},
                {'from': 'size.size', 'to': destination + '.size'}])
            self.write(value)
            plan = graph.compile_graph(self.path)
            self.assertEqual([101, 67], plan['resources']['Main/' + destination]['extent'])
            self.assertEqual(1, plan['roots'][0]['version'])
            # Restore the original fixed-size workspace for the next case.
            self.node(value, source)['extent'] = [640, 480]
            self.node(value, destination)['extent'] = [640, 480]
            self.config(value)['nodes'] = [node for node in self.config(value)['nodes'] if node['id'] != 'size']
            self.config(value)['edges'] = [edge for edge in self.config(value)['edges']
                                           if edge['to'] not in ('size.texture', destination + '.size')]
            self.write(value)

    def test_sizes_cross_subgraph_interfaces_and_resolve_per_instance(self):
        value = self.share_size([97, 65])
        size = self.config(value)['nodes'].pop()
        child = {'version': 2, 'id': 'SizeLibrary', 'default': 'Default', 'configurations': [{
            'id': 'Default', 'inputs': [{'id': 'reference', 'type': 'ColorTexture'}],
            'outputs': [{'id': 'size', 'type': 'RenderSize', 'source': 'size.size', 'required': False}],
            'nodes': [{**size, 'extent': 'Texture'}],
            'edges': [{'from': 'input.reference', 'to': 'size.texture'}]}]}
        self.write(child, 'sizes.lurender.json')
        self.config(value)['nodes'].extend([
            {'id': 'size', 'kind': 'Subgraph', 'asset': 'sizes.lurender.json', 'configuration': 'Default'},
            {**graph._target('reference', 'ColorTexture'), 'extent': [97, 65]}])
        self.config(value)['edges'].append({'from': 'reference.texture', 'to': 'size.reference'})
        large = copy.deepcopy(self.config(value))
        large['id'] = 'Large'
        next(node for node in large['nodes'] if node['id'] == 'reference')['extent'] = [193, 129]
        value['configurations'].append(large)
        self.write(value)
        for configuration, extent in (('Default', [97, 65]), ('Large', [193, 129])):
            plan = graph.compile_graph(self.path, configuration=configuration)
            for identity in ('color', 'depth'):
                self.assertEqual(extent, plan['resources']['Main/' + identity]['extent'])
            self.assertNotIn('Main/reference', plan['resources'])
        self.assertEqual(child, self.read('sizes.lurender.json'))

    def test_unbound_type_cycle_and_dimension_limits_are_explicit(self):
        value = self.share_size('Texture')
        self.write(value)
        self.fails('UnboundInput')
        self.config(value)['edges'].append({'from': 'scene.scene', 'to': 'size.texture'})
        self.write(value)
        self.fails('TypeMismatch')
        self.config(value)['edges'][-1]['from'] = 'color.texture'
        self.write(value)
        self.fails('Cycle')
        for extent in ([0, 1], [graph.MAX_TARGET_EXTENT + 1, 1], [True, 1], [1], 'Unknown'):
            self.node(value, 'size')['extent'] = extent
            self.write(value)
            message = self.fails('Schema' if type(extent) is not list or len(extent) != 2 else 'Capacity')
            self.assertIn('#Default/size:', message)

    def test_size_output_is_a_subgraph_value_not_an_exportable_texture(self):
        value = {'version': 2, 'id': 'Sizes', 'default': 'Default', 'configurations': [{
            'id': 'Default', 'inputs': [],
            'outputs': [{'id': 'size', 'type': 'RenderSize', 'source': 'size.size', 'required': False}],
            'nodes': [{'id': 'size', 'kind': 'RenderSize', 'extent': [97, 65]}], 'edges': []}]}
        self.write(value)
        self.assertEqual('RenderSize', graph.load_library(self.path)['configurations'][0]['outputs'][0]['type'])
        self.assertIn('graph value', self.fails('TypeMismatch'))

    def test_mismatch_reports_targets_dimensions_and_shared_size_repair(self):
        value = self.read()
        self.node(value, 'depth')['extent'] = [97, 65]
        self.write(value)
        message = self.fails('AttachmentMismatch')
        for text in ('Main/color', 'Main/depth', '[640, 480]', '[97, 65]', 'same RenderSize'):
            self.assertIn(text, message)


if __name__ == '__main__':
    unittest.main()
