"""Built-in editable render graph packages, owned by the renderer and shipped in the SDK."""

from pathlib import Path
import json


def restir_files(profiles, graph):
    from . import render_graph_build as build
    graph._require(any(target in ('metal', 'spirv') for target in profiles), 'Capability', 'ReSTIR DI',
                   'select Metal or Vulkan; ReSTIR DI requires native ray tracing')
    candidates = (Path(__file__).resolve().parents[3] / 'modules/graphics/renderer/render_graphs/restir_di',
                  Path(__file__).resolve().parent.parent / 'templates/restir_di')
    source = next((path for path in candidates if path.is_dir()), None)
    graph._require(source is not None, 'Source', 'ReSTIR DI', 'built-in template files are missing; repair the engine installation')
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
