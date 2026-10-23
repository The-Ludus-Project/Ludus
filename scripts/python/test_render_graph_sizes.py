"""Shared fixed sizes, texture-derived extents and instance-local contracts."""
import copy
import unittest

from ludus_tools import render_graph as graph
from ludus_tools.errors import ToolingError
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

    def provide_output_size(self, output_extent, scale):
        value = self.share_size([1, 1])
        self.node(value, 'size').update(extent='OutputSize', scale=scale)
        self.config(value)['nodes'].append({'id': 'output_size', 'kind': 'OutputSize', 'extent': output_extent})
        self.config(value)['edges'].append({'from': 'output_size.size', 'to': 'size.output'})
        return value

    def test_native_resolution_uses_equal_output_and_render_sizes(self):
        self.write(self.provide_output_size([1921, 1081], [1, 1]))
        plan = graph.compile_graph(self.path)
        for identity in ('color', 'depth'):
            resource = plan['resources']['Main/' + identity]
            self.assertEqual([1921, 1081], resource['extent'])
            self.assertNotIn('view_scale', resource)
        self.assertNotIn('Main/output_size', plan['resources'])
        self.assertNotIn('Main/size', plan['resources'])

    def test_fixed_output_scales_with_integer_ceil_and_preserves_sources(self):
        self.write(self.provide_output_size([1921, 1081], [1, 2]))
        before = self.path.read_bytes()
        plan = graph.compile_graph(self.path)
        for identity in ('color', 'depth'):
            self.assertEqual([961, 541], plan['resources']['Main/' + identity]['extent'])
        self.assertEqual(before, self.path.read_bytes())

    def test_viewport_output_retains_a_dynamic_render_fraction(self):
        value = self.provide_output_size('Viewport', [2, 3])
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual([427, 320], plan['resources']['Main/color']['extent'])
        self.assertEqual([2, 3], plan['resources']['Main/color']['view_scale'])
        self.node(value, 'size')['scale'] = [1, 1]
        self.write(value)
        self.assertEqual([1, 1], graph.compile_graph(self.path)['resources']['Main/color']['view_scale'])

    def test_output_size_can_directly_drive_targets_without_an_internal_scale(self):
        value = self.share_size([1, 1])
        self.node(value, 'size').update(kind='OutputSize', extent='Viewport')
        self.write(value)
        resource = graph.compile_graph(self.path)['resources']['Main/color']
        self.assertEqual([1, 1], resource['view_scale'])
        self.assertEqual([640, 480], resource['extent'])

    def test_bad_output_policies_and_scales_reject_without_mutating_saved_inputs(self):
        for extent, scale in (('Texture', [1, 2]), ({'reference': 'View', 'scale': [1, 1]}, [1, 1]),
                              ([0, 1080], [1, 1]), ('Viewport', [1, 0]), ('Viewport', [True, 2])):
            value = self.provide_output_size(extent, scale)
            self.write(value)
            before = self.path.read_bytes()
            with self.assertRaises(ToolingError):
                graph.compile_graph(self.path)
            self.assertEqual(before, self.path.read_bytes())
        value = self.provide_output_size('Viewport', [1, 2])
        value['version'] = 1
        self.write(value)
        self.fails('Version')

    def test_unbound_output_reference_and_out_of_range_fixed_render_extent_reject(self):
        value = self.provide_output_size([graph.MAX_TARGET_EXTENT, 1], [2, 1])
        self.write(value)
        self.fails('Capacity')
        self.config(value)['edges'].pop()
        self.write(value)
        self.fails('UnboundInput')

    def test_scaled_view_reference_cannot_be_scaled_again(self):
        value = self.provide_output_size('Viewport', [1, 2])
        self.node(value, 'output_size').update(kind='RenderSize', extent={'reference': 'View', 'scale': [1, 2]})
        self.write(value)
        self.assertIn('unscaled OutputSize', self.fails('SizePolicy'))

    def test_output_and_render_provider_chain_crosses_subgraph_interfaces(self):
        value = self.provide_output_size([1919, 1079], [2, 3])
        child = {'version': 2, 'id': 'RenderSizing', 'default': 'Default', 'configurations': [{
            'id': 'Default', 'inputs': [{'id': 'output', 'type': 'RenderSize'}],
            'outputs': [{'id': 'render', 'type': 'RenderSize', 'source': 'size.size', 'required': False}],
            'nodes': [{'id': 'size', 'kind': 'RenderSize', 'extent': 'OutputSize', 'scale': [2, 3]}],
            'edges': [{'from': 'input.output', 'to': 'size.output'}]}]}
        self.write(child, 'sizes.lurender.json')
        node = self.node(value, 'size')
        node.clear()
        node.update(id='size', kind='Subgraph', asset='sizes.lurender.json', configuration='Default')
        for edge in self.config(value)['edges']:
            if edge['from'] == 'size.size':
                edge['from'] = 'size.render'
        self.write(value)
        resource = graph.compile_graph(self.path)['resources']['Main/color']
        self.assertEqual([1280, 720], resource['extent'])
        self.assertEqual(child, self.read('sizes.lurender.json'))

    def test_view_size_policy_survives_targets_and_matching_metadata(self):
        value = self.share_size({'reference': 'View', 'scale': [2, 4]})
        self.write(value)
        plan = graph.compile_graph(self.path)
        for identity in ('color', 'depth'):
            self.assertEqual([320, 240], plan['resources']['Main/' + identity]['extent'])
            self.assertEqual([1, 2], plan['resources']['Main/' + identity]['view_scale'])
        # The same nominal fixed dimensions do not establish a runtime equality contract.
        self.node(value, 'depth')['extent'] = [320, 240]
        self.config(value)['edges'] = [edge for edge in self.config(value)['edges'] if edge['to'] != 'depth.size']
        self.write(value)
        self.fails('AttachmentMismatch')

    def test_large_scale_is_validated_against_the_runtime_reference_not_the_nominal_one(self):
        self.write(self.share_size({'reference': 'View', 'scale': [graph.MAX_TARGET_EXTENT, 1]}))
        plan = graph.compile_graph(self.path)
        self.assertEqual([graph.MAX_TARGET_EXTENT, graph.MAX_TARGET_EXTENT], plan['resources']['Main/color']['extent'])
        self.assertEqual([graph.MAX_TARGET_EXTENT, 1], plan['resources']['Main/color']['view_scale'])

    def test_view_size_schema_rejects_unknown_reference_and_invalid_scale(self):
        for extent in ({'reference': 'Window', 'scale': [1, 2]},
                       {'reference': 'View', 'scale': [1, 0]},
                       {'reference': 'View', 'scale': [True, 2]},
                       {'reference': 'View', 'scale': [1, 2], 'extra': True}):
            self.write(self.share_size(extent))
            with self.assertRaises(ToolingError):
                graph.compile_graph(self.path)

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
