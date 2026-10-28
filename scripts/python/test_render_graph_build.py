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
    def test_guarded_shader_cycles_capture_every_dependency_and_invalidate_consumers(self):
        source = self.manifest.parent / 'main_shaders/color.slang'
        original = source.read_text()
        bridge = source.with_name('bridge.slang')
        bridge.write_text('#pragma once\n#include "color.slang"\n')
        source.write_text('#include "bridge.slang"\n' + original)
        snapshot = build.Snapshot(self.manifest.parent)
        expected = ['main_shaders/bridge.slang', 'main_shaders/color.slang']
        self.assertEqual(expected, snapshot.shader_closure('main_shaders/color.slang'))
        self.assertEqual(expected, snapshot.shader_closure('main_shaders/bridge.slang'))
        self.run_build()
        self.calls.clear()
        bridge.write_text(bridge.read_text() + '// edited helper\n')
        self.run_build()
        self.assertEqual(['fragment', 'fragment'], [stage for _, stage, _ in self.calls])

    def test_unguarded_cycle_reports_the_include_chain_and_preserves_the_package(self):
        self.run_build()
        prior = (self.output / 'current.json').read_bytes()
        source = self.manifest.parent / 'main_shaders/color.slang'
        source.write_text('#include "bridge.slang"\n')
        source.with_name('bridge.slang').write_text('#include "color.slang"\n')
        with self.assertRaisesRegex(ToolingError, 'color.slang -> main_shaders/bridge.slang -> main_shaders/color.slang'):
            self.run_build()
        self.assertEqual(prior, (self.output / 'current.json').read_bytes())

    def test_conditional_pragma_does_not_hide_an_unguarded_cycle(self):
        source = self.manifest.parent / 'main_shaders/color.slang'
        source.write_text('#if 0\n#pragma once\n#endif\n#include "color.slang"\n')
        with self.assertRaisesRegex(ToolingError, 'unguarded shader include cycle'):
            build.Snapshot(self.manifest.parent).shader_closure('main_shaders/color.slang')

    def test_acyclic_shader_depth_stays_bounded(self):
        for index in range(build.MAX_SHADER_INCLUDE_DEPTH + 1):
            source = self.manifest.parent / f'level{index}.slang'
            source.write_text(f'#include "level{index + 1}.slang"\n' if index < build.MAX_SHADER_INCLUDE_DEPTH else '')
        with self.assertRaisesRegex(ToolingError, 'shader include depth exceeds 16: level0.slang ->'):
            build.Snapshot(self.manifest.parent).shader_closure('level0.slang')

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

    def test_offline_build_needs_no_native_client_and_ignores_unused_client_changes(self):
        self.assertFalse((self.helper / build.NATIVE_WORKER_HELPER).exists())
        first = self.run_build()
        client = self.helper / build.NATIVE_WORKER_HELPER
        client.write_text('unused native client')
        second = self.run_build()
        client.write_text('changed unused native client')
        third = self.run_build()
        self.assertEqual(first['generation'], second['generation'])
        self.assertEqual(first['generation'], third['generation'])
        self.assertEqual(0, second['cache_misses'])
        self.assertEqual(0, third['cache_misses'])

    def test_explicit_or_environment_worker_requires_its_sdk_client_and_preserves_publication(self):
        self.run_build()
        pointer = self.pointer()
        for uses_environment in (False, True):
            with self.subTest(environment=uses_environment):
                environment = {'LUDUS_SHADER_WORKER_SOCKET': '/unused/compiler.sock'} if uses_environment else {}
                options = {} if uses_environment else {'worker_socket': '/unused/compiler.sock'}
                with patch.dict(os.environ, environment):
                    with self.assertRaisesRegex(ToolingError, 'native_worker.py') as caught:
                        self.run_build(**options)
                self.assertEqual('RenderGraphBuildTools', caught.exception.code)
                self.assertEqual(pointer, self.pointer())

    def test_selected_upscaler_preserves_the_published_package_and_compiler_cache(self):
        from ludus_tools import render_graph_upscale as upscale
        self.run_build()
        pointer = self.pointer()
        value = json.loads(self.graph.read_text())
        value['version'] = 2
        configuration = value['configurations'][0]
        configuration['nodes'].append(upscale.new_node('fsr-1', 'upscale'))
        output = next(edge for edge in configuration['edges'] if edge['to'] == 'output.color')
        configuration['edges'].append({'from': output['from'], 'to': 'upscale.color'})
        configuration['edges'].append({'from': 'color.texture', 'to': 'upscale.destination'})
        output['from'] = 'upscale.color'
        self.graph.write_text(json.dumps(value))
        self.calls.clear()
        with self.assertRaisesRegex(ToolingError, 'backend is unavailable'):
            self.run_build()
        self.assertEqual(pointer, self.pointer())
        self.assertEqual([], self.calls)

    def test_native_client_is_hashed_when_worker_is_selected(self):
        client = self.helper / build.NATIVE_WORKER_HELPER
        client.write_text('native client')
        first, _ = build.tool_identity(self.helper, self.compiler, None, None, worker_socket='/unused/compiler.sock')
        self.assertIn('generator/render_graph_diagnostics.py', first)
        self.assertIn('generator/render_graph_upscale.py', first)
        client.write_text('changed native client')
        second, _ = build.tool_identity(self.helper, self.compiler, None, None, worker_socket='/unused/compiler.sock')
        self.assertNotEqual(first['helper/' + build.NATIVE_WORKER_HELPER],
                            second['helper/' + build.NATIVE_WORKER_HELPER])

    def test_tool_hashes_reuse_content_only_within_an_unchanged_file_identity(self):
        hashes = build.ToolFileHashes()
        first = hashes(self.compiler)
        alias = self.root / 'compiler-alias'
        alias.symlink_to(self.compiler)
        self.assertEqual(first, hashes(alias))
        self.assertEqual(1, len(hashes.values))
        stamp = self.compiler.stat()
        self.compiler.write_bytes(b'x' * stamp.st_size)
        os.utime(self.compiler, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertNotEqual(first, hashes(self.compiler))
        replacement = self.root / 'replacement'
        replacement.write_bytes(b'y' * stamp.st_size)
        os.replace(replacement, self.compiler)
        self.assertEqual(build.file_hash(self.compiler), hashes(alias))
        alias.unlink()
        alias.symlink_to(self.helper / build.HELPERS[0])
        self.assertEqual(build.file_hash(alias), hashes(alias))
        self.compiler.unlink()
        with self.assertRaises(OSError):
            hashes(self.compiler)

    def test_late_tool_edit_rejects_publication_and_discards_new_cache_entries(self):
        self.run_build()
        prior = self.pointer()
        previous_cache = {p.name for p in (self.output / 'cache').iterdir()}
        source = self.graph.parent / 'main_shaders/color.slang'
        source.write_text('// force a new stage\n')
        original_atomic = build.atomic
        def publish(path, data):
            original_atomic(path, data)
            if path.name == 'build-report.json':
                stamp = self.compiler.stat()
                self.compiler.write_bytes(b'x' * stamp.st_size)
                os.utime(self.compiler, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        with patch.object(build, 'atomic', side_effect=publish):
            with self.assertRaisesRegex(ToolingError, 'changed before publication'):
                self.run_build()
        self.assertEqual(prior, self.pointer())
        self.assertEqual(previous_cache, {p.name for p in (self.output / 'cache').iterdir()})

    def test_unstable_tool_hash_also_removes_new_cache_entries(self):
        cached = self.root / 'new-cache-entry'
        cached.mkdir()
        def unstable_hash(path):
            raise ToolingError('RenderGraphBuildStale', 'tool changed while hashing')
        with build.cache_writes({'compiler': self.compiler}, {'compiler': 'original'}, unstable_hash) as written:
            written.append(cached)
        self.assertFalse(cached.exists())

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

    def test_unreferenced_imported_configurations_do_not_spend_the_configuration_budget(self):
        rasterize = self.graph.with_name('rasterize.lurender.json')
        value = json.loads(rasterize.read_text())
        template = value['configurations'][0]
        while len(value['configurations']) < graph.MAX_CONFIGURATIONS:
            value['configurations'].append({**template, 'id': f'Unused{len(value["configurations"])}'})
        rasterize.write_text(json.dumps(value))
        package = build.verify_package(Path(self.run_build()['package']))
        self.assertEqual(2, package['counts']['graph_configurations'])

    def test_reachable_imported_configurations_do_not_spend_the_configuration_budget(self):
        value = json.loads(self.graph.read_text())
        value['configurations'].append({**value['configurations'][0], 'id': 'Second'})
        self.graph.write_text(json.dumps(value))
        self.edit(lambda v: v['budgets'].update(configurations=2))
        package = build.verify_package(Path(self.run_build()['package']))
        self.assertEqual(2, package['counts']['root_configurations'])
        self.assertEqual(3, package['counts']['graph_configurations'])

    def test_root_configurations_spend_the_configuration_budget(self):
        value = json.loads(self.graph.read_text())
        value['configurations'].append({**value['configurations'][0], 'id': 'Second'})
        self.graph.write_text(json.dumps(value))
        self.edit(lambda v: v['budgets'].update(configurations=1))
        with self.assertRaises(ToolingError) as caught:
            self.run_build()
        self.assertEqual('RenderGraphBuildCapacity', caught.exception.code)
        self.assertFalse(self.calls)

    def test_no_missing_or_unused_stage_programs(self):
        for change in (lambda v: v['techniques'][0].update(fragment='Missing'),
                       lambda v: v['techniques'][0].update(vertex='Color'),
                       lambda v: v['techniques'][0].update(configuration='Implicit'),
                       lambda v: v['programs'][0].update(output='Color')):
            value = self.value(); change(value)
            with self.assertRaises(ToolingError):
                build.validate_manifest(value)

    def test_registered_draft_callbacks_do_not_require_techniques_or_compile(self):
        program = {'id': 'DraftMiss', 'stage': 'miss', 'source': 'draft.slang',
                   'function': 'ShaderMain', 'input': 'Ray', 'output': 'Color', 'bindings': []}
        (self.graph.parent / 'draft.slang').write_text('// unfinished authoring draft')
        self.edit(lambda value: value['programs'].append(program))
        build.validate_manifest(self.value())
        result = self.run_build()
        self.assertEqual(result['cache_misses'], 6)
        self.assertEqual(len(self.calls), 6)
        self.assertTrue(all('unfinished authoring draft' not in code for _, _, code in self.calls))

    def test_incomplete_pipeline_reports_missing_stages_instead_of_unused_programs(self):
        fixture = ROOT / 'tests/sdk_consumer/rg2/raytracing/main.lurender.json'
        value = json.loads(fixture.read_text())
        node = next(node for node in value['configurations'][0]['nodes'] if node['kind'] == 'RayTracingPipeline')
        node['shaders'] = [shader for shader in node['shaders'] if shader['stage'] != 'miss']
        self.graph.write_text(json.dumps(value))
        for shader in node['shaders']:
            program = {'id': shader['program'], 'stage': shader['stage'], 'source': shader['program'] + '.slang',
                       'function': 'ShaderMain', 'bindings': [],
                       'input': 'RayLaunch' if shader['stage'] == 'ray_generation' else 'RayHit',
                       'output': 'Ray' if shader['stage'] == 'ray_generation' else 'Color'}
            (self.graph.parent / program['source']).write_text('// unfinished callback')
            self.edit(lambda value: value['programs'].append(program))
        with self.assertRaises(ToolingError) as failure:
            self.run_build()
        self.assertEqual('RenderGraphIncompletePipeline', failure.exception.code)
        self.assertIn('miss', str(failure.exception))
        self.assertFalse(self.calls)

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
        with self.assertRaisesRegex(ToolingError, 'undeclared technique') as caught:
            self.run_build()
        self.assertIn('DefaultStaticOpaque.ColorAndDepth', str(caught.exception))
        self.assertIn('main.lushader.json', str(caught.exception))
        self.assertIn('rasterize.lurender.json', str(caught.exception))
        self.assertFalse(self.calls)

    def test_duplicate_nonfinite_and_unknown_json_fields_fail(self):
        for raw in ('{"version":1,"version":1}', '{"version":NaN}', '{broken'):
            self.manifest.write_text(raw)
            with self.assertRaises(ToolingError):
                self.run_build()

    def test_software_ray_metal_rejects_native_resources_and_preserves_binding_gaps(self):
        import sys
        sys.path.insert(0, str(ROOT / 'cmake/shaders'))
        from compile_graph_stage import verify_ray_compute_metal
        interface = {'entries': [{'binding': 0, 'kind': 'UniformBuffer'},
                                  {'binding': 6, 'kind': 'StorageReadWrite'}]}
        code = 'kernel void Main(constant float4* parameters [[buffer(0)]], device uint4* values [[buffer(6)]]) {}'
        self.assertEqual('Main', verify_ray_compute_metal(code, interface, False))
        with self.assertRaisesRegex(RuntimeError, 'binding differs'):
            verify_ray_compute_metal(code.replace('buffer(6)', 'buffer(5)'), interface, False)
        with self.assertRaisesRegex(RuntimeError, 'must not require native'):
            verify_ray_compute_metal('#include <metal_raytracing>\n' + code, interface, False)
        interface['entries'].append({'binding': 5, 'kind': 'RayScene'})
        with self.assertRaisesRegex(RuntimeError, 'must not require native'):
            verify_ray_compute_metal(code, interface, False)

    def test_forward_metal_arrays_are_opt_in_and_independently_sized(self):
        import sys
        sys.path.insert(0, str(ROOT / 'cmake/shaders'))
        from compile_graph_stage import metal_layouts
        interface = {'entries': [{'name': 'material', 'size': 48, 'offsets': {'Tint': 0, 'Lights': 16}}]}
        raw = {'parameters': [{'name': 'material', 'type': {'elementType': {'name': 'Material'}}}]}
        code = 'struct Material_0 { float4 Tint_0; array<float4, int(2)> Lights_0; };'
        with self.assertRaisesRegex(RuntimeError, 'scalar/vector'):
            metal_layouts(code, interface, raw)
        metal_layouts(code, interface, raw, allows_arrays=True)
        with self.assertRaisesRegex(RuntimeError, 'packing differs'):
            metal_layouts(code.replace('int(2)', 'int(3)'), interface, raw, allows_arrays=True)

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


class InstalledToolsTests(unittest.TestCase):
    """The SDK installs a hand-listed subset of ludus_tools beside cmake/render_graph/build.py."""

    def test_installed_modules_import_without_the_source_tree(self):
        # A module-level import of an uninstalled module (render_graph_layout) only failed in the
        # batch's installed-SDK consumers; integration PRs do not run them.
        import re
        import subprocess
        import sys
        listing = re.search(r'install\(FILES (scripts/python/ludus_tools/__init__\.py.*?)\s+DESTINATION '
                            r'"\$\{CMAKE_INSTALL_LIBDIR\}/cmake/Ludus/render_graph/ludus_tools"\)',
                            (ROOT / 'CMakeLists.txt').read_text(), re.DOTALL)
        self.assertIsNotNone(listing, 'CMakeLists.txt no longer installs ludus_tools for render graphs')
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'ludus_tools'
            package.mkdir()
            for name in listing.group(1).split():
                shutil.copy(ROOT / name, package)
            # build.py's own imports, resolved against only the installed copy (-E -s also drop
            # PYTHONPATH and user site-packages, which may hold the full ludus_tools).
            probe = ('import sys; sys.path.insert(0, sys.argv[1]); '
                     'from ludus_tools import render_graph_build, render_graph_execution; '
                     'assert render_graph_build.__file__.startswith(sys.argv[1]), render_graph_build.__file__')
            result = subprocess.run([sys.executable, '-E', '-s', '-c', probe, temporary],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(0, result.returncode, result.stderr)


if __name__ == '__main__':
    unittest.main()
