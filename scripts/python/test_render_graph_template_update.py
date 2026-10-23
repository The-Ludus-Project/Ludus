"""Copied templates upgrade by exact content, without replacing authored shader edits."""
import hashlib
import json
import shutil
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import render_graph as graph
from ludus_tools import render_graph_template_update as upgrade
from ludus_tools.errors import ToolingError


class RestirTemplateUpdateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / 'graph'
        graph.new_workspace(self.root, output='offscreen', renderer_static=True,
                            profiles=('metal', 'wgsl'), template='restir-di')
        self.manifest = self.root / 'main.lushader.json'
        self.initial = self.root / 'main_shaders/initial.slang'
        self.helper = self.initial.with_name('initial_emitter_sampling.slang')
        self.updated = self.initial.read_bytes()
        self.previous = (Path(__file__).with_name('fixtures') / 'restir_initial_v1.slang').read_bytes()
        self.assertEqual(hashlib.sha256(self.previous).hexdigest(), upgrade.RESTIR_INITIAL_V1_SHA256)
        self.initial.write_bytes(self.previous)
        self.helper.unlink()

    def test_preview_apply_backup_and_idempotence(self):
        before = self.manifest.read_bytes()
        result = upgrade.update_restir_sampling(self.manifest)
        self.assertFalse(result['applied'])
        self.assertEqual(self.initial.read_bytes(), self.previous)
        self.assertFalse(self.helper.exists())
        result = upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertTrue(result['applied'])
        self.assertEqual(self.initial.read_bytes(), self.updated)
        self.assertTrue(self.helper.exists())
        self.assertEqual((Path(result['backup']) / 'main_shaders/initial.slang').read_bytes(), self.previous)
        self.assertEqual(self.manifest.read_bytes(), before)
        self.assertFalse(upgrade.update_restir_sampling(self.manifest, apply=True)['updates'])

    def test_custom_callback_and_conflicting_helper_are_preserved(self):
        custom = self.previous + b'// authored change\n'
        self.initial.write_bytes(custom)
        result = upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertEqual(result['custom_sources_preserved'], ['main_shaders/initial.slang'])
        self.assertEqual(self.initial.read_bytes(), custom)
        self.initial.write_bytes(self.previous)
        self.helper.write_bytes(b'// authored helper\n')
        with self.assertRaises(ToolingError):
            upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertEqual(self.initial.read_bytes(), self.previous)
        self.assertEqual(self.helper.read_bytes(), b'// authored helper\n')

    def test_write_failure_rolls_back_new_helper_and_preserves_original(self):
        with patch.object(upgrade.build, 'atomic', side_effect=OSError('injected publication failure')):
            with self.assertRaises(OSError):
                upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertFalse(self.helper.exists())
        self.assertEqual(self.initial.read_bytes(), self.previous)

    def test_editor_save_lock_and_escaping_sources_are_rejected(self):
        lock = self.root / '.ludus-editor-render-graph.lock'
        lock.write_text('active editor save')
        with self.assertRaises(ToolingError):
            upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertEqual(self.initial.read_bytes(), self.previous)
        lock.unlink()
        data = json.loads(self.manifest.read_text())
        next(p for p in data['programs'] if p['stage'] == 'closest_hit')['source'] = '../outside.slang'
        self.manifest.write_text(json.dumps(data))
        with self.assertRaises(ToolingError):
            upgrade.update_restir_sampling(self.manifest, apply=True)

    def test_current_callback_repairs_missing_helper_but_preserves_modified_helper(self):
        self.initial.write_bytes(self.updated)
        result = upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertEqual(result['updates'], ['main_shaders/initial_emitter_sampling.slang'])
        self.assertTrue(self.helper.exists())
        self.helper.write_text('// custom helper')
        with self.assertRaises(ToolingError):
            upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertEqual(self.helper.read_text(), '// custom helper')

    def test_nested_backup_symlink_cannot_write_outside_package(self):
        backup = self.root / 'out/template-updates/restir-initial-sampling-v2'
        backup.mkdir(parents=True)
        outside = Path(self.directory.name) / 'outside'
        outside.mkdir()
        (backup / 'main_shaders').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ToolingError):
            upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertFalse((outside / 'initial.slang').exists())
        self.assertEqual(self.initial.read_bytes(), self.previous)

    def test_installed_template_closure_can_upgrade_copied_source(self):
        # Match the installed SDK's tools/ludus_tools + tools/templates layout.
        installed = Path(self.directory.name) / 'sdk/tools'
        module = installed / 'ludus_tools/render_graph_templates.py'
        module.parent.mkdir(parents=True)
        module.touch()
        source = Path(upgrade.templates.__file__).resolve().parents[3] / 'modules/graphics/renderer/render_graphs/restir_di'
        shutil.copytree(source, installed / 'templates/restir_di')
        with patch.object(upgrade.templates, '__file__', str(module)):
            result = upgrade.update_restir_sampling(self.manifest, apply=True)
        self.assertTrue(result['applied'])
        self.assertEqual(self.initial.read_bytes(), self.updated)
        self.assertTrue(self.helper.exists())

    def test_failed_exclusive_publication_leaves_no_partial_helper(self):
        target = self.root / 'new.slang'
        with patch.object(upgrade.os, 'link', side_effect=OSError('injected publication failure')):
            with self.assertRaises(OSError):
                upgrade.publish_new(target, b'complete shader bytes')
        self.assertFalse(target.exists())
        self.assertEqual(list(self.root.glob('.template-update-*')), [])
        target.write_bytes(b'authored')
        with self.assertRaises(FileExistsError):
            upgrade.publish_new(target, b'replacement')
        self.assertEqual(target.read_bytes(), b'authored')
