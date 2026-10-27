"""RG2 lowering/admission and dependency regressions; native pixels qualify shaders."""
import copy
import json
from pathlib import Path
import re
import struct
import unittest

import test_render_graph_build as fixtures

ROOT = fixtures.ROOT
from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_execution as execution
from ludus_tools.errors import ToolingError


class ExecutionTests(unittest.TestCase):
    run_build = fixtures.BuildTests.run_build
    value = fixtures.BuildTests.value
    edit = fixtures.BuildTests.edit

    def setUp(self):
        fixtures.BuildTests.setUp(self)
        self.graph = graph.new_workspace(self.root / 'static')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'), renderer_static=True)
        self.stage_slot = 0
        self.has_bias = False
        self.has_visibility = False
        (self.helper / 'visibility_buffer.slang').write_bytes(
            (ROOT / 'modules/graphics/renderer/shaders/visibility_buffer.slang').read_bytes())

    def stage(self, command, cancel):
        fixtures.BuildTests.stage(self, command, cancel)
        output = Path(command[command.index('--output') + 1])
        descriptor = json.loads((output / 'stage.json').read_text())
        if descriptor['stage'] == 'vertex' and self.value()['id'] == 'RendererStaticBuild':
            descriptor['interface']['inputs'] = [
                {'location': i, 'format': 'Float3' if i == 0 else 'Float4', 'name': name}
                for i, name in enumerate(('PositionMesh', 'MeshToClipRow0', 'MeshToClipRow1',
                                         'MeshToClipRow2', 'MeshToClipRow3', 'Color'))]
        if descriptor['stage'] == 'vertex' and self.has_visibility:
            fields = next(schema['fields'] for schema in self.value()['schemas'] if schema['id'] == 'RendererStatic')
            descriptor['interface']['inputs'] = [
                {'location': field['location'], 'format': field['type'], 'name': field['id']} for field in fields]
        for entry in descriptor['interface']['entries']:
            entry['binding'] = self.stage_slot
            if self.has_bias:
                entry['shapes']['bias'] = 'Float32'
                entry['offsets']['bias'] = entry['size']
                entry['size'] += 16
        (output / 'stage.json').write_bytes(build.encoded(descriptor))

    def package(self):
        return build.verify_package(Path(self.run_build()['package']))

    def test_independent_same_family_passes_keep_distinct_entry_points_in_both_emitters(self):
        from ludus_tools import render_graph_pipeline as pipeline
        self.run_build()
        original_interface = execution._runtime_contents(self.output, 'Default')['interface_identity']
        manifest = self.value()
        base = manifest['techniques'][0]
        program = copy.deepcopy(next(p for p in manifest['programs'] if p['id'] == base['fragment']))
        program.update(id='GBuffer', function='GBufferMain')
        manifest['programs'].append(program)
        # Two functions in the same source remain different compiled stage programs.
        source = self.manifest.parent / program['source']
        source.write_text(source.read_text() + '\nfloat4 GBufferMain(RasterVarying value) { return float4(0, 1, 0, 1); }\n')
        variant = copy.deepcopy(base)
        variant.update(id=base['id'] + '.' + base['vertex'] + '.GBuffer', fragment='GBuffer')
        manifest['techniques'].append(variant)
        manifest['budgets'].update(configurations=8, programs=32, artifacts=128, pipelines=64)
        self.manifest.write_bytes(build.encoded(manifest))
        library = self.graph.parent / 'rasterize.lurender.json'
        document = json.loads(library.read_text())
        configuration = copy.deepcopy(document['configurations'][0])
        configuration['id'] = 'GBuffer'
        node = pipeline.migrate_raster(configuration['nodes'][0], base)
        node['shaders'][1]['program'] = 'GBuffer'
        configuration['nodes'] = [node]
        document['version'] = 2
        document['configurations'].append(configuration)
        library.write_bytes(graph.canonical_asset(document))
        document = json.loads(self.graph.read_text())
        configuration = document['configurations'][0]
        configuration['nodes'].extend([
            graph._target('gbufferColor', 'ColorTexture'), graph._target('gbufferDepth', 'DepthTexture'),
            {'id': 'gbuffer', 'kind': 'Subgraph', 'asset': 'rasterize.lurender.json', 'configuration': 'GBuffer'},
            {'id': 'gbufferOutput', 'kind': 'Export', 'type': 'ColorTexture'}])
        configuration['edges'].extend([
            {'from': 'scene.scene', 'to': 'gbuffer.scene'}, {'from': 'camera.view', 'to': 'gbuffer.view'},
            {'from': 'gbufferColor.texture', 'to': 'gbuffer.color'},
            {'from': 'gbufferDepth.texture', 'to': 'gbuffer.depth'},
            {'from': 'gbuffer.color', 'to': 'gbufferOutput.texture'}])
        self.graph.write_bytes(graph.canonical_asset(document))
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        self.assertEqual({'ColorAndDepth', 'ColorAndDepthVariant1'}, {p['Program'] for p in contents['passes']})
        self.assertEqual(original_interface, contents['interface_identity'])
        self.assertTrue(all(p['AllowsRepeatedCoverage'] == 1 for p in contents['passes']))
        colors = [item['bytes'] for key, item in contents['artifacts'].items()
                  if key[1:] == ('fragment', 'metal')]
        self.assertEqual(2, len(colors))
        self.assertNotEqual(colors[0], colors[1])
        runtime = execution.encode_runtime_package(contents)
        self.assertEqual(execution.PACKAGE_VERSION, struct.unpack_from('<I', runtime, 4)[0])
        header = self.root / 'independent.h'
        execution.emit_runtime(self.output, header, name='IndependentGraph')
        text = header.read_text()
        self.assertIn('static_cast<format::Technique>(5)', text)
        self.assertIn('result.AdditionalRasterStages[1].Artifact.Msl', text)
        self.assertIn('GBufferMain(value)', text)
        self.assertIn('PSMain(value)', text)
        self.assertEqual(runtime, execution.encode_runtime_package(execution._runtime_contents(self.output, 'Default')))

    def test_large_linked_shader_uses_character_initializers_without_losing_bytes(self):
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        artifact = next(item for key, item in contents['artifacts'].items() if key[2] == 'metal')
        artifact['bytes'] = ('// ' + 'x' * 70000 + '\n').encode()
        from unittest.mock import patch
        destination = self.root / 'large.h'
        with patch.object(execution, '_runtime_contents', return_value=contents):
            execution.emit_runtime(self.output, destination, name='LargeGraph')
        text = destination.read_text()
        self.assertIn("'\\x78'", text)
        self.assertNotIn('x' * 70000, text)

    def test_numeric_plan_and_static_artifacts_do_not_synthesize_programs(self):
        report = self.run_build()
        destination = self.root / 'graph.h'
        result = execution.emit_runtime(self.output, destination, name='RasterGraph')
        self.assertEqual(result['passes'], 1)
        text = destination.read_text()
        self.assertIn('format::Technique::ColorAndDepth', text)
        self.assertNotIn('result.DepthVertex', text)
        self.assertEqual(build.verify_package(Path(report['package']))['counts']['artifacts'], 6)
        self.assertIn('result.ColorVertex.Artifact.Msl', text)
        self.assertIn('result.ColorVertex.Artifact.Wgsl', text)
        self.assertIn('result.CompositeVertex = compositeVertex', text)
        self.assertNotIn('std::', text)

    def test_shared_render_sizes_lower_to_numeric_sdk_resource_descriptors(self):
        value = json.loads(self.graph.read_text())
        value['version'] = 2
        configuration = value['configurations'][0]
        configuration['nodes'].append({'id': 'size', 'kind': 'RenderSize', 'extent': [97, 65]})
        for node in configuration['nodes']:
            if node['kind'] == 'Target':
                node['extent'] = 'RenderSize'
                configuration['edges'].append({'from': 'size.size', 'to': node['id'] + '.size'})
        self.graph.write_bytes(graph.canonical_asset(value))
        package = self.package()
        _, resources, _, _ = execution.lower_plan(package, 'Default')
        self.assertTrue(all(resource['Width'] == 97 and resource['Height'] == 65 for resource in resources))
        destination = self.root / 'shared_sizes.h'
        execution.emit_runtime(self.output, destination, name='SizedGraph')
        self.assertNotIn('RenderSize', destination.read_text())

    def test_output_and_render_providers_preserve_runtime_resize_metadata(self):
        value = json.loads(self.graph.read_text())
        value['version'] = 2
        configuration = value['configurations'][0]
        configuration['nodes'].extend([
            {'id': 'outputSize', 'kind': 'OutputSize', 'extent': 'Viewport'},
            {'id': 'renderSize', 'kind': 'RenderSize', 'extent': 'OutputSize', 'scale': [2, 3]}])
        configuration['edges'].append({'from': 'outputSize.size', 'to': 'renderSize.output'})
        for node in configuration['nodes']:
            if node['kind'] == 'Target':
                node['extent'] = 'RenderSize'
                configuration['edges'].append({'from': 'renderSize.size', 'to': node['id'] + '.size'})
        self.graph.write_bytes(graph.canonical_asset(value))
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        data = execution.encode_runtime_package(contents)
        for index, resource in enumerate(contents['resources']):
            self.assertEqual((resource['ViewNumerator'], resource['ViewDenominator']), (2, 3))
            self.assertEqual(struct.unpack_from('<9I', data, 104 + index * execution.RESOURCE_RECORD_BYTES)[6:8], (2, 3))
        configuration['nodes'][-2]['extent'] = [1921, 1081]
        self.graph.write_bytes(graph.canonical_asset(value))
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        for resource in contents['resources']:
            self.assertEqual((resource['Width'], resource['Height']), (1281, 721))
            self.assertEqual((resource['ViewNumerator'], resource['ViewDenominator']), (0, 1))

    def test_relative_sizes_survive_lowering_and_runtime_serialization(self):
        value = json.loads(self.graph.read_text())
        value['version'] = 2
        configuration = value['configurations'][0]
        configuration['nodes'].append({'id': 'size', 'kind': 'RenderSize',
                                       'extent': {'reference': 'View', 'scale': [1, 2]}})
        for node in configuration['nodes']:
            if node['kind'] == 'Target':
                node['extent'] = 'RenderSize'
                configuration['edges'].append({'from': 'size.size', 'to': node['id'] + '.size'})
        self.graph.write_text(json.dumps(value))
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        data = execution.encode_runtime_package(contents)
        for index, resource in enumerate(contents['resources']):
            self.assertEqual((resource['ViewNumerator'], resource['ViewDenominator']), (1, 2))
            self.assertEqual(struct.unpack_from('<9I', data, 104 + index * execution.RESOURCE_RECORD_BYTES)[6:8], (1, 2))
        destination = self.root / 'relative.h'
        execution.emit_runtime(self.output, destination, name='RelativeGraph')
        self.assertIn('1, 2, 0}', destination.read_text())

    def test_depth_only_export_has_explicit_technique_and_private_backing(self):
        self.graph = graph.new_workspace(self.root / 'depth', output='depth')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'), renderer_static=True)
        package = self.package()
        _, resources, passes, _ = execution.lower_plan(package, 'Default')
        self.assertEqual(resources[0]['Kind'], 'Depth')
        self.assertEqual(resources[0]['Backing'], 0)
        self.assertEqual(resources[0]['IsExported'], 1)
        self.assertEqual(passes[0]['Program'], 'DepthOnly')
        self.assertEqual(passes[0]['Color'], execution.ABSENT)
        self.assertEqual(passes[0]['ColorStore'], 'Discard')

    def test_admission_rejects_unknown_state_import_history_format_and_samples(self):
        package = self.package()
        for field, value in [('cull', 'Back'), ('depth_write', False), ('blend', 'Premultiplied')]:
            changed = copy.deepcopy(package)
            for recipe in changed['recipes']:
                recipe['state'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ToolingError, 'admitted static Renderer state'):
                execution.lower_plan(changed, 'Default')
        for field, value in [('initialized', True), ('samples', 4), ('format', 'RGBA16Float')]:
            changed = copy.deepcopy(package)
            resource = next(iter(changed['plans'][0]['resources'].values()))
            resource[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ToolingError, 'unqualified'):
                execution.lower_plan(changed, 'Default')

    def test_distinct_logical_depths_cannot_alias_one_color_owner(self):
        package = self.package()
        plan = package['plans'][0]
        depth = next(r for r, value in plan['resources'].items() if value['type'] == 'DepthTexture')
        plan['resources']['SecondDepth'] = copy.deepcopy(plan['resources'][depth])
        operation = copy.deepcopy(plan['passes'][0])
        operation['attachments']['depth']['resource'] = 'SecondDepth'
        plan['passes'].insert(1, operation)
        with self.assertRaisesRegex(ToolingError, 'distinct logical images'):
            execution.lower_plan(package, 'Default')

    def test_integrity_failure_preserves_generated_header(self):
        self.run_build()
        destination = self.root / 'graph.h'
        execution.emit_runtime(self.output, destination)
        original = destination.read_bytes()
        (self.output / 'current.json').write_text('{"version":1,"generation":"../bad"}')
        with self.assertRaisesRegex(ToolingError, 'pointer'):
            execution.emit_runtime(self.output, destination)
        self.assertEqual(destination.read_bytes(), original)

    def test_generic_rg1_interface_requires_explicit_runtime_profile(self):
        self.graph = graph.new_workspace(self.root / 'generic')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'))
        self.run_build()
        with self.assertRaisesRegex(ToolingError, 'renderer-static'):
            execution.emit_runtime(self.output, self.root / 'generic.h')

    def test_neutral_format_limits_match_offline_lowering(self):
        header = (ROOT / 'modules/content/render_graph_format/include/ludus/content/render_graph_format/plan.h').read_text()
        for name in ('MAX_PASSES', 'MAX_RESOURCES', 'MAX_CAMERAS', 'VERSION', 'MAX_UNIFORM_VALUES',
                     'MAX_UNIFORM_BINDINGS', 'MAX_UNIFORM_FIELDS', 'MAX_UNIFORM_BLOCK_BYTES', 'UNIFORM_SLOT_COUNT'):
            self.assertEqual(int(re.search(r'\b' + name + r' = (\d+);', header).group(1)),
                             getattr(execution, 'FORMAT_VERSION' if name == 'VERSION' else name))

    def test_nested_source_graph_preserves_package_relative_include_paths(self):
        nested = self.root / 'nested sources'
        self.graph = graph.new_workspace(nested / 'graphs')
        document = json.loads(self.graph.read_text())
        document['configurations'][0]['nodes'][4]['asset'] = 'graphs/rasterize.lurender.json'
        self.graph.write_text(json.dumps(document))
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'), root=nested)
        self.assertTrue(self.value()['programs'][0]['source'].startswith('graphs/main_shaders/'))
        report = self.run_build(root=nested)
        self.assertEqual(build.verify_package(Path(report['package']))['source_map']['build_manifest'],
                         'graphs/main.lushader.json')

    def test_packer_concatenation_collision_rejects_instead_of_overwriting(self):
        schema = {'S': {'fields': []}}
        layout = {'schema': 'S', 'size': 4, 'fields': [], 'binding': 'B_metal_C'}
        other = dict(layout, binding='C')
        with self.assertRaisesRegex(ToolingError, 'packer name collision'):
            build.cpp_header(schema, {('A', 'metal'): {'layouts': [layout]},
                                     ('A_metal_B', 'metal'): {'layouts': [other]}})

    def test_depth_wrapper_declares_void_contract_before_author_code(self):
        # Inspect a complete emitted package's readable wrapper, not a synthetic regex on author code.
        package = Path(self.run_build()['package'])
        document = build.verify_package(package)
        depth = next(p for p in document['programs'] if p['id'] == 'Depth')
        wrapper = (package / 'programs' / depth['key'] / 'wrapper.slang').read_text()
        self.assertLess(wrapper.index('void DepthMain('), wrapper.index('#include "'))


    def bind_material(self, value, *, vertex=False):
        value['schemas'].append({'id': 'Material', 'role': 'Uniform',
                                 'fields': [{'id': 'tint', 'type': 'Float3'}, {'id': 'gain', 'type': 'Float32'}]})
        for program in value['programs']:
            if program['id'] == 'Color' or (vertex and program['id'] == 'Geometry'):
                program['bindings'] = [{'id': 'material', 'schema': 'Material', 'slot': 0}]

    def mask_techniques(self, value):
        for technique in value['techniques']:
            technique['id'] = technique['id'].replace('DefaultStaticOpaque.', 'DefaultStaticMasked.')
        rasterize = self.graph.parent / 'rasterize.lurender.json'
        document = json.loads(rasterize.read_text())
        for configuration in document['configurations']:
            for node in configuration['nodes']:
                if node['kind'] == 'Raster':
                    node['material'] = 'DefaultMasked'
        rasterize.write_text(json.dumps(document))

    def test_bound_uniforms_lower_to_target_tables_and_named_values(self):
        self.edit(self.bind_material)
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        self.assertEqual(contents['uniform_values'], 4)
        self.assertEqual([{k: v for k, v in b.items() if k not in ('schema', 'layouts')} for b in contents['bindings']],
                         [{'Program': 'ColorAndDepth', 'Slot': 0, 'Stages': 'Fragment', 'ValueOffset': 0, 'ValueCount': 4}])
        self.assertEqual(contents['blocks'], [{'Binding': 0, 'Target': 'metal', 'Size': 32},
                                              {'Binding': 0, 'Target': 'wgsl', 'Size': 16}])
        self.assertEqual([(f['Target'], f['ValueOffset'], f['Components'], f['ByteOffset']) for f in contents['fields']],
                         [('metal', 0, 3, 0), ('metal', 3, 1, 16), ('wgsl', 0, 3, 0), ('wgsl', 3, 1, 12)])
        destination = self.root / 'bound.h'
        result = execution.emit_runtime(self.output, destination, name='BoundGraph')
        self.assertEqual((result['uniform_values'], result['uniform_bindings']), (4, 1))
        text = destination.read_text()
        self.assertIn('UNIFORM_VALUE_COUNT = 4;', text)
        self.assertIn('UNIFORM_VALUE_MATERIAL_TINT = 0;', text)
        self.assertIn('UNIFORM_VALUE_MATERIAL_GAIN = 3;', text)
        self.assertIn('{format::Technique::ColorAndDepth, 0, format::Visibility::Fragment, 0, 4},', text)
        self.assertIn('{0, format::ShaderTarget::Wgsl, 3, 1, 12},', text)
        # The stage admits the largest carried block so one layout serves every target.
        self.assertIn('{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, 32},', text)
        self.assertIn('result.ColorFragment.Bindings = ColorFragmentBindings.AsSpan();', text)
        self.assertNotIn('ColorVertexBindings', text)
        runtime = execution.cook_runtime(self.output, self.root / 'bound.lurgrt')
        self.assertEqual(runtime['interface_identity'], result['interface_identity'])
        data = (self.root / 'bound.lurgrt').read_bytes()
        header = struct.unpack('<10I', data[:40])
        self.assertEqual(header[:2], (execution.PACKAGE_MAGIC, execution.PACKAGE_VERSION))
        self.assertEqual(header[5:9], (4, 1, 2, 4))
        records = 104 + execution.RESOURCE_RECORD_BYTES * 2 + 4 + 20 * 4
        self.assertEqual(struct.unpack('<5I', data[records:records + 20]), (0, 0, 2, 0, 4))

    def test_runtime_package_names_its_resources_in_record_order(self):
        self.run_build()
        destination = self.root / 'named.lurgrt'
        runtime = execution.cook_runtime(self.output, destination)
        sidecar = execution.resource_names_path(destination)
        self.assertEqual(runtime['resource_names'], str(sidecar))
        self.assertEqual(runtime['sha256'], build.digest(destination.read_bytes()))
        self.assertEqual(sidecar.name, 'named.lurgrt.resources.json')
        names = json.loads(sidecar.read_text())
        self.assertEqual(names['version'], execution.RESOURCE_NAMES_VERSION)
        self.assertEqual(names['generation'], runtime['generation'])
        self.assertEqual(names['runtime_sha256'], runtime['sha256'])
        contents = execution._runtime_contents(self.output, 'Default')
        pass_names = [p['id'] for p in contents['plan']['passes'] if p['kind'] in ('Raster', 'RayTracing')]
        suffix = b''.join(struct.pack('<I', len(name.encode())) + name.encode() + bytes(-len(name.encode()) % 4)
                          for name in pass_names)
        self.assertTrue(destination.read_bytes().endswith(suffix))
        header = self.root / 'named.hpp'
        execution.emit_runtime(self.output, header)
        self.assertIn('PASS_NAMES.GetData()', header.read_text())
        self.assertEqual(len(names['resources']), len(contents['resources']))
        self.assertEqual([entry['name'] for entry in names['resources']], sorted(contents['plan']['resources']))
        for entry, record in zip(names['resources'], contents['resources']):
            self.assertEqual(entry['extent'], [record['Width'], record['Height']])
            self.assertEqual(entry['type'] == 'DepthTexture', record['Kind'] == 'Depth')
            self.assertIn(entry['format'], ('RGBA8Unorm', 'Depth32Float'))
        operations = {operation['id']: operation for operation in names['operations']}
        raster = next(operation for operation in operations.values() if operation['kind'] == 'Raster')
        self.assertIn(raster['attachments']['color']['resource'], [entry['name'] for entry in names['resources']])
        self.assertEqual(raster['attachments']['depth']['store'], 'Store')
        roots = [operation for operation in operations.values() if operation['kind'] in ('Export', 'Present')]
        self.assertEqual(len(roots), 1)
        self.assertEqual(set(roots[0]['attachments']), {'texture'})
        self.assertTrue(all(camera['target'] for camera in names['cameras']))
        self.assertEqual(sidecar.read_bytes(), execution.encode_resource_names(
            contents, runtime_identity=runtime['sha256']))

    def test_vertex_and_fragment_sharing_a_slot_merge_into_one_binding(self):
        self.edit(lambda value: self.bind_material(value, vertex=True))
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        self.assertEqual([b['Stages'] for b in contents['bindings']], ['Both'])
        stage = {'id': 'Color', 'descriptor': {'layouts': [{'slot': 0, 'schema': 'Material', 'size': 16,
                                                             'fields': [{'id': 'tint', 'type': 'Float3', 'offset': 0},
                                                                        {'id': 'gain', 'type': 'Float32', 'offset': 12}]}]}}
        other = copy.deepcopy(stage)
        other['descriptor']['layouts'][0]['fields'][1]['offset'] = 8
        package = {'schemas': [{'id': 'Material', 'role': 'Uniform',
                                'fields': [{'id': 'tint', 'type': 'Float3'}, {'id': 'gain', 'type': 'Float32'}]}]}
        with self.assertRaisesRegex(ToolingError, 'one schema and layout'):
            execution._lower_uniforms(package, {('ColorAndDepth', 'vertex', 'wgsl'): stage,
                                                ('ColorAndDepth', 'fragment', 'wgsl'): other})

    def visibility_fields(self):
        # Read the renderer-owned schema rather than recreating the expected layout in a fixture.
        from ludus_tools import render_graph_templates as templates
        files = templates.template_files('visibility-buffer', ('metal', 'wgsl'), graph)
        manifest = json.loads(files['main.lushader.json'])
        return next(schema['fields'] for schema in manifest['schemas'] if schema['id'] == 'RendererStatic')

    def test_supported_visibility_layout_keeps_the_compiled_static_interface(self):
        self.run_build()
        original_interface = execution._runtime_contents(self.output, 'Default')['interface_identity']
        # This is the fingerprint already compiled into the stock Metal/WGSL editor.
        self.assertEqual('7c90fffb376e45500932936927e1dbe7eebab7e2e0b975108253787e93ace25b', original_interface)
        fields = self.visibility_fields()
        self.edit(lambda manifest: manifest['schemas'][0].update(fields=fields))
        self.has_visibility = True
        self.run_build()
        contents = execution._runtime_contents(self.output, 'Default')
        self.assertEqual(original_interface, contents['interface_identity'])
        vertex_inputs = [artifact['descriptor']['interface']['inputs']
                         for key, artifact in contents['artifacts'].items() if key[1] == 'vertex']
        self.assertTrue(all(len(inputs) == len(fields) for inputs in vertex_inputs))
        destination = self.root / 'visibility.h'
        execution.emit_runtime(self.output, destination)
        self.assertIn('0x7c, 0x90, 0xff, 0xfb', destination.read_text())
        # The runtime and compiled-header emitters share the compatibility identity;
        # neither rewrites the real eight-location shader inputs.
        runtime = execution.cook_runtime(self.output, self.root / 'visibility.lurgrt')
        self.assertEqual(original_interface, runtime['interface_identity'])

    def test_incompatible_visibility_or_base_fields_do_not_match_the_static_host(self):
        self.run_build()
        original_interface = execution._runtime_contents(self.output, 'Default')['interface_identity']
        original = self.value()
        cases = []
        for field_index in (0, 6, 7):
            for key, value in (('id', 'OtherField'), ('semantic', 'TEXCOORD9'), ('type', 'Float4'), ('location', 9)):
                fields = self.visibility_fields()
                fields[field_index][key] = value
                cases.append((f'{field_index}:{key}', fields))
        reordered = self.visibility_fields()
        reordered[-2:] = reversed(reordered[-2:])
        cases.append(('reordered extension', reordered))
        partial = self.visibility_fields()[:-1]
        cases.append(('partial extension', partial))
        extra = self.visibility_fields()
        extra.append({'id': 'OtherField', 'type': 'Float2', 'semantic': 'TEXCOORD9', 'location': 8})
        cases.append(('additional field', extra))
        self.has_visibility = True
        for name, fields in cases:
            with self.subTest(name=name):
                manifest = copy.deepcopy(original)
                manifest['schemas'][0]['fields'] = fields
                self.manifest.write_bytes(build.encoded(manifest))
                try:
                    self.run_build()
                    identity = execution._runtime_contents(self.output, 'Default')['interface_identity']
                except ToolingError as error:
                    # Unsupported physical input formats/locations fail admission before activation.
                    self.assertEqual('RenderGraphBuildConfiguration', error.code)
                    self.assertRegex(str(error), 'unsupported static/visibility vertex interface')
                    continue
                self.assertNotEqual(original_interface, identity)

    def test_interface_identity_changes_with_uniform_schemas_and_layouts(self):
        self.run_build()
        unbound = execution._runtime_contents(self.output, 'Default')['interface_identity']
        self.edit(self.bind_material)
        self.run_build()
        bound = execution._runtime_contents(self.output, 'Default')['interface_identity']
        def add_field(value):
            value['schemas'][-1]['fields'].append({'id': 'bias', 'type': 'Float32'})
        self.edit(add_field)
        self.has_bias = True
        self.run_build()
        widened = execution._runtime_contents(self.output, 'Default')['interface_identity']
        def move_slot(value):
            next(p for p in value['programs'] if p['id'] == 'Color')['bindings'][0]['slot'] = 1
        self.edit(move_slot)
        self.stage_slot = 1
        self.run_build()
        moved = execution._runtime_contents(self.output, 'Default')['interface_identity']
        self.assertEqual(len({unbound, bound, widened, moved}), 4)

    def test_masked_techniques_lower_to_explicit_masked_programs(self):
        self.edit(self.mask_techniques)
        self.run_build()
        _, _, passes, _ = execution.lower_plan(self.package(), 'Default')
        self.assertEqual(passes[0]['Program'], 'MaskedColorAndDepth')
        destination = self.root / 'masked.h'
        execution.emit_runtime(self.output, destination)
        text = destination.read_text()
        self.assertIn('format::Technique::MaskedColorAndDepth', text)
        self.assertIn('result.MaskedColorFragment.Artifact.Wgsl', text)
        self.assertNotIn('result.ColorFragment', text)

    def test_masked_depth_requires_its_own_authored_coverage_function(self):
        def declare_both(value):
            masked = copy.deepcopy(value['techniques'])
            for technique in masked:
                technique['id'] = technique['id'].replace('DefaultStaticOpaque.', 'DefaultStaticMasked.')
            value['techniques'] += masked
        self.edit(declare_both)
        with self.assertRaisesRegex(ToolingError, 'separately authored depth function'):
            self.run_build()

    def test_unqualified_materials_layouts_and_history_still_reject(self):
        rasterize = self.graph.parent / 'rasterize.lurender.json'
        original = rasterize.read_text()
        for field, value in [('material', 'DefaultTransparent'), ('vertex_layout', 'RendererSkinned')]:
            document = json.loads(original)
            document['configurations'][0]['nodes'][0][field] = value
            rasterize.write_text(json.dumps(document))
            with self.subTest(field=field), self.assertRaises(ToolingError):
                graph.compile_graph(self.graph)
        rasterize.write_text(original)
        package = self.package()
        for operation in package['plans'][0]['passes']:
            if operation['kind'] == 'Raster':
                operation['material'] = 'DefaultMasked'
        with self.assertRaisesRegex(ToolingError, 'unqualified operation'):
            execution.lower_plan(package, 'Default')


if __name__ == '__main__':
    unittest.main()
