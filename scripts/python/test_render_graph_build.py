"""RG1 build contract/invalidation/publication regressions with controlled compiler IO."""
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph
from ludus_tools import render_graph_build as build
from ludus_tools.errors import ToolingError
from ludus_tools.cli import main

ROOT = Path(__file__).resolve().parents[2]


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.graph = graph.new_workspace(self.root / 'sources')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'))
        self.helper = self.root / 'sdk'
        self.helper.mkdir()
        for name in build.HELPERS:
            source = ROOT / ('config' if name.endswith('.json') else 'cmake/shaders') / name
            shutil.copy(source, self.helper / name)
        self.compiler = self.root / 'slangc'
        self.compiler.write_text('test tool identity')
        self.output = self.root / 'build'
        self.calls = []
        self.invalid_layout = False
        self.invalid_links = False
        self.invalid_dependency = False
        self.after_compile = None
        self.runner = patch.object(build, 'run_stage', self.stage)
        self.runner.start()
        self.addCleanup(self.runner.stop)
        self.addCleanup(self.temp.cleanup)

    def value(self):
        return json.loads(self.manifest.read_text())

    def edit(self, function):
        value = self.value()
        function(value)
        self.manifest.write_text(json.dumps(value))

    def stage(self, command, cancel):
        def arg(name):
            return command[command.index(name) + 1]
        target, stage = arg('--graph-target'), arg('--graph-stage')
        source = Path(arg('--source'))
        code = source.read_text()
        self.calls.append((target, stage, code))
        output = Path(arg('--output'))
        output.mkdir()
        entries, raw = [], []
        if 'ConstantBuffer<Material>' in code:
            offset, size = (16, 32) if target == 'metal' else (12, 16)
            entries = [{'binding': 0, 'name': 'material', 'kind': 'UniformBuffer', 'size': size,
                        'offsets': {'tint': 0, 'gain': offset}, 'shapes': {'tint': 'Float3', 'gain': 'Float32'}}]
            raw = [{'name': 'material'}]
        if self.invalid_layout and entries:
            entries[0]['shapes']['tint'] = 'Float4'
        links = [{'location': 0, 'type': 'Float4'}] if self.invalid_links and stage == 'fragment' else []
        result = {'version': 1, 'target': target, 'stage': stage, 'source_entry': 'rgEntry', 'entry': 'rgEntry',
                  'artifact': 'artifact.txt', 'interface': {'entries': entries, 'inputs': [
                      {'location': 0, 'format': 'Float3', 'name': 'position'}] if stage == 'vertex' else []}, 'links': links}
        (output / 'artifact.txt').write_text('artifact\n' + code)
        result['sha256'] = build.digest((output / 'artifact.txt').read_bytes())
        (output / 'stage.json').write_bytes(build.encoded(result))
        (output / 'reflection.json').write_bytes(build.encoded({'parameters': raw}))
        dependency = str(source) if not self.invalid_dependency else '/untracked/source.slang'
        (output / 'stage.d').write_text('artifact: ' + dependency.replace(' ', '\\ ') + '\n')
        if self.after_compile:
            self.after_compile()

    def run_build(self, **kwargs):
        return build.build_package(self.manifest, self.output, helper=self.helper, compiler=self.compiler, **kwargs)

    def pointer(self):
        return (self.output / 'current.json').read_bytes()

    def test_clean_warm_and_independent_clean_builds_are_identical(self):
        first = self.run_build()
        self.assertEqual((first['cache_hits'], first['cache_misses']), (0, 6))
        second = self.run_build()
        self.assertEqual((second['cache_hits'], second['cache_misses']), (6, 0))
        self.assertEqual(first['generation'], second['generation'])
        self.output = self.root / 'other build with spaces'
        third = self.run_build()
        self.assertEqual(first['generation'], third['generation'])
        package = build.verify_package(Path(third['package']))
        self.assertEqual(package['counts']['programs'], 3)
        self.assertEqual(package['source_map']['build_manifest'], 'main.lushader.json')
        self.assertEqual(len(package['recipes']), 4)
        self.assertIn('PipelineReadiness', package['checks_pending'])
        self.assertEqual(len({p['key'] for p in package['programs'] if p['id'] == 'Geometry'}), 2)

    def test_target_packing_is_independent_and_generated_cpp_is_documented(self):
        report = self.run_build()
        package = build.verify_package(Path(report['package']))
        offsets = {p['profile']: p['descriptor']['layouts'][0]['fields'][1]['offset']
                   for p in package['programs'] if p['id'] == 'Color'}
        self.assertEqual(offsets, {'metal': 16, 'wgsl': 12})
        header = (Path(report['package']) / 'schemas.h').read_text()
        self.assertIn('float32 tint[3]', header)
        self.assertIn('Pack_Color_metal_material', header)
        self.assertIn('output[16 + i]', header)
        self.assertNotIn('std::', header)
        self.assertIn('preserves all bytes on failure', header)

    def test_transitive_include_invalidation_only_rebuilds_affected_stage(self):
        color = self.graph.parent / 'main_shaders/color.slang'
        include = color.with_name('surface.slang')
        include.write_text('float factor() { return 1; }\n')
        color.write_text('#include "surface.slang"\n' + color.read_text())
        self.run_build()
        include.write_text('float factor() { return 2; }\n')
        changed = self.run_build()
        self.assertEqual((changed['cache_hits'], changed['cache_misses']), (4, 2))

    def test_schema_change_only_invalidates_using_programs(self):
        self.run_build()
        self.edit(lambda v: v['schemas'][2]['fields'].append({'id': 'extra', 'type': 'Float4'}))
        # Real helper reflects all fields; provide corresponding synthetic result.
        self.invalid_layout = True
        with self.assertRaisesRegex(ToolingError, 'field identities/types'):
            self.run_build()
        self.assertEqual(len(self.calls), 7)

    def test_graph_layout_change_reuses_all_stage_artifacts(self):
        first = self.run_build()
        value = json.loads(self.graph.read_text())
        value['configurations'][0]['layout'] = {'nodes': []}
        # RG0 layout is opaque bounded JSON; use its accepted format.
        value['configurations'][0]['layout'] = {'positions': []}
        self.graph.write_text(json.dumps(value))
        second = self.run_build()
        self.assertEqual(second['cache_misses'], 0)
        self.assertNotEqual(first['generation'], second['generation'])

    def test_pipeline_state_change_reuses_stages_and_changes_recipe_key(self):
        first = build.verify_package(Path(self.run_build()['package']))
        self.edit(lambda v: v['techniques'][0]['recipes'][0].update(cull='None'))
        changed = self.run_build()
        second = build.verify_package(Path(changed['package']))
        self.assertEqual(changed['cache_misses'], 0)
        self.assertNotEqual(first['recipes'][0]['key'], second['recipes'][0]['key'])

    def test_disconnected_color_never_creates_an_implicit_technique(self):
        self.graph = graph.new_workspace(self.root / 'depth_sources', output='depth')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal', 'wgsl'))
        package = build.verify_package(Path(self.run_build()['package']))
        self.assertEqual(package['counts']['configurations'], 2)
        self.assertEqual(package['plans'][0]['techniques'], ['DefaultStaticOpaque.DepthOnly'])
        self.assertEqual(package['counts']['artifacts'], 6)

    def test_unused_color_with_depth_consumer_keeps_authored_color_stages(self):
        value = graph._root_asset('offscreen')
        config = value['configurations'][0]
        for node in config['nodes']:
            if node['id'] == 'camera':
                node['target_type'] = 'DepthTexture'
            if node['id'] == 'output':
                node['type'] = 'DepthTexture'
        for edge in config['edges']:
            if edge['to'] == 'camera.target':
                edge['from'] = 'depth.texture'
            if edge['to'] == 'output.texture':
                edge['from'] = 'raster.depth'
        self.graph.write_bytes(graph.canonical_asset(value))
        report = self.run_build()
        package = build.verify_package(Path(report['package']))
        self.assertEqual(package['plans'][0]['techniques'], ['DefaultStaticOpaque.ColorAndDepth'])
        raster = package['plans'][0]['passes'][0]
        self.assertIn('color', raster['attachments'])
        self.assertEqual(package['counts']['artifacts'], 6)

    def test_extent_and_inactive_camera_edits_reuse_all_stage_keys(self):
        self.run_build()
        value = json.loads(self.graph.read_text())
        nodes = value['configurations'][0]['nodes']
        for node in nodes:
            if node['kind'] == 'Target':
                node['extent'] = [320, 240]
        nodes.append({'id': 'OtherCamera', 'kind': 'Camera', 'active': False,
                      'role': 'Presentation', 'target_type': 'ColorTexture'})
        self.graph.write_bytes(graph.canonical_asset(value))
        self.assertEqual(self.run_build()['cache_misses'], 0)

    def test_corrupt_cache_artifact_rebuilds_only_its_stage(self):
        first = self.run_build()
        path = next((self.output / 'cache').glob('*/artifact.txt'))
        path.write_text('corruption')
        second = self.run_build()
        self.assertEqual(second['cache_misses'], 1)
        self.assertEqual(first['generation'], second['generation'])

    def test_corrupt_cache_descriptor_rebuilds(self):
        self.run_build()
        path = next((self.output / 'cache').glob('*/cache.json'))
        value = json.loads(path.read_text())
        value['result']['entry'] = 'incorrect'
        path.write_text(json.dumps(value))
        self.assertEqual(self.run_build()['cache_misses'], 1)

    def test_corrupt_cache_inventory_rebuilds(self):
        self.run_build()
        path = next((self.output / 'cache').glob('*/cache.json'))
        for bad in ('{broken', json.dumps({'identity': None, 'files': [1]})):
            path.write_text(bad)
            self.assertEqual(self.run_build()['cache_misses'], 1)

    def test_corrupt_package_is_rejected(self):
        path = Path(self.run_build()['package'])
        (path / 'schemas.h').write_text('broken')
        with self.assertRaisesRegex(ToolingError, 'digest mismatch'):
            build.verify_package(path)

    def test_package_extra_file_and_symlink_are_rejected(self):
        path = Path(self.run_build()['package'])
        (path / 'extra').write_text('untracked')
        with self.assertRaisesRegex(ToolingError, 'inventory mismatch'):
            build.verify_package(path)
        (path / 'extra').unlink()
        data = (path / 'schemas.h').read_bytes()
        (path / 'schemas.h').unlink()
        elsewhere = self.root / 'external.h'
        elsewhere.write_bytes(data)
        (path / 'schemas.h').symlink_to(elsewhere)
        with self.assertRaises(ToolingError):
            build.verify_package(path)

    def test_cancel_before_and_after_compilation_preserves_previous_package(self):
        self.run_build()
        prior = self.pointer()
        with self.assertRaisesRegex(ToolingError, 'cancelled'):
            self.run_build(cancel=lambda: True)
        self.assertEqual(prior, self.pointer())
        (self.graph.parent / 'main_shaders/color.slang').write_text('// edit\n')
        cancelled = [False]
        self.after_compile = lambda: cancelled.__setitem__(0, True)
        with self.assertRaisesRegex(ToolingError, 'cancelled'):
            self.run_build(cancel=lambda: cancelled[0])
        self.assertEqual(prior, self.pointer())
        self.assertFalse(list(self.output.glob('.rg1-*')))

    def test_stale_source_preserves_previous_package(self):
        self.run_build()
        prior = self.pointer()
        source = self.graph.parent / 'main_shaders/color.slang'
        source.write_text('// initial edit\n')
        self.after_compile = lambda: source.write_text('// newer edit\n')
        with self.assertRaisesRegex(ToolingError, 'changed during build'):
            self.run_build()
        self.assertEqual(prior, self.pointer())

    def test_stale_tools_preserves_previous_package(self):
        self.run_build()
        prior = self.pointer()
        source = self.graph.parent / 'main_shaders/color.slang'
        source.write_text('// edit\n')
        previous_cache = {p.name for p in (self.output / 'cache').iterdir()}
        self.after_compile = lambda: self.compiler.write_text('changed tool')
        with self.assertRaisesRegex(ToolingError, 'toolchain changed'):
            self.run_build()
        self.assertEqual(prior, self.pointer())
        self.assertEqual({p.name for p in (self.output / 'cache').iterdir()}, previous_cache)

    def test_shader_failure_preserves_previous_package(self):
        self.run_build()
        prior = self.pointer()
        self.compiler.write_text('different tool')
        with patch.object(build, 'run_stage', side_effect=ToolingError('RenderGraphBuildShader', 'compile failed')):
            with self.assertRaises(ToolingError):
                self.run_build()
        self.assertEqual(prior, self.pointer())

    def test_failed_report_write_cannot_activate(self):
        self.run_build()
        prior = self.pointer()
        self.edit(lambda v: v['techniques'][0]['recipes'][0].update(cull='None'))
        with patch.object(build, 'atomic', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                self.run_build()
        self.assertEqual(prior, self.pointer())

    def test_concurrent_writer_is_busy(self):
        self.output.mkdir()
        with build.BuildTreeLock(self.output):
            with self.assertRaisesRegex(ToolingError, 'another CLI'):
                self.run_build()

    def test_untracked_compiler_dependency_is_rejected(self):
        self.invalid_dependency = True
        with self.assertRaisesRegex(ToolingError, 'uncaptured dependency'):
            self.run_build()
        self.assertFalse((self.output / 'current.json').exists())

    def test_layout_and_stage_link_failure_preserve_prior_output(self):
        self.run_build()
        prior = self.pointer()
        self.compiler.write_text('edit identity')
        self.invalid_layout = True
        with self.assertRaisesRegex(ToolingError, 'field identities/types'):
            self.run_build()
        self.assertEqual(prior, self.pointer())
        self.invalid_layout = False
        self.invalid_links = True
        self.compiler.write_text('edit again')
        with self.assertRaisesRegex(ToolingError, 'stage links mismatch'):
            self.run_build()
        self.assertEqual(prior, self.pointer())

    def test_capacity_and_configuration_budgets_fail_before_compilation(self):
        for key in build.LIMITS:
            value = self.value()
            value['budgets'][key] = 0
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)
        self.edit(lambda v: v['budgets'].update(artifacts=1))
        with self.assertRaisesRegex(ToolingError, 'authored budget'):
            self.run_build()
        self.assertFalse(self.calls)

    def test_no_missing_or_unused_stage_programs(self):
        for change in (lambda v: v['techniques'][0].update(fragment='Missing'),
                       lambda v: v['techniques'][0].update(vertex='Color'),
                       lambda v: v['techniques'][0].update(configuration='Implicit'),
                       lambda v: v['programs'][0].update(output='Color')):
            value = self.value(); change(value)
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)

    def test_structs_resources_arrays_and_matrix_profiles_are_closed(self):
        for bad in ('Bool', 'Float4x4', 'Float4Array', 'Pointer', 'Texture2D'):
            value = self.value()
            value['schemas'][2]['fields'][0]['type'] = bad
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)
        self.edit(lambda v: v['schemas'][0].update(id='class'))
        with self.assertRaises(ToolingError):
            self.run_build()

    def test_shader_include_escape_dynamic_import_and_recursion_fail(self):
        source = self.graph.parent / 'main_shaders/color.slang'
        for text in ('#include "../../external.slang"', '#include MACRO', 'import outside;', '#include "color.slang"'):
            source.write_text(text)
            with self.assertRaises(ToolingError):
                self.run_build()
        outside = self.root / 'external.slang'
        outside.write_text('external')
        link = source.with_name('external.slang'); link.symlink_to(outside)
        source.write_text('#include "external.slang"')
        with self.assertRaisesRegex(ToolingError, 'symlink escapes'):
            self.run_build()

    def test_graph_selecting_undeclared_technique_fails(self):
        self.edit(lambda v: (v['techniques'].pop(0), v['programs'].pop(1)))
        with self.assertRaisesRegex(ToolingError, 'undeclared technique'):
            self.run_build()
        self.assertFalse(self.calls)

    def test_duplicate_nonfinite_and_unknown_json_fields_fail(self):
        for raw in ('{"version":1,"version":1}', '{"version":NaN}', '{broken'):
            self.manifest.write_text(raw)
            with self.assertRaises(ToolingError):
                self.run_build()

    def test_malformed_members_report_explicit_schema_errors(self):
        for change in (lambda v: v.update(profiles=[{}]), lambda v: v['programs'][0].update(input={}),
                       lambda v: v['techniques'][0].update(vertex=[]), lambda v: v['programs'][1]['bindings'][0].update(schema=[])):
            value = self.value(); change(value)
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)

    def test_metal_emitted_layout_and_reflection_must_agree(self):
        import sys
        sys.path.insert(0, str(ROOT / 'cmake/shaders'))
        from compile_graph_stage import metal_layouts, varying_interface
        interface = {'entries': [{'name': 'material', 'size': 32, 'offsets': {'tint': 0, 'gain': 16}}]}
        raw = {'parameters': [{'name': 'material', 'type': {'elementType': {'name': 'Material'}}}]}
        code = 'struct Material_0 { float3 tint_0; float gain_0; };'
        metal_layouts(code, interface, raw)
        with self.assertRaisesRegex(RuntimeError, 'packing differs'):
            metal_layouts(code.replace('float3', 'float2'), interface, raw)
        with self.assertRaisesRegex(RuntimeError, 'scalar/vector'):
            metal_layouts(code.replace('float gain_0', 'int gain_0'), interface, raw)
        vector = {'kind': 'vector', 'elementCount': 4, 'elementType': {'scalarType': 'float32'}}
        point = {'result': {'type': {'kind': 'struct', 'fields': [
            {'type': vector, 'semanticName': 'SV_POSITION'},
            {'type': vector, 'semanticName': 'COLOR', 'binding': {'kind': 'varyingOutput', 'index': 0}}]}}}
        self.assertEqual(varying_interface(point, 'vertex'), [{'location': 0, 'type': 'Float4'}])

    def test_glsl_depth_fragment_can_drop_unused_varyings(self):
        import sys
        sys.path.insert(0, str(ROOT / 'cmake/shaders'))
        from compile_graph_stage import verify_glsl_links
        color = {'location': 0, 'type': 'Float4'}
        self.assertEqual(verify_glsl_links([color], [], 'fragment'), [])
        self.assertEqual(verify_glsl_links([color], [color], 'vertex'), [color])
        with self.assertRaisesRegex(RuntimeError, 'stage links differ'):
            verify_glsl_links([color], [], 'vertex')
        for emitted in ([{'location': 0, 'type': 'Float3'}],
                        [{'location': 1, 'type': 'Float4'}], [color, color]):
            with self.subTest(emitted=emitted), self.assertRaisesRegex(RuntimeError, 'stage links differ'):
                verify_glsl_links([color], emitted, 'fragment')

    def test_cancel_terminates_and_reaps_compiler_process_group(self):
        import sys
        with self.assertRaisesRegex(ToolingError, 'cancelled'):
            # Supply source metadata expected by the diagnostics mapper; the
            # interpreter process itself sleeps without launching a real tool.
            self.runner.stop()
            build.run_stage([sys.executable, '-c', 'import time; time.sleep(60)', '--source', str(self.graph)], lambda: True)

    def test_source_limits_and_output_root_guard(self):
        source = self.graph.parent / 'main_shaders/color.slang'
        source.write_bytes(b' ' * (build.MAX_FILE + 1))
        with self.assertRaisesRegex(ToolingError, 'byte limit'):
            self.run_build()
        source.write_text('')
        self.output = self.root
        with self.assertRaisesRegex(ToolingError, 'contain source root'):
            self.run_build()

    def test_cli_build_and_verify_use_shared_backend(self):
        from contextlib import redirect_stdout
        import io
        args = ['--json', 'render-graph', 'build', str(self.manifest), str(self.output),
                '--helper', str(self.helper), '--compiler', str(self.compiler)]
        with redirect_stdout(io.StringIO()) as stream:
            self.assertEqual(main(args), 0)
        report = json.loads(stream.getvalue())['build']
        with redirect_stdout(io.StringIO()) as stream:
            self.assertEqual(main(['--json', 'render-graph', 'verify-package', report['package']]), 0)
        self.assertEqual(json.loads(stream.getvalue())['package']['stage'], 'RG1')

    def test_template_never_overwrites_existing_sources(self):
        prior = self.manifest.read_bytes()
        with self.assertRaises(ToolingError):
            build.new_build_sources(self.graph)
        self.assertEqual(prior, self.manifest.read_bytes())


