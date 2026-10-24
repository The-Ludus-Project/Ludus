"""RG3 mapped diagnostics, draft equivalence, migration and runtime generation regressions."""
import contextlib
import io
import json
from pathlib import Path
import shutil
import struct
import unittest
from unittest.mock import patch

import test_render_graph_build as fixtures
import test_render_graph_execution as execution_fixtures
from ludus_tools import render_graph as graph
from ludus_tools import render_graph_authoring as authoring
from ludus_tools import render_graph_build as build
from ludus_tools import render_graph_execution as execution
from ludus_tools.cli import main
from ludus_tools.errors import ToolingError


def run_cli(*arguments):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        status = main(['--json', *arguments])
    return status, json.loads(output.getvalue())


class MappedDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.temp = fixtures.tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.graph = graph.new_workspace(self.root / 'sources')

    def edit(self, name, function):
        path = self.root / 'sources' / name
        value = json.loads(path.read_text())
        function(value)
        path.write_text(json.dumps(value))

    def test_nested_library_error_maps_to_authored_source_configuration_and_node(self):
        self.edit('rasterize.lurender.json', lambda v: v['configurations'][0]['edges'].append(
            {'from': 'input.scene', 'to': 'draw.missing'}))
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.graph)
        self.assertEqual('RenderGraphPort', caught.exception.code)
        self.assertEqual({'source': 'rasterize.lurender.json', 'configuration': 'ColorAndDepth',
                          'node': 'draw', 'port': 'missing', 'direction': 'input'}, caught.exception.location)

    def test_expanded_instance_error_maps_to_its_owner_and_nested_path(self):
        self.edit('rasterize.lurender.json', lambda v: v['configurations'][0]['nodes'][0].update(depth_load='Load'))
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.graph)
        self.assertEqual('RenderGraphInitialization', caught.exception.code)
        self.assertEqual({'source': 'rasterize.lurender.json', 'configuration': 'ColorAndDepth',
                          'node': 'draw', 'instance': 'Main/raster/draw', 'port': 'depth',
                          'direction': 'input', 'field': 'depth_load'}, caught.exception.location)

    def test_document_field_error_maps_to_its_configuration_before_import(self):
        def initialized(value):
            for node in value['configurations'][0]['nodes']:
                if node['id'] == 'depth':
                    node['initialized'] = True
        self.edit('main.lurender.json', initialized)
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.graph)
        self.assertEqual({'source': 'main.lurender.json', 'configuration': 'Default'}, caught.exception.location)

    def test_camera_route_failure_selects_the_camera_node(self):
        def clear_twice(value):
            config = value['configurations'][0]
            config['nodes'] = [n for n in config['nodes'] if n['id'] != 'output']
            config['edges'] = [e for e in config['edges'] if not e['to'].startswith('output.')]
        self.edit('main.lurender.json', clear_twice)
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.graph)
        self.assertEqual('RenderGraphCameraRouteMissing', caught.exception.code)
        self.assertEqual({'source': 'main.lurender.json', 'configuration': 'Default',
                          'instance': 'Main/camera', 'node': 'camera'}, caught.exception.location)

    def test_cli_json_failure_carries_the_same_location(self):
        self.edit('rasterize.lurender.json', lambda v: v['configurations'][0]['edges'].append(
            {'from': 'input.scene', 'to': 'draw.missing'}))
        status, payload = run_cli('render-graph', 'validate', str(self.graph))
        self.assertEqual(1, status)
        with self.assertRaises(ToolingError) as caught:
            graph.compile_graph(self.graph)
        self.assertEqual(caught.exception.location, payload['error']['location'])
        self.assertEqual(caught.exception.message, payload['error']['message'])
        self.assertNotIn(str(self.root), json.dumps(payload['error']['location']))

    def test_draft_snapshot_in_another_directory_has_an_identical_plan(self):
        saved = graph.compile_graph(self.graph)
        draft = self.root / 'drafts' / '7'
        shutil.copytree(self.root / 'sources', draft)
        status, payload = run_cli('render-graph', 'dump', str(draft / 'main.lurender.json'), '--root', str(draft))
        self.assertEqual(0, status)
        self.assertEqual(saved, payload['plan'])

    def test_draft_layout_only_edit_keeps_semantic_digest(self):
        saved = graph.compile_graph(self.graph)
        draft = self.root / 'draft'
        shutil.copytree(self.root / 'sources', draft)
        path = draft / 'main.lurender.json'
        value = json.loads(path.read_text())
        value['configurations'][0]['layout'] = {'positions': [{'node': 'camera', 'x': 40, 'y': -12}]}
        path.write_bytes(graph.canonical_asset(value))
        moved = graph.compile_graph(path)
        self.assertEqual(saved['semantic_digest'], moved['semantic_digest'])
        self.assertEqual(saved['passes'], moved['passes'])
        self.assertNotEqual(saved['dependencies'], moved['dependencies'])


