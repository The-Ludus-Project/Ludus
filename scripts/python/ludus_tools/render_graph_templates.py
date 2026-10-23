"""Built-in editable render graph packages, owned by the renderer and shipped in the SDK."""

from pathlib import Path
import json


TEMPLATES = {
    'restir-di': ('restir_di', 'ReSTIR DI', True),
    'visibility-buffer': ('visibility_buffer', 'Visibility buffer', False),
    'visibility-secondary-rays': ('visibility_secondary_rays', 'Visibility + secondary rays', True),
}


def template_files(identity, profiles, graph):
    """Load one canonical source/installed template and validate its complete editable closure."""
    from . import render_graph_build as build
    directory, label, requires_native_rays = TEMPLATES[identity]
    graph._require(not requires_native_rays or any(target in ('metal', 'spirv') for target in profiles),
                   'Capability', label, 'select Metal or Vulkan; this template requires native ray tracing')
    candidates = (Path(__file__).resolve().parents[3] / 'modules/graphics/renderer/render_graphs' / directory,
                  Path(__file__).resolve().parent.parent / 'templates' / directory)
    source = next((path for path in candidates if path.is_dir()), None)
    graph._require(source is not None, 'Source', label, 'built-in template files are missing; repair the engine installation')
    snapshot = build.Snapshot(source)
    text = snapshot.load('main.lushader.json.in').decode('utf-8')
    text = text.replace('@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', json.dumps(list(profiles)))
    manifest = json.loads(text)
    build.validate_manifest(manifest)
    snapshot.load('main.lurender.json')
    for program in manifest['programs']:
        snapshot.shader_closure(program['source'])
    for technique in manifest['techniques']:
        if 'payload' in technique.get('scene_interface', {}):
            snapshot.shader_closure(technique['scene_interface']['payload'])
    snapshot.verify()
    files = {'main.lushader.json': text}
    for relative, data in sorted(snapshot.files.items()):
        if relative != 'main.lushader.json.in':
            files[relative] = data.decode('utf-8')
    return files