@unittest.skipUnless(os.environ.get('LUDUS_RG1_SLANG'), 'set LUDUS_RG1_SLANG for pinned compiler acceptance')
class CompilerAcceptance(unittest.TestCase):
    def test_real_target_entries_layouts_determinism_and_generated_cpp(self):
        compiler = os.environ['LUDUS_RG1_SLANG']
        profiles = os.environ.get('LUDUS_RG1_TARGETS', 'metal,wgsl').split(',')
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            helper = Path(os.environ['LUDUS_RG1_HELPER']) if os.environ.get('LUDUS_RG1_HELPER') else temp / 'sdk'
            if not os.environ.get('LUDUS_RG1_HELPER'):
                helper.mkdir()
                for name in build.HELPERS:
                    shutil.copy(ROOT / ('config' if name.endswith('.json') else 'cmake/shaders') / name, helper / name)
            root = graph.new_workspace(temp / 'source')
            manifest = build.new_build_sources(root, profiles=profiles)
            options = dict(helper=helper, compiler=compiler, validator=os.environ.get('LUDUS_RG1_VALIDATOR'),
                           spirv_cross=os.environ.get('LUDUS_RG1_CROSS'))
            first = build.build_package(manifest, temp / 'one', **options)
            second = build.build_package(manifest, temp / 'two', **options)
            self.assertEqual(first['generation'], second['generation'])
            warm = build.build_package(manifest, temp / 'one', **options)
            self.assertEqual(warm['cache_misses'], 0)
            # A real compiler error maps through the helper's prefixed report to its source line.
            color = root.parent / 'main_shaders/color.slang'
            broken = color.read_text().replace('material.tint', 'undefinedTint')
            color.write_text(broken)
            with self.assertRaises(ToolingError) as caught:
                build.build_package(manifest, temp / 'broken', **options)
            line = next(i for i, text in enumerate(broken.splitlines(), 1) if 'undefinedTint' in text)
            self.assertEqual({'source': 'main_shaders/color.slang', 'line': line}, caught.exception.location)
            self.assertNotIn(str(temp.resolve()), caught.exception.message)
            self.assertNotIn(str(temp), caught.exception.message)
            color.write_text(broken.replace('undefinedTint', 'material.tint'))
            package = build.verify_package(Path(first['package']))
            self.assertEqual(package['counts']['artifacts'], 3 * len(profiles))
            for program in package['programs']:
                self.assertTrue(program['descriptor']['entry'])
                if program['id'] == 'Color':
                    layout = program['descriptor']['layouts'][0]
                    self.assertEqual(layout['fields'][1]['offset'], 16 if program['profile'] == 'metal' else 12)
            # Compile and execute generated SDK packers with the caller's pinned
            # native compiler when available; reject short outputs without writes.
            if os.environ.get('LUDUS_RG1_CXX'):
                import subprocess
                source = temp / 'pack.cpp'
                labels = [f"Pack_Color_{p}_material" for p in profiles]
                source.write_text('#include "schemas.h"\nusing namespace ludus;\nusing namespace ludus::render_graph_generated;\n'
                                  'int main() { Material m{}; m.tint[0]=1; m.gain=2; foundation::uint8 bytes[32]{};\n' +
                                  '\n'.join(f'if ({label}(m, bytes, 1) || bytes[0] != 0 || !{label}(m, bytes, 32)) return 1;' for label in labels) + '\nreturn 0; }\n')
                subprocess.run([os.environ['LUDUS_RG1_CXX'], '-std=c++23', '-fno-exceptions', '-Wall', '-Wextra', '-Werror',
                                '-I' + str(ROOT / 'modules/foundation/base/include'), '-I' + first['package'],
                                str(source), '-o', str(temp / 'pack')], check=True)
                subprocess.run([str(temp / 'pack')], check=True)


if __name__ == '__main__':
    unittest.main()