class ShaderDiagnosticTests(unittest.TestCase):
    def test_compiler_lines_map_to_captured_sources_and_generated_owner(self):
        output = ('+ slangc ...\nmain_shaders/color.slang(3): error 30015: undefined identifier \'x\'.\n'
                  'wrapper.slang(12,5): error 39999: no overload\nother.slang(1): warning 1: ignored\n')
        program = {'id': 'Color'}
        diagnostics = build.map_shader_diagnostics(output, program, 'metal', ['main_shaders/color.slang'])
        self.assertEqual({'source': 'main_shaders/color.slang', 'line': 3}, diagnostics[0]['location'])
        self.assertEqual({'generated': 'wrapper.slang', 'program': 'Color', 'line': 12, 'column': 5},
                         diagnostics[1]['location'])
        self.assertNotIn('location', diagnostics[2])
        self.assertEqual(['error', 'error', 'warning'], [d['severity'] for d in diagnostics])

    def test_caret_maps_to_scalar_column_and_length(self):
        # Echo and caret as printed by the pinned slangc: tabs expanded to four-column
        # stops, carets placed by UTF-8 byte column.
        source = ('void main()\n{\n\tfloat v = missing + 1.0;\n/*é*/\tfloat z = bad;\r\n'
                  '    int q = "str";\n    return\n}\n').encode('utf-8')
        output = ('Ludus shader build: a.slang(3): error 30015: undefined identifier \'missing\'.\n'
                  '    float v = missing + 1.0;\n'
                  '              ^~~~~~~\n'
                  'a.slang(4): error 30015: undefined identifier \'bad\'.\n'
                  '/*é*/  float z = bad;\n'
                  '                  ^~~\n'
                  'a.slang(5): error 30019: expected an expression of type \'int\', got \'String\'\n'
                  '    int q = "str";\n'
                  '                 ^\n'
                  'a.slang(6): error 20001: unexpected end of line, expected \';\'\n'
                  '    return\n'
                  '          ^\n')
        diagnostics = build.map_shader_diagnostics(output, {'id': 'P'}, 'metal', ['a.slang'], {'a.slang': source})
        self.assertEqual([{'source': 'a.slang', 'line': 3, 'column': 12, 'length': 7},
                          {'source': 'a.slang', 'line': 4, 'column': 17, 'length': 3},
                          {'source': 'a.slang', 'line': 5, 'column': 18, 'length': 1},
                          {'source': 'a.slang', 'line': 6, 'column': 11, 'length': 0}],
                         [d['location'] for d in diagnostics])

    def test_unverifiable_caret_keeps_line_only(self):
        source = ('x' * 300 + ' + bad;\n').encode('utf-8')
        truncated = ('a.slang(1): error 30015: undefined identifier \'bad\'.\n' + 'x' * 200 + '\n' + ' ' * 200 + '\n')
        edited = 'a.slang(1): error 30015: undefined identifier \'bad\'.\n    y + bad;\n        ^~~\n'
        for output in (truncated, edited):
            diagnostics = build.map_shader_diagnostics(output, {'id': 'P'}, 'metal', ['a.slang'], {'a.slang': source})
            self.assertEqual({'source': 'a.slang', 'line': 1}, diagnostics[0]['location'])

    def test_bounded_diagnostics(self):
        output = ''.join(f'a.slang({i}): error 1: x\n' for i in range(100))
        self.assertEqual(build.MAX_SHADER_DIAGNOSTICS,
                         len(build.map_shader_diagnostics(output, {'id': 'P'}, 'wgsl', ['a.slang'])))


