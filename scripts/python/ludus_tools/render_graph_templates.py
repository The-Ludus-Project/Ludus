"""Built-in editable render graph packages, owned by the renderer and shipped in the SDK."""

from pathlib import Path
import json


# Directory, label and the editor template registry's requirement (render_graph_templates.json):
# None for portable templates, otherwise a requirement that needs a Metal or Vulkan profile.
TEMPLATES = {
    'raster': ('default', 'Raster', None),
    'forward-lit': ('forward_lit', 'Forward lit', None),
    'restir-di': ('restir_di', 'ReSTIR DI', 'native-ray-tracing'),
    'visibility-buffer': ('visibility_buffer', 'Visibility buffer', None),
    'opaque-material-resolve': ('visibility_buffer', 'Opaque material resolve', 'compute-ray-tracing'),
    'visibility-secondary-rays': ('visibility_secondary_rays', 'Visibility + secondary rays', 'native-ray-tracing'),
    'hybrid-shadows': ('hybrid_shadows', 'Hybrid shadows', 'native-ray-tracing'),
    'hybrid-reflections': ('hybrid_reflections', 'Hybrid reflections', 'native-ray-tracing'),
    'hybrid-indirect-diffuse': ('hybrid_indirect_diffuse', 'Hybrid indirect diffuse', 'native-ray-tracing'),
    'tiled-vbuffer-nee': ('tiled_vbuffer_nee', 'Tiled visibility-buffer NEE', 'native-ray-tracing'),
    'simple-compute-ray-tracing': ('simple_compute_ray_tracing', 'Simple compute ray tracing', 'native-ray-tracing'),
    'compute-ray-tracing': ('compute_ray_tracing', 'Compute ray tracing', 'native-ray-tracing'),
    'veach-guibas-mis': ('veach_guibas_mis', 'Veach and Guibas MIS', 'native-ray-tracing'),
    'line-sampling': ('line_sampling', 'Line sampling', 'compute-ray-tracing'),
    'ris-projected-solid-angle': ('ris_projected_solid_angle', 'RIS and projected solid angle', 'compute-ray-tracing'),
    'salesin-jarosz-lines': ('salesin_jarosz_lines', 'Salesin and Jarosz point and line MIS', 'compute-ray-tracing'),
    'polygonal-light-sampling': ('polygonal_light_sampling', 'Polygonal light sampling', 'compute-ray-tracing'),
    'slice-sampling-luminaire': ('slice_sampling_luminaire', 'Slice sampling luminaire', 'compute-ray-tracing'),
    'rough-pair-slice': ('rough_pair_slice', 'Rough pair slice sampling', 'compute-ray-tracing'),
    'steinberg-plt': ('steinberg_plt', 'Steinberg physical light transport', 'native-ray-tracing'),
    'steinberg-physical-optics': ('steinberg_physical_optics', 'Steinberg practical physical optics',
                                  'native-ray-tracing'),
    'wave-tracing': ('wave_tracing', 'Wave tracing scene optical transport', 'compute-ray-tracing'),
    'rao-blackwell-mh': ('rao_blackwell_mh', 'Rao-Blackwellized MH', 'compute-ray-tracing'),
    'guided-restir-fg-plus': ('guided_restir_fg_plus', 'Guided ReSTIR FG+', 'compute-ray-tracing'),
    'conditional-restir': ('conditional_restir', 'Conditional ReSTIR', 'native-ray-tracing'),
    'one-more-vertex-nee': ('one_more_vertex_nee', 'One-more-vertex NEE', 'compute-ray-tracing'),
    'light-transformer': ('light_transformer', 'Light Transformer irradiance', 'compute-ray-tracing'),
    'restir-gi': ('restir_gi', 'ReSTIR GI', 'native-ray-tracing'),
}
# What each requirement needs, as the capability repair names it.
REQUIREMENT_REPAIRS = {
    'native-ray-tracing': 'native ray tracing',
    'compute-ray-tracing': 'Metal or Vulkan compute',
}


def template_files(identity, profiles, graph):
    """Load one canonical source/installed template and validate its complete editable closure."""
    from . import render_graph_build as build
    directory, label, requirement = TEMPLATES[identity]
    graph._require(requirement is None or any(target in ('metal', 'spirv') for target in profiles),
                   'Capability', label,
                   f'select Metal or Vulkan; this template requires {REQUIREMENT_REPAIRS.get(requirement)}')
    candidates = (Path(__file__).resolve().parents[3] / 'modules/graphics/renderer/render_graphs' / directory,
                  Path(__file__).resolve().parent.parent / 'templates' / directory)
    source = next((path for path in candidates if path.is_dir()), None)
    graph._require(source is not None, 'Source', label, 'built-in template files are missing; repair the engine installation')
    snapshot = build.Snapshot(source)
    # Two starters compose the same visibility producer and raster shader sources.
    stem = 'resolve' if identity == 'opaque-material-resolve' else 'main'
    root_graph = stem + '.lurender.json'
    root_manifest = stem + '.lushader.json.in'
    text = snapshot.load(root_manifest).decode('utf-8')
    text = text.replace('@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@', json.dumps(list(profiles)))
    if identity == 'light-transformer':
        snapshot.load('light_transformer/model.json')
        document = json.loads(text)
        document['profiles'] = [profile for profile in profiles if profile in ('metal', 'spirv')]
        text = json.dumps(document, indent=2) + '\n'
    manifest = json.loads(text)
    build.validate_manifest(manifest)
    pending = [root_graph]
    captured = set()
    while pending:
        relative = pending.pop()
        if relative in captured:
            continue
        captured.add(relative)
        asset = json.loads(snapshot.load(relative))
        pending.extend(node['asset'] for configuration in asset['configurations']
                       for node in configuration['nodes'] if node['kind'] == 'Subgraph')
    for program in manifest['programs']:
        snapshot.shader_closure(program['source'])
    for technique in manifest['techniques']:
        if 'payload' in technique.get('scene_interface', {}):
            snapshot.shader_closure(technique['scene_interface']['payload'])
    snapshot.verify()
    files = {'main.lushader.json': text}
    for relative, data in sorted(snapshot.files.items()):
        if relative != root_manifest:
            files['main.lurender.json' if relative == root_graph else relative] = data.decode('utf-8')
    return files
