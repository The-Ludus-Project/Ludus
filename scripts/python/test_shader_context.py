"""Compiler-context export and optional real slangd semantic acceptance."""
import json
import base64
import io
import hashlib
from unittest.mock import patch
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import unquote, urlparse

from ludus_tools import render_graph as graph, render_graph_build as build
from ludus_tools.shader_context import export_context, capture_draft, MAX_DRAFT_INPUT_BYTES, CONTEXT_OWNER_FILE
from ludus_tools.errors import ToolingError


class ShaderContextTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.graph = graph.new_workspace(self.root / 'source')
        self.manifest = build.new_build_sources(self.graph, profiles=('metal',))
        self.helper = self.root / 'helper'
        self.helper.mkdir()
        self.source = 'main_shaders/color.slang'
        self.output = self.root / 'language'

    def export(self, **options):
        return export_context(self.manifest, self.output, self.source, helper=self.helper, **options)

    def test_exact_compiler_wrapper_and_verbatim_unicode_crlf_editable_span(self):
        path = self.manifest.parent / self.source
        authored = b'// \xf0\x9f\x98\x80\r\nfloat4 Main(Varying value) { return float4(1); }\r\n'
        path.write_bytes(authored)
        result = self.export()
        unit = Path(unquote(urlparse(result['uri']).path)).read_bytes()
        self.assertEqual(unit, result['prefix'].encode() + authored + result['suffix'].encode())
        snapshot, _, schemas, programs, *_ = build.prepare_sources(self.manifest, self.helper)
        program = programs[result['context']]
        wrapper = build.shader_source(program, schemas)
        include = '#include "' + self.source + '"\n'
        self.assertEqual(wrapper.replace(include, authored.decode() + '\n'), unit.decode())
        self.assertEqual([{'id': 'Color', 'stage': 'fragment', 'programs': ['Color']}], result['contexts'])
        self.assertEqual(authored, snapshot.files[self.source])

    def test_text_only_changes_reuse_uri_but_dependency_and_schema_changes_invalidate(self):
        path = self.manifest.parent / self.source
        dependency = path.with_name('draft_api.slang')
        dependency.write_text('float ProjectValue() { return 1; }\n')
        path.write_text('#include "draft_api.slang"\n' + path.read_text())
        first = self.export()
        path.write_text(path.read_text() + '// new unsaved body\n')
        second = self.export()
        self.assertEqual(first['uri'], second['uri'])
        dependency.write_text('float ProjectValue() { return 2; }\n')
        third = self.export()
        self.assertNotEqual(first['uri'], third['uri'])
        manifest = json.loads(self.manifest.read_text())
        manifest['schemas'][0]['fields'][0]['id'] = 'RenamedField'
        self.manifest.write_text(json.dumps(manifest))
        # Use the geometry context, which actually consumes the changed schema.
        self.source = 'main_shaders/geometry.slang'
        changed = self.export()
        self.assertIn('RenamedField', changed['prefix'])

    def test_missing_context_escaping_source_and_output_inside_snapshot_are_rejected(self):
        with self.assertRaisesRegex(ToolingError, 'context no longer includes'):
            self.export(context='MissingProgram')
        with self.assertRaises(ToolingError):
            export_context(self.manifest, self.output, '../escape.slang', helper=self.helper)
        with self.assertRaisesRegex(ToolingError, 'separate from source snapshot'):
            export_context(self.manifest, self.manifest.parent / 'language', self.source, helper=self.helper)

    def test_navigation_inventory_maps_exact_captured_sources_and_not_project_paths(self):
        dependency = self.manifest.parent / 'main_shaders/helper.slang'
        dependency.write_bytes(b'// unicode \xf0\x9f\x98\x80\r\nfloat Helper() { return 1; }\r\n')
        source = self.manifest.parent / self.source
        source.write_text('#include "helper.slang"\n' + source.read_text())
        before = source.read_bytes()
        result = self.export()
        entries = {entry['file']: entry for entry in result['navigation_sources']}
        entry = entries['main_shaders/helper.slang']
        captured = Path(unquote(urlparse(entry['uri']).path))
        self.assertTrue(captured.resolve().is_relative_to(self.output.resolve()))
        self.assertFalse(captured.resolve().is_relative_to(self.manifest.parent.resolve()))
        self.assertEqual(dependency.read_bytes(), captured.read_bytes())
        self.assertEqual(hashlib.sha256(captured.read_bytes()).hexdigest(), entry['sha256'])
        self.assertEqual(before, source.read_bytes())

    def test_included_source_retains_its_relative_includes_and_comment_directives(self):
        parent = self.manifest.parent / self.source
        original = parent.read_text()
        parent.write_text('#include "editable.slang"\n' + original)
        self.source = 'main_shaders/editable.slang'
        editable = self.manifest.parent / self.source
        editable.write_text('/*\n#include "missing.slang"\n*/\n#include "other.slang"\nfloat Helper() { return Other(); }\n')
        editable.with_name('other.slang').write_text('float Other() { return 1; }\n')
        result = self.export()
        unit = Path(unquote(urlparse(result['uri']).path)).read_bytes()
        self.assertIn(editable.read_bytes(), unit)
        self.assertTrue(Path(unquote(urlparse(result['uri']).path)).with_name('other.slang').is_file())

    def test_shared_source_has_explicit_distinct_program_contexts(self):
        manifest = json.loads(self.manifest.read_text())
        depth = next(program for program in manifest['programs'] if program['id'] == 'Depth')
        depth['source'] = self.source
        self.manifest.write_text(json.dumps(manifest))
        first = self.export(context='Color')
        second = self.export(context='Depth')
        self.assertEqual({'Color', 'Depth'}, {item['id'] for item in first['contexts']})
        self.assertNotEqual(first['uri'], second['uri'])
        self.assertEqual('Depth', second['context'])
        self.assertEqual(['Color', 'Depth'], second['contexts'][0]['programs'])

    def test_repeated_editable_include_fails_without_guessing_a_span(self):
        parent = self.manifest.parent / self.source
        parent.write_text('#include "repeated.slang"\n#include "repeated.slang"\n' + parent.read_text())
        self.source = 'main_shaders/repeated.slang'
        (self.manifest.parent / self.source).write_text('#pragma once\nfloat Value() { return 1; }\n')
        with self.assertRaisesRegex(ToolingError, 'more than once'):
            self.export()


    def test_captured_draft_is_private_and_removed_on_success_and_failure(self):
        stage = self.root / 'captured'
        authored = b'// unsaved \xf0\x9f\x98\x80\r\n'
        wire = json.dumps({'version': 1, 'files': {'shader/main.slang': base64.b64encode(authored).decode()}}).encode()
        with capture_draft(io.BytesIO(wire), stage) as captured:
            self.assertEqual(authored, (captured / 'shader/main.slang').read_bytes())
        self.assertFalse(stage.exists())
        with self.assertRaisesRegex(RuntimeError, 'backend failure'):
            with capture_draft(io.BytesIO(wire), stage):
                raise RuntimeError('backend failure')
        self.assertFalse(stage.exists())
        stage.mkdir()
        (stage / 'keep').write_text('user file')
        with self.assertRaisesRegex(ToolingError, 'must be new'):
            with capture_draft(io.BytesIO(wire), stage):
                self.fail('existing stage accepted')
        self.assertEqual('user file', (stage / 'keep').read_text())

    def test_capture_rejects_paths_base64_and_capacity_before_writing(self):
        stage = self.root / 'captured'
        for files in ({'../escape': 'eA=='}, {'file': 'invalid!'}, {'file': 1}, {}):
            with self.subTest(files=files), self.assertRaises(ToolingError):
                with capture_draft(io.BytesIO(json.dumps({'version': 1, 'files': files}).encode()), stage):
                    self.fail('invalid capture accepted')
            self.assertFalse(stage.exists())
        oversized = base64.b64encode(b'x' * (build.MAX_FILE + 1)).decode()
        with self.assertRaises(ToolingError):
            with capture_draft(io.BytesIO(json.dumps({'version': 1, 'files': {'file': oversized}}).encode()), stage):
                self.fail('oversized decoded file accepted')
        with patch('ludus_tools.shader_context.MAX_DRAFT_INPUT_BYTES', 8):
            with self.assertRaises(ToolingError):
                with capture_draft(io.BytesIO(b'x' * 9), stage):
                    self.fail('oversized wire accepted')
        self.assertFalse(stage.exists())

    def test_context_cleanup_only_removes_previous_owned_contexts(self):
        first = self.export()
        previous = Path(unquote(urlparse(first['uri']).path)).parents[1]
        unowned = self.output / 'context-user'
        unowned.mkdir()
        (unowned / 'keep').write_text('user file')
        other = self.output / 'other'
        other.mkdir()
        (self.manifest.parent / self.source).with_name('dependency.slang').write_text('float Other() { return 1; }')
        source = self.manifest.parent / self.source
        source.write_text('#include "dependency.slang"\n' + source.read_text())
        second = self.export()
        self.assertNotEqual(first['uri'], second['uri'])
        self.assertFalse(previous.exists())
        self.assertEqual('user file', (unowned / 'keep').read_text())
        self.assertTrue(other.is_dir())
        self.assertTrue(Path(unquote(urlparse(second['uri']).path)).parents[1].joinpath(CONTEXT_OWNER_FILE).is_file())