class BuildFixture(unittest.TestCase):
    """Reuse the RG1 controlled-compiler fixture without re-running its tests."""
    setUp = fixtures.BuildTests.setUp
    stage = fixtures.BuildTests.stage
    run_build = fixtures.BuildTests.run_build
    value = fixtures.BuildTests.value
    edit = fixtures.BuildTests.edit
    pointer = fixtures.BuildTests.pointer


class ShaderFailureTests(BuildFixture):
    def test_build_shader_failure_reports_mapped_function_line_and_keeps_package(self):
        self.run_build()
        prior = self.pointer()
        self.compiler.write_text('another tool')

        def failing(command, cancel):
            build.fail('Shader', 'shader helper', 'wrapper.slang(9): error 30015: undefined identifier')
        with patch.object(build, 'run_stage', failing):
            with self.assertRaises(ToolingError) as caught:
                self.run_build()
        self.assertEqual('RenderGraphBuildShader', caught.exception.code)
        self.assertEqual({'generated': 'wrapper.slang', 'program': 'Color', 'line': 9}, caught.exception.location)
        self.assertEqual(1, len(caught.exception.diagnostics))
        self.assertEqual(prior, self.pointer())

    def test_located_shader_error_names_the_authored_function_file(self):
        source = self.value()['programs'][1]['source']

        def failing(command, cancel):
            build.fail('Shader', 'shader helper', source + '(2): error 30015: undefined identifier')
        with patch.object(build, 'run_stage', failing):
            with self.assertRaises(ToolingError) as caught:
                self.run_build()
        self.assertEqual({'source': source, 'line': 2}, caught.exception.location)

    def fail_with_real_helper(self, *extra):
        """Build through the real run_stage/compile_shader.py path with a stand-in slangc.

        The stand-in reports the pinned version, then prints a Slang diagnostic for the
        captured absolute source path, any extra lines (formatted with `source`, `output`
        and `tool`) and fails. Return the raised ToolingError.
        """
        import stat
        import sys
        self.runner.stop()
        version = json.loads((self.helper / 'shader_toolchain.json').read_text())['slang']['version']
        self.compiler.write_text(
            f'#!{sys.executable}\nimport sys\nfrom pathlib import Path\n'
            f'if sys.argv[1:] == ["-version"]:\n    print({version!r})\n    sys.exit(0)\n'
            'source = Path(next(a for a in sys.argv if a.endswith("wrapper.slang"))).parent\n'
            'output = Path(sys.argv[sys.argv.index("-o") + 1]).parent\n'
            'tool = sys.argv[0]\n'
            'print(f"{source}/main_shaders/color.slang(3): error 30015: undefined identifier \'x\'.")\n'
            'print("    return float4(x, 1);")\n' +
            ''.join(f'print(f{line!r})\n' for line in extra) + 'sys.exit(1)\n')
        self.compiler.chmod(self.compiler.stat().st_mode | stat.S_IXUSR)
        with self.assertRaises(ToolingError) as caught:
            self.run_build()
        self.assertEqual('RenderGraphBuildShader', caught.exception.code)
        return caught.exception

    def test_real_helper_failure_maps_its_prefixed_compiler_line(self):
        # The helper reports compiler failure on stderr as `Ludus shader build: <compiler
        # output>`, so the first diagnostic line carries that prefix.
        error = self.fail_with_real_helper()
        self.assertTrue(error.message.startswith(
            'shader helper: ' + build.SHADER_HELPER_PREFIX + 'main_shaders/color.slang(3): error 30015'), error.message)
        self.assertEqual({'source': 'main_shaders/color.slang', 'line': 3}, error.location)
        self.assertEqual("undefined identifier 'x'.", error.diagnostics[0]['message'])

    def test_real_helper_failure_message_has_no_machine_paths(self):
        # The helper echoes `+ <absolute slangc> ... -o <temporary artifact path>`; compiler
        # messages may name the tool, the stage output and helper files.
        error = self.fail_with_real_helper('{tool}: cannot write {output}/artifact.metal',
                                           'note: see ' + str(self.helper.resolve()) + '/compile_graph_stage.py')
        self.assertNotIn(str(self.root), error.message)
        self.assertNotIn(str(self.root.resolve()), error.message)
        self.assertFalse(any(line.startswith('+ ') for line in error.message.splitlines()), error.message)
        self.assertIn('\nslangc: cannot write artifact/artifact.metal\n', error.message)
        self.assertIn('\nnote: see helper/compile_graph_stage.py', error.message)
        self.assertEqual({'source': 'main_shaders/color.slang', 'line': 3}, error.location)


