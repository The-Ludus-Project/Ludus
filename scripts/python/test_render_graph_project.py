"""Project-selected shader tools, build publication and read-only resolution."""
import io
import json
import shutil
from contextlib import redirect_stdout
import unittest
from unittest.mock import patch

import test_render_graph_build as fixtures
from test_ludus_tools import _manifest_json, _write_sdk_prefix
from ludus_tools import cli, lockfile, render_graph_project as project_tools, render_graph_reload
from ludus_tools.create import create_project
from ludus_tools.errors import ToolingError
from ludus_tools.identity import load_prefix_manifest
from ludus_tools.sdkstore import InstalledSdk, SdkStore


class ProjectGraphToolsTests(unittest.TestCase):
    def setUp(self):
        self.builder = fixtures.BuildTests('runTest')
        self.builder.setUp()
        self.addCleanup(self.builder.doCleanups)
        self.root = self.builder.root.resolve()
        self.sdk = self.make_sdk('installed-sdk')
        self.project = self.root / 'Game'
        create_project(self.project, name='Game', template_id='minimal', engine_version='',
                       local_sdk_prefix=self.sdk, preset='linux-clang-development')
        self.tools = self.root / 'trusted-tools'
        self.compiler = self.tools / 'out/shader-tools/slang/bin/slangc'
        self.compiler.parent.mkdir(parents=True)
        self.builder.compiler.chmod(0o755)
        self.compiler.symlink_to(self.builder.compiler)

    def make_sdk(self, name):
        sdk = _write_sdk_prefix(self.root / name, _manifest_json())
        shutil.copytree(self.builder.helper, sdk / 'lib/cmake/Ludus/shaders')
        return sdk

    def resolve(self, **options):
        return project_tools.resolve_build_tools(self.project, self.tools, self.builder.manifest, **options)

    def build_cli(self, *extra):
        output = io.StringIO()
        with redirect_stdout(output):
            code = cli.main(['--json', 'render-graph', 'build', str(self.builder.manifest),
                             str(self.builder.output), '--project', str(self.project),
                             '--tooling-root', str(self.tools), *extra])
        return code, json.loads(output.getvalue())

    def snapshot(self, root):
        return {p.relative_to(root): p.read_bytes() for p in root.rglob('*') if p.is_file()}

    def test_auto_build_uses_selected_sdk_and_prepared_compiler_without_writing_inputs(self):
        before = {root: self.snapshot(root) for root in (self.project, self.sdk, self.tools)}
        code, result = self.build_cli()
        self.assertEqual(0, code, result)
        self.assertEqual(str(self.sdk), result['toolchain']['sdk'])
        self.assertEqual(str(self.sdk / 'lib/cmake/Ludus/shaders'), result['toolchain']['helper'])
        self.assertEqual(str(self.compiler), result['toolchain']['compiler'])
        self.assertNotIn('validator', result['toolchain'])
        self.assertEqual(before, {root: self.snapshot(root) for root in before})
        self.assertTrue((self.builder.output / 'current.json').is_file())

    def test_pre_worker_sdk_builds_without_optional_native_client(self):
        helper = self.sdk / 'lib/cmake/Ludus/shaders'
        self.assertFalse((helper / 'native_worker.py').exists())
        before = self.snapshot(self.sdk)
        code, result = self.build_cli()
        self.assertEqual(0, code, result)
        self.assertEqual(str(helper), result['toolchain']['helper'])
        self.assertTrue((self.builder.output / 'current.json').is_file())
        self.assertEqual(before, self.snapshot(self.sdk))

    def test_sdk_switch_is_resolved_again_and_never_falls_back_to_another_install(self):
        _, first = self.resolve()
        other = self.make_sdk('replacement-sdk')
        path = self.project / '.ludus/local.json'
        local = lockfile.parse_local_settings_file(path)
        local.set_override(target='x86_64-linux-gnu', flavor='Development', prefix=str(other))
        path.write_bytes(local.serialize())
        _, second = self.resolve()
        self.assertNotEqual(first['helper'], second['helper'])
        self.assertEqual(str(other), second['sdk'])
        other.rename(self.root / 'moved-sdk')
        with self.assertRaisesRegex(ToolingError, 'Repair Project Setup'):
            self.resolve()

    def test_locked_sdk_store_selection_uses_the_shared_project_resolver(self):
        (self.project / '.ludus/local.json').unlink()
        path = self.project / 'ludus.lock.json'
        lock = lockfile.parse_lock_file(path)
        lock.resolved = True
        identity = load_prefix_manifest(self.sdk)
        digest = 'a' * 64
        lock.packages = [lockfile.PackageEntry('x86_64-linux-gnu', 'Development', identity.sdk_variant, digest, 1)]
        path.write_bytes(lock.serialize())
        store = SdkStore(self.root / 'store')
        with patch.object(store, 'find_by_digest', return_value=InstalledSdk(identity, self.sdk, digest)) as find:
            _, result = self.resolve(store=store)
        find.assert_called_once_with(digest)
        self.assertEqual('locked release in the shared store', result['sdk_source'])

    def test_project_preset_tools_and_only_required_profile_dependencies_are_selected(self):
        value = self.builder.value()
        value['profiles'] = ['spirv', 'glsl']
        self.builder.manifest.write_text(json.dumps(value))
        cache = {}
        for key, name in [('LUDUS_SLANG_COMPILER', 'slangc'), ('LUDUS_SPIRV_VALIDATOR', 'spirv-val'),
                          ('LUDUS_SPIRV_CROSS', 'spirv-cross')]:
            path = self.project / 'custom-tools' / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('prepared tool')
            path.chmod(0o755)
            cache[key] = {'type': 'FILEPATH', 'value': str(path.relative_to(self.project))}
        (self.project / 'CMakeUserPresets.json').write_text(json.dumps({
            'version': 6, 'configurePresets': [{'name': 'linux-clang-development', 'cacheVariables': cache}]}))
        options, _ = self.resolve()
        self.assertEqual(self.project / 'custom-tools/slangc', options['compiler'])
        self.assertEqual(self.project / 'custom-tools/spirv-val', options['validator'])
        self.assertEqual(self.project / 'custom-tools/spirv-cross', options['spirv_cross'])
        options, _ = self.resolve(helper=self.builder.helper, compiler=self.builder.compiler)
        self.assertEqual(self.builder.helper, options['helper'])
        self.assertEqual(self.builder.compiler, options['compiler'])
        (self.project / 'custom-tools/spirv-val').unlink()
        with self.assertRaisesRegex(ToolingError, 'spirv-val'):
            self.resolve()

    def test_missing_tools_preserve_existing_build_and_runtime(self):
        self.builder.run_build()
        pointer = self.builder.pointer()
        runtime = self.root / 'runtime/main.lurgrt'
        runtime.parent.mkdir()
        runtime.write_bytes(b'last drawable generation')
        self.compiler.unlink()
        code, result = self.build_cli('--runtime-package', str(runtime))
        self.assertNotEqual(0, code)
        self.assertEqual('MissingTools', result['error']['code'])
        self.assertEqual(pointer, self.builder.pointer())
        self.assertEqual(b'last drawable generation', runtime.read_bytes())
        self.compiler.symlink_to(self.builder.compiler)
        (self.sdk / 'lib/cmake/Ludus/shaders/compile_graph_stage.py').unlink()
        code, result = self.build_cli('--runtime-package', str(runtime))
        self.assertNotEqual(0, code)
        self.assertIn('Repair Project Setup', result['error']['message'])
        self.assertEqual(pointer, self.builder.pointer())

    def test_selection_change_during_build_preserves_prior_publication(self):
        self.builder.run_build()
        pointer = self.builder.pointer()
        path = self.project / '.ludus/local.json'
        shader = self.builder.manifest.parent / 'main_shaders/color.slang'
        shader.write_text(shader.read_text() + '\n// trigger a changed shader stage\n')
        self.builder.after_compile = lambda: path.write_bytes(path.read_bytes() + b' ')
        code, result = self.build_cli()
        self.assertNotEqual(0, code)
        self.assertEqual('RenderGraphProjectChanged', result['error']['code'])
        self.assertEqual(pointer, self.builder.pointer())

    def test_selection_change_during_cook_preserves_prior_runtime(self):
        options, _ = self.resolve()
        runtime = self.root / 'runtime/main.lurgrt'
        runtime.parent.mkdir()
        runtime.write_bytes(b'last drawable generation')
        def cook(package, output, **unused):
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(b'replacement')
            path = self.project / '.ludus/local.json'
            path.write_bytes(path.read_bytes() + b' ')
            return {'runtime_package': str(output)}
        with patch.object(render_graph_reload.execution, 'cook_runtime', side_effect=cook):
            with self.assertRaisesRegex(ToolingError, 'selection changed'):
                render_graph_reload.rebuild(self.builder.manifest, self.builder.output, runtime, **options)
        self.assertEqual(b'last drawable generation', runtime.read_bytes())

    def test_retargeted_tool_link_invalidates_an_in_flight_selection(self):
        options, _ = self.resolve()
        other = self.root / 'another-slangc'
        other.write_bytes(self.builder.compiler.read_bytes())
        other.chmod(0o755)
        self.compiler.unlink()
        self.compiler.symlink_to(other)
        with self.assertRaisesRegex(ToolingError, 'selection changed'):
            options['cancel']()


if __name__ == '__main__':
    unittest.main()