class SlangPeer:
    """Independent stdio client for checking compiler-owned source with real Slang."""
    def __init__(self, program, workspace):
        self.process = subprocess.Popen([program], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.messages = queue.Queue()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()
        self.serial = 0
        self.call('initialize', {'rootUri': Path(workspace).as_uri(), 'capabilities': {
            'general': {'positionEncodings': ['utf-16']}, 'workspace': {'configuration': True},
            'textDocument': {'completion': {'completionItem': {'snippetSupport': False}}}}})
        self.notify('initialized', {})

    def read(self):
        while True:
            header = self.process.stdout.readline()
            if not header:
                return
            length = int(header.split(b':')[1])
            self.process.stdout.readline()
            self.messages.put(json.loads(self.process.stdout.read(length)))

    def send(self, message):
        body = json.dumps(dict(jsonrpc='2.0', **message)).encode()
        self.process.stdin.write(b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
        self.process.stdin.flush()

    def notify(self, method, parameters):
        self.send({'method': method, 'params': parameters})

    def call(self, method, parameters):
        self.serial += 1
        serial = self.serial
        self.send({'id': serial, 'method': method, 'params': parameters})
        while True:
            message = self.messages.get(timeout=15)
            if 'method' in message and 'id' in message:
                result = None
                if message['method'] == 'workspace/configuration':
                    result = [[] if item['section'] in ('slang.predefinedMacros', 'slang.additionalSearchPaths') else
                              False if item['section'] == 'slang.searchInAllWorkspaceDirectories' else None
                              for item in message['params']['items']]
                self.send({'id': message['id'], 'result': result})
            if message.get('id') == serial and 'method' not in message:
                if 'error' in message:
                    raise AssertionError(message['error'])
                return message['result']

    def close(self):
        self.process.kill()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.thread.join(timeout=5)


@unittest.skipUnless(os.environ.get('LUDUS_TEST_SLANGD') and os.environ.get('LUDUS_RG1_HELPER'),
                     'set LUDUS_TEST_SLANGD and LUDUS_RG1_HELPER for real Slang semantic acceptance')
class SlangSemanticAcceptance(unittest.TestCase):
    def test_scene_material_shader_and_unsaved_package_symbols_use_compiler_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph_path = graph.new_workspace(root / 'source', output='offscreen', renderer_static=True,
                                             profiles=('metal',), template='simple-compute-ray-tracing')
            manifest = graph_path.with_name('main.lushader.json')
            source = 'main_shaders/trace.slang'
            path = manifest.parent / source
            helper = path.with_name('draft_api.slang')
            helper.write_text('float ProjectTint(float value) { return value; }\n')
            path.write_text('#include "draft_api.slang"\nfloat3 TracePixel(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) { return 0; }\n')
            context = export_context(manifest, root / 'language', source, helper=os.environ['LUDUS_RG1_HELPER'])
            peer = SlangPeer(os.environ['LUDUS_TEST_SLANGD'], root / 'language')
            try:
                for serial, (probe, method, expected) in enumerate((
                    ('scene.', 'completion', 'Version'),
                    ('material.', 'completion', 'BaseColor'),
                    ('Evaluat', 'completion', 'EvaluateMaterial'),
                    ('EvaluateMaterial(scene, ', 'signatureHelp', 'SurfaceHit hit'),
                    ('TraceClosest(scene, queries, ', 'signatureHelp', 'Ray ray'),
                    ('ProjectTint(', 'signatureHelp', 'float value'),
                ), 1):
                    before = '#include "draft_api.slang"\n// \U0001f600\nfloat3 TracePixel(SceneContext scene, RayLaunch launch, inout RayQueryContext queries) {\n MaterialSample material = (MaterialSample)0;\n '
                    source_text = before + probe + '\n return 0;\n}\n'
                    text = context['prefix'] + source_text + context['suffix']
                    prefix = context['prefix'] + before + probe
                    position = {'line': prefix.count('\n'), 'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}
                    if serial == 1:
                        peer.notify('textDocument/didOpen', {'textDocument': {'uri': context['uri'], 'languageId': 'slang', 'version': serial, 'text': text}})
                    else:
                        peer.notify('textDocument/didChange', {'textDocument': {'uri': context['uri'], 'version': serial}, 'contentChanges': [{'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': previous.count('\n'), 'character': len(previous.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}}, 'text': text}]})
                    previous = text
                    result = peer.call('textDocument/' + method, {'textDocument': {'uri': context['uri']}, 'position': position})
                    if method == 'completion':
                        items = result if isinstance(result, list) else result.get('items', [])
                        self.assertIn(expected, [item['label'] for item in items], probe)
                    else:
                        self.assertTrue(any(expected in item['label'] for item in result['signatures']), (probe, result))
                        if probe.startswith('EvaluateMaterial'):
                            self.assertEqual(1, result['activeParameter'])
            finally:
                peer.close()