class MigrationTests(BuildFixture):
    def stage(self, command, cancel):
        """Reflect the current Material schema instead of the RG1 fixture's fixed layout."""
        fixtures.BuildTests.stage(self, command, cancel)
        output = Path(command[command.index('--output') + 1])
        target = command[command.index('--graph-target') + 1]
        descriptor = json.loads((output / 'stage.json').read_text())
        material = next(s for s in self.value()['schemas'] if s['id'] == 'Material')
        for entry in descriptor['interface']['entries']:
            offset, offsets, shapes = 0, {}, {}
            for field in material['fields']:
                width = build.SHAPES[field['type']][1]
                align = 16 if width >= 3 and target == 'metal' else (8 if width == 2 else 4)
                offset = -(-offset // align) * align
                offsets[field['id']], shapes[field['id']] = offset, field['type']
                offset += 4 * (4 if width == 3 and target == 'metal' else width)
            entry.update(offsets=offsets, shapes=shapes, size=-(-offset // 16) * 16)
        (output / 'stage.json').write_bytes(build.encoded(descriptor))

    def package(self):
        report = self.run_build()
        return Path(report['package'])

    def test_identical_rebuild_is_compatible(self):
        old = self.package()
        self.assertEqual('Compatible', authoring.compare_packages(old, old)['cpu_interface'])

    def test_added_uniform_field_requires_restart_and_explicit_default(self):
        old = self.package()
        self.edit(lambda v: v['schemas'][2]['fields'].append({'id': 'bias', 'type': 'Float32'}))
        new = self.package()
        comparison = authoring.compare_packages(old, new)
        self.assertEqual('RestartRequired', comparison['cpu_interface'])
        material = next(s for s in comparison['schemas'] if s['id'] == 'Material')
        self.assertEqual('Changed', material['status'])
        values = {'Material': {'tint': [1, 0.5, 0.25], 'gain': [2]}}
        rejected = authoring.migrate_values(old, new, values)
        self.assertEqual('Rejected', rejected['status'])
        self.assertNotIn('values', rejected)
        self.assertIn({'schema': 'Material', 'field': 'bias', 'action': 'Rejected',
                       'reason': 'field added; declare an explicit default'}, rejected['diff'])
        migrated = authoring.migrate_values(old, new, values, {'Material': {'bias': [0]}})
        self.assertEqual('Migrated', migrated['status'])
        self.assertEqual({'tint': [1.0, 0.5, 0.25], 'gain': [2.0], 'bias': [0]}, migrated['values']['Material'])

    def test_changed_type_never_reinterprets_and_removed_value_is_reported(self):
        old = self.package()

        def change(value):
            fields = value['schemas'][2]['fields']
            fields[1]['type'] = 'Float2'
            fields.pop(0)
            fields.append({'id': 'tint4', 'type': 'Float4'})
        self.edit(change)
        new = self.package()
        values = {'Material': {'tint': [1, 1, 1], 'gain': [3]}}
        result = authoring.migrate_values(old, new, values, {'Material': {'tint4': [1, 1, 1, 1]}})
        self.assertEqual('Rejected', result['status'])
        actions = {d['field']: d['action'] for d in result['diff']}
        self.assertEqual({'tint': 'Dropped', 'gain': 'Rejected', 'tint4': 'Defaulted'}, actions)

    def test_unknown_or_nonfinite_values_reject(self):
        old = self.package()
        for values in ({'Material': {'other': [1]}}, {'Unknown': {}}, {'Material': {'gain': [float('inf')]}},
                       {'Material': {'gain': [True]}}, {'Material': {'tint': [1, 2]}}):
            with self.assertRaises(ToolingError):
                authoring.migrate_values(old, old, values)

    def test_cli_migration_reports_rejection_without_writing(self):
        old = self.package()
        self.edit(lambda v: v['schemas'][2]['fields'].append({'id': 'bias', 'type': 'Float32'}))
        new = self.package()
        values = self.root / 'values.json'
        values.write_text(json.dumps({'Material': {'gain': [1]}}))
        before = values.read_bytes()
        status, payload = run_cli('render-graph', 'migrate', str(old), str(new), '--values', str(values))
        self.assertEqual(1, status)
        self.assertEqual('Rejected', payload['migration']['status'])
        self.assertEqual(before, values.read_bytes())
        status, payload = run_cli('render-graph', 'schema-diff', str(old), str(new))
        self.assertEqual(0, status)
        self.assertEqual('RestartRequired', payload['schema_diff']['cpu_interface'])


def parse_runtime(data):
    """Independent little-endian reader for the runtime_package.h layout."""
    offset = 0

    def take(count):
        nonlocal offset
        values = struct.unpack_from('<' + 'I' * count, data, offset)
        offset += 4 * count
        return values
    magic, version, resources, passes, cameras, values, bindings, blocks, fields, stages = take(10)
    interface, generation = data[offset:offset + 32].hex(), data[offset + 32:offset + 64].hex()
    offset += 64
    result = {'magic': magic, 'version': version, 'interface': interface, 'generation': generation,
              'resources': [take(9 if version >= 11 else 8 if version >= 4 else 6) for _ in range(resources)], 'cameras': [take(1) for _ in range(cameras)],
              'passes': [take(19 if version >= 16 else 18 if version >= 15 else 17 if version >= 14 else 16 if version >= 13 else 15 if version >= 10 else 13 if version >= 9 else 10 if version >= 5 else 9) for _ in range(passes)], 'uniform_values': values,
              'bindings': [take(5) for _ in range(bindings)], 'blocks': [take(3) for _ in range(blocks)],
              'fields': [take(5) for _ in range(fields)], 'stages': []}
    for _ in range(stages):
        role, target, inputs, entry_bytes, artifact_bytes, block_count = take(6)
        stage_inputs = [take(2) for _ in range(inputs)]
        names = []
        for _ in range(block_count):
            slot, name_bytes = take(2)
            names.append((slot, data[offset:offset + name_bytes].decode()))
            offset += name_bytes + (-name_bytes % 4)
        entry = data[offset:offset + entry_bytes].decode()
        offset += entry_bytes + (-entry_bytes % 4)
        artifact = data[offset:offset + artifact_bytes]
        offset += artifact_bytes + (-artifact_bytes % 4)
        result['stages'].append({'role': role, 'target': target, 'inputs': stage_inputs, 'entry': entry,
                                 'artifact': artifact, 'blocks': names})
    result['storage_buffers'] = [take(4) for _ in range(take(1)[0])] if version >= 8 else []
    result['pass_names'] = []
    if version >= 3:
        for _ in range(passes):
            (name_bytes,) = take(1)
            assert 0 < name_bytes <= 127
            name = data[offset:offset + name_bytes].decode('utf-8')
            assert '\0' not in name
            result['pass_names'].append(name)
            offset += name_bytes + (-name_bytes % 4)
    assert offset == len(data)
    return result


class RuntimePackageTests(BuildFixture):
    setUp = execution_fixtures.ExecutionTests.setUp

    def stage(self, command, cancel):
        """Reflect the current static vertex schema names, including renamed fields."""
        execution_fixtures.ExecutionTests.stage(self, command, cancel)
        output = Path(command[command.index('--output') + 1])
        descriptor = json.loads((output / 'stage.json').read_text())
        if descriptor['stage'] == 'vertex':
            fields = self.value()['schemas'][0]['fields']
            descriptor['interface']['inputs'] = [{'location': f['location'], 'format': f['type'], 'name': f['id']} for f in fields]
        (output / 'stage.json').write_bytes(build.encoded(descriptor))

    def test_runtime_package_matches_header_plan_and_identity(self):
        package = Path(self.run_build()['package'])
        header = execution.emit_runtime(package, self.root / 'Graph.h', name='Graph')
        cooked = execution.cook_runtime(package, self.root / 'graph.lurgrt')
        data = (self.root / 'graph.lurgrt').read_bytes()
        parsed = parse_runtime(data)
        self.assertEqual((execution.PACKAGE_MAGIC, execution.PACKAGE_VERSION), (parsed['magic'], parsed['version']))
        self.assertEqual(['Main/raster/draw'], parsed['pass_names'])
        self.assertEqual(header['interface_identity'], parsed['interface'])
        self.assertEqual(cooked['generation'], parsed['generation'])
        self.assertEqual(package.name, parsed['generation'])
        self.assertEqual(header['resources'], len(parsed['resources']))
        self.assertEqual(header['passes'], len(parsed['passes']))
        text = (self.root / 'Graph.h').read_text()
        identity = bytes.fromhex(parsed['interface'])
        self.assertIn(', '.join(hex(b) for b in identity[:16]), text)
        roles = sorted({(s['role'], s['target']) for s in parsed['stages']})
        self.assertEqual(len(roles), len(parsed['stages']))
        for stage in parsed['stages']:
            if stage['role'] % 2 == 0:
                self.assertEqual([(i, 2 if i == 0 else 3) for i in range(6)], stage['inputs'])
            else:
                self.assertEqual([], stage['inputs'])
            self.assertEqual('rgEntry', stage['entry'])

    def test_bound_runtime_package_carries_the_header_uniform_tables(self):
        self.edit(lambda value: execution_fixtures.ExecutionTests.bind_material(self, value))
        package = Path(self.run_build()['package'])
        execution.emit_runtime(package, self.root / 'Bound.h', name='Bound')
        execution.cook_runtime(package, self.root / 'bound.lurgrt')
        parsed = parse_runtime((self.root / 'bound.lurgrt').read_bytes())
        text = (self.root / 'Bound.h').read_text()
        self.assertEqual(4, parsed['uniform_values'])
        self.assertIn('UNIFORM_VALUE_COUNT = 4;', text)
        programs = {code: name for name, code in execution.PROGRAM_CODES.items()}
        visibility = {code: name for name, code in execution.VISIBILITY.items()}
        targets = {code: execution.TARGET_NAMES[name] for name, code in execution.TARGET_CODES.items()}
        for program, slot, stages, offset, count in parsed['bindings']:
            self.assertIn(f'{{format::Technique::{programs[program]}, {slot}, format::Visibility::{visibility[stages]}, '
                          f'{offset}, {count}}},', text)
        for binding, target, size in parsed['blocks']:
            self.assertIn(f'{{{binding}, format::ShaderTarget::{targets[target]}, {size}}},', text)
        for binding, target, offset, components, byte_offset in parsed['fields']:
            self.assertIn(f'{{{binding}, format::ShaderTarget::{targets[target]}, {offset}, {components}, {byte_offset}}},',
                          text)
        self.assertEqual(len(parsed['fields']), 4)
        self.assertTrue(all(stage['blocks'] == [] for stage in parsed['stages']))

    def test_runtime_package_is_deterministic_and_preserves_destination_on_failure(self):
        package = Path(self.run_build()['package'])
        execution.cook_runtime(package, self.root / 'a.lurgrt')
        execution.cook_runtime(package, self.root / 'b.lurgrt')
        self.assertEqual((self.root / 'a.lurgrt').read_bytes(), (self.root / 'b.lurgrt').read_bytes())
        prior = (self.root / 'a.lurgrt').read_bytes()
        with self.assertRaises(ToolingError):
            execution.cook_runtime(package, self.root / 'a.lurgrt', configuration='Missing')
        self.assertEqual(prior, (self.root / 'a.lurgrt').read_bytes())

    def test_vertex_schema_rename_changes_the_cpu_interface_identity(self):
        package = Path(self.run_build()['package'])
        before = execution.cook_runtime(package, self.root / 'a.lurgrt')['interface_identity']
        self.edit(lambda v: v['schemas'][0]['fields'][5].update(id='Tint'))
        for name in ('geometry.slang',):
            path = self.graph.parent / 'main_shaders' / name
            path.write_text(path.read_text().replace('value.Color', 'value.Tint'))
        renamed = Path(self.run_build()['package'])
        after = execution.cook_runtime(renamed, self.root / 'b.lurgrt')['interface_identity']
        self.assertNotEqual(before, after)
        self.assertEqual('RestartRequired', authoring.compare_packages(package, renamed)['cpu_interface'])

    def test_cli_cook_runtime_uses_the_shared_backend(self):
        package = Path(self.run_build()['package'])
        status, payload = run_cli('render-graph', 'cook-runtime', str(self.output), str(self.root / 'cli.lurgrt'))
        self.assertEqual(0, status)
        direct = execution.cook_runtime(package, self.root / 'direct.lurgrt')
        self.assertEqual(direct['generation'], payload['runtime']['generation'])
        self.assertEqual((self.root / 'direct.lurgrt').read_bytes(), (self.root / 'cli.lurgrt').read_bytes())


if __name__ == '__main__':
    unittest.main()
