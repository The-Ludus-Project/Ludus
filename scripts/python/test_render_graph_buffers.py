"""Structured buffer graph contracts, shader declarations and independent reflection checks."""
from copy import deepcopy
import json
from pathlib import Path
import unittest

from ludus_tools import render_graph as graph, render_graph_build as build
from ludus_tools import render_graph_buffers as buffers, render_graph_execution as execution
from ludus_tools.errors import ToolingError
import test_render_graph_ray as fixtures


class StructuredBufferTests(unittest.TestCase):
    setUp = fixtures.RayPipelineTests.setUp
    compile = fixtures.RayPipelineTests.compile

    def add_buffers(self):
        configuration = self.document['configurations'][0]
        for name in ('ReservoirA', 'ReservoirB'):
            configuration['nodes'].append({'id': name, 'kind': 'StructuredBuffer', 'stride': 32, 'count': 'ColorTarget'})
            self.node['inputs'].append({'id': name, 'type': 'Buffer', 'required': True})
            self.node['outputs'].append({'id': name, 'type': 'Buffer', 'target': name, 'format': 'Bytes', 'load': 'Load', 'store': 'Store'})
            configuration['edges'].append({'from': name + '.buffer', 'to': 'trace.' + name})
        return configuration

    def test_two_reservoirs_have_explicit_in_place_versions_and_do_not_become_images(self):
        self.add_buffers()
        plan = self.compile()
        storage = plan['passes'][0]['buffers']
        self.assertEqual({'ReservoirA', 'ReservoirB'}, set(storage))
        for name, use in storage.items():
            self.assertEqual({'resource': 'Main/' + name, 'input_version': 0, 'output_version': 1, 'access': 'ReadWrite'}, use)
        _, resources, passes, _ = execution.lower_plan({'plans': [plan]}, 'Default')
        self.assertEqual(1, len(resources))
        self.assertEqual(0, passes[0]['Color'])
        self.assertIn('ReservoirA', graph.dump_plan(plan))

    def test_storage_names_do_not_collide_with_internal_color_scene_or_view_roles(self):
        configuration = self.add_buffers()
        for port in self.node['inputs']:
            if port['id'] == 'color':
                port['id'] = 'target'
            elif port['id'] == 'ReservoirA':
                port['id'] = 'color'
        self.node['outputs'][0]['target'] = 'target'
        self.node['outputs'][1]['target'] = 'color'
        for edge in configuration['edges']:
            if edge['to'] == 'trace.color':
                edge['to'] = 'trace.target'
            elif edge['to'] == 'trace.ReservoirA':
                edge['to'] = 'trace.color'
        plan = self.compile()
        self.assertEqual('Main/ReservoirA', plan['passes'][0]['buffers']['color']['resource'])
        self.assertEqual('Main/color', plan['passes'][0]['attachments']['color']['resource'])

    def test_aliasing_and_discard_load_are_rejected(self):
        configuration = self.add_buffers()
        self.node['outputs'][-1]['load'] = 'Discard'
        with self.assertRaises(ToolingError):
            self.compile()
        self.node['outputs'][-1]['load'] = 'Load'
        configuration['edges'][-1]['from'] = 'ReservoirA.buffer'
        with self.assertRaisesRegex(ToolingError, 'one physical buffer'):
            self.compile()

    def test_stride_count_and_capacity_are_bounded(self):
        configuration = self.add_buffers()
        resource = configuration['nodes'][-1]
        for field, invalid in (('stride', 0), ('stride', 6), ('stride', 4100), ('count', 0), ('count', True), ('count', 2**32)):
            original = resource[field]
            resource[field] = invalid
            with self.subTest(field=field, invalid=invalid), self.assertRaises(ToolingError):
                self.compile()
            resource[field] = original
        self.node['inputs'].append({'id': 'third', 'type': 'Buffer', 'required': True})
        with self.assertRaises(ToolingError):
            self.compile()

    def test_read_only_binding_has_no_write_version_and_reflection_is_exact(self):
        self.add_buffers()
        self.node['outputs'].pop()
        plan = self.compile()
        self.assertEqual('Read', plan['passes'][0]['buffers']['ReservoirB']['access'])
        self.assertEqual(0, plan['passes'][0]['buffers']['ReservoirB']['output_version'])
        entries = [{'name': name, 'binding': slot, 'kind': kind, 'size': 32}
                   for name, slot, kind in (('ReservoirA', 6, 'StorageReadWrite'), ('ReservoirB', 7, 'StorageRead'))]
        stages = {('RayTracing', 'compute', target): {'descriptor': {'interface': {'entries': deepcopy(entries)}}}
                  for target in ('metal', 'spirv')}
        records = buffers.lower_bindings(plan, stages, build)
        self.assertEqual([{'Slot': 6, 'Stride': 32, 'Count': 0, 'IsReadOnly': 0},
                          {'Slot': 7, 'Stride': 32, 'Count': 0, 'IsReadOnly': 1}], records)
        for field, invalid in (('size', 16), ('binding', 5), ('kind', 'UniformBuffer'), ('name', 'wrong')):
            changed = stages[('RayTracing', 'compute', 'spirv')]['descriptor']['interface']['entries'][0]
            original = changed[field]; changed[field] = invalid
            with self.subTest(field=field), self.assertRaises(ToolingError):
                buffers.lower_bindings(plan, stages, build)
            changed[field] = original

    def test_storage_schema_changes_do_not_require_a_new_host_uniform_interface(self):
        from unittest.mock import patch
        from ludus_tools import render_graph_authoring as authoring
        before = {'id': 'Before', 'schemas': [{'id': 'Reservoir', 'role': 'Storage',
                   'fields': [{'id': 'Sample', 'type': 'Float4'}]}], 'programs': []}
        after = deepcopy(before)
        after['id'] = 'After'
        after['schemas'][0]['fields'][0]['type'] = 'UInt4'
        with patch.object(authoring, '_package', side_effect=(before, after)):
            result = authoring.compare_packages('old', 'new')
        self.assertEqual('Compatible', result['cpu_interface'])
        self.assertEqual('Changed', result['schemas'][0]['fields'][0]['status'])

    def test_typed_storage_declarations_share_one_schema_and_validate_slots(self):
        manifest = json.loads((fixtures.FIXTURE / 'main.lushader.json.in').read_text().replace('@RG2_TEST_PROFILES@', '["metal"]'))
        manifest['schemas'].append({'id': 'Reservoir', 'role': 'Storage', 'fields': [{'id': 'Sample', 'type': 'Float4'}]})
        program = next(value for value in manifest['programs'] if value['stage'] == 'ray_generation')
        program['bindings'] = [{'id': name, 'schema': 'Reservoir', 'slot': slot, 'access': access}
                               for name, slot, access in (('ReservoirA', 6, 'Read'), ('ReservoirB', 7, 'ReadWrite'))]
        schemas, _, _, _ = build.validate_manifest(manifest)
        text = buffers.declarations(program['bindings'], schemas, build)
        self.assertEqual(1, text.count('struct Reservoir'))
        self.assertIn('StructuredBuffer<Reservoir> ReservoirA;', text)
        self.assertIn('RWStructuredBuffer<Reservoir> ReservoirB;', text)
        program['bindings'][0]['slot'] = 5
        with self.assertRaises(ToolingError):
            build.validate_manifest(manifest)
