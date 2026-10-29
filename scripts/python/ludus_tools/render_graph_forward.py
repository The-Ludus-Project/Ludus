"""The opt-in forward material ABI reuses renderer-owned recipes and preparation."""
import re

PROFILE = {'version': 1, 'profile': 'ForwardLit'}
DEFERRED_PROFILE = {'version': 1, 'profile': 'ClusteredDeferred'}
CLUSTERED_PROFILE = {'version': 1, 'profile': 'ClusteredForward'}
# Appended roles preserve every historical raster/ray role and record width.
SUPPORT = {
    'ForwardLit': ('renderer_authored', ('vertex', 'fragment')),
    'ForwardShadow': ('renderer_authored_shadow', ('fragment',)),
    'ForwardOutput': ('renderer_output', ('vertex', 'fragment')),
    'ForwardFlatShadow': ('renderer_shadow', ('fragment',)),
    'ForwardDepth': ('renderer_clustered_authored_depth', ('fragment',)),
    'DeferredGBuffer': ('renderer_deferred_material', ('fragment',)),
    'DeferredLighting': ('renderer_deferred_lighting', ('vertex', 'fragment')),
    'DeferredDepthCopy': ('renderer_deferred_depth', ('fragment',)),
}
ROLE_KEYS = [('ForwardLit', 'vertex'), ('ForwardLit', 'fragment'), ('ForwardShadow', 'fragment'),
             ('ForwardOutput', 'vertex'), ('ForwardOutput', 'fragment'), ('ForwardFlatShadow', 'fragment'),
             ('ForwardDepth', 'fragment'), ('DeferredGBuffer', 'fragment'), ('DeferredLighting', 'vertex'),
             ('DeferredLighting', 'fragment'), ('DeferredDepthCopy', 'fragment')]
MEMBERS = ['ForwardVertex', 'ForwardFragment', 'ForwardShadow', 'LightingVertex', 'LightingFragment', 'LightingShadow', 'ForwardDepth', 'DeferredGBufferFragment', 'DeferredLightingVertex', 'DeferredLightingFragment', 'DeferredDepthCopyFragment']
INPUTS = [{'location': i, 'format': 'Float3' if i in (0, 7) else 'Float2' if i in (6, 13, 15) else 'Float4'} for i in range(16)]


def is_forward(technique):
    return technique.get('scene_interface') in (PROFILE, CLUSTERED_PROFILE, DEFERRED_PROFILE)


def is_clustered(technique):
    return technique.get('scene_interface') in (CLUSTERED_PROFILE, DEFERRED_PROFILE)


def is_deferred(technique):
    return technique.get('scene_interface') == DEFERRED_PROFILE


def support_program(techniques, group, stage):
    owner = next(item for item in techniques.values() if is_forward(item))
    if group == 'DeferredLighting' and stage == 'fragment':
        return 'DeferredLighting'
    if group == 'ForwardLit':
        return owner[stage]
    if group == 'ForwardDepth' and 'DefaultStaticOpaque.DepthOnly' in techniques:
        return techniques['DefaultStaticOpaque.DepthOnly']['fragment']
    return group + stage.title()


def material_layout(snapshot, build, is_clustered_profile=False):
    """Derive the fixed material layout from its existing host/shader owners."""
    lighting = snapshot.files['_ludus_forward/lighting_layout.h'].decode()
    environment = snapshot.files['_ludus_forward/renderer_environment_layout.h'].decode()
    def constant(text, name):
        match = re.search(r'\b' + name + r'(?:\s*=\s*|\s+)([0-9]+)', text)
        build.require(match is not None, 'Layout', name, 'missing owned forward layout constant')
        return int(match[1])
    word = 16
    light_count = 'CLUSTERED_LIGHT_COUNT' if is_clustered_profile else 'FORWARD_LIGHT_COUNT'
    shadow_offset = (constant(lighting, 'FORWARD_HEADER_WORDS') + constant(lighting, light_count) * constant(lighting, 'FORWARD_LIGHT_WORDS')) * word
    surface_offset = shadow_offset + constant(lighting, 'SHADOW_REGION_CAPACITY') * constant(lighting, 'SHADOW_REGION_WORDS') * word
    grid_offset = surface_offset
    if is_clustered_profile:
        surface_offset += (constant(lighting, 'CLUSTERED_GRID_ROWS') + 2) * word
    names = ('Tint', 'UvTransform', 'Options', 'Pbr', 'Emission', 'EnvironmentIrradiance', 'EnvironmentRadiance', 'BakeScale', 'BakeUv', 'Lights')
    offsets = {name: index * word for index, name in enumerate(names)}
    offsets.update(ShadowData=shadow_offset, SurfaceMap=surface_offset, Authored=surface_offset + word, IrradianceSh=surface_offset + 2 * word)
    if is_clustered_profile:
        offsets.update(GridRows=grid_offset, GridMinimum=grid_offset + 3 * word, GridScale=grid_offset + 4 * word)
    shapes = {name: 'Float4Array' if name in ('Lights', 'ShadowData', 'IrradianceSh', 'GridRows') else 'Float4' for name in offsets}
    return {'offsets': offsets, 'shapes': shapes, 'size': surface_offset + (2 + constant(environment, 'LUDUS_ENVIRONMENT_SH_COEFFICIENTS')) * word}


def expand(programs, techniques, snapshot, helper, build):
    selected = [technique for technique in techniques.values() if is_forward(technique)]
    if not selected:
        return programs
    is_deferred_profile = len(selected) == 1 and is_deferred(selected[0])
    is_clustered_profile = len(selected) == 1 and is_clustered(selected[0])
    allowed = {'DefaultStaticOpaque.ColorAndDepth', 'DefaultStaticOpaque.DepthOnly'} if is_clustered_profile else {'DefaultStaticOpaque.ColorAndDepth'}
    build.require(len(selected) == 1 and set(techniques) <= allowed and selected[0]['id'] == 'DefaultStaticOpaque.ColorAndDepth',
                  'Capability', 'ForwardLit', 'the first forward profile requires one opaque ColorAndDepth technique')
    recipes = build.document(build.read(helper / 'recipes.json'), 'renderer recipes')
    # Capture the renderer manifest and its literal shader closure; consumers never guess paths/entries.
    build.require('_ludus_forward/recipes.json' not in snapshot.files, 'Source', 'ForwardLit', 'reserved engine forward helper path')
    captured = set()
    snapshot.files['_ludus_forward/recipes.json'] = build.read(helper / 'recipes.json')
    snapshot.external['_ludus_forward/recipes.json'] = helper / 'recipes.json'
    def capture(filename):
        relative = '_ludus_forward/' + filename
        if relative in captured:
            return
        build.require(relative not in snapshot.files, 'Source', relative, 'reserved engine forward helper path')
        captured.add(relative)
        data = build.read(helper / filename)
        snapshot.files[relative] = data
        snapshot.external[relative] = helper / filename
        snapshot.total += len(data)
        build.require(snapshot.total <= build.MAX_BYTES and len(snapshot.files) <= build.MAX_FILES,
                      'Capacity', relative, 'forward helper closure exceeds package bounds')
        for child in re.findall(r'^\s*#\s*include\s+"([^"\n]+)"', data.decode(), re.M):
            capture(child)
    technique = selected[0]
    depth = techniques.get('DefaultStaticOpaque.DepthOnly')
    build.require(depth is None or depth['vertex'] == technique['vertex'], 'Capability', 'ClusteredForward',
                  'the prepass must declare the same authored geometry program as color')
    build.require(is_deferred_profile is False or programs.get('DeferredLighting', {}).get('stage') == 'fragment',
                  'Capability', 'ClusteredDeferred', 'an authored DeferredLighting fragment callback is required')
    result = dict(programs)
    for group, (recipe_name, stages) in SUPPORT.items():
        if group.startswith('Deferred') and not is_deferred_profile:
            continue
        if group == 'ForwardDepth' and not is_clustered_profile:
            continue
        if is_clustered_profile and group in ('ForwardLit', 'ForwardShadow'):
            recipe_name = 'renderer_clustered_authored' + ('_shadow' if group == 'ForwardShadow' else '')
        recipe = recipes[recipe_name]
        capture(recipe['source'])
        for stage in stages:
            identity = support_program(techniques, group, stage)
            original = programs.get(technique['fragment'] if group == 'DeferredGBuffer' else identity)
            source = '#include "_ludus_forward/' + recipe['source'] + '"\n'
            if original is not None:
                source += '#include "' + original['source'] + '"\n'
            function = original['function'] if original is not None else recipe[stage]
            argument = ('DeferredInput' if stage == 'vertex' else 'DeferredVarying') if group in ('DeferredLighting', 'DeferredDepthCopy') else 'Input' if stage == 'vertex' else 'Varying'
            output = ('DeferredVarying' if group == 'DeferredLighting' else 'Varying') if stage == 'vertex' else 'void' if group == 'ForwardDepth' else 'GBufferOutput' if group == 'DeferredGBuffer' else 'DeferredLightingOutput' if group == 'DeferredLighting' else 'DeferredDepthOutput' if group == 'DeferredDepthCopy' else 'float4'
            expression = function + '(value)'
            if is_deferred_profile and group == 'ForwardLit' and stage == 'fragment':
                expression = 'ShadeMaterial(' + expression + ', value)'
            elif group == 'DeferredGBuffer':
                expression = 'PackDeferredMaterial(' + expression + ')'
            semantic = ' : SV_Target0' if stage == 'fragment' and output == 'float4' else ''
            source += '[shader("' + stage + '")]\n' + output + ' rgEntry(' + argument + ' value)' + semantic + '\n{\n    ' + ('' if group == 'ForwardDepth' else 'return ') + expression + ';\n}\n'
            result[identity] = {'id': identity, 'stage': stage, 'source': original['source'] if original else '_ludus_forward/' + recipe['source'],
                                'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [],
                                'forward_wrapper': source, 'forward_group': group,
                                'fragment_storage': group == 'DeferredLighting' and stage == 'fragment',
                                'forward_layout': material_layout(snapshot, build, is_clustered_profile) if group in ('ForwardLit', 'ForwardShadow', 'DeferredGBuffer', 'DeferredLighting') else None}
    # The existing flat pair initializes the renderer's shared mesh/view services.
    recipe = recipes['renderer_flat']
    capture(recipe['source'])
    for stage in ('vertex', 'fragment'):
        identity = 'ForwardSetup' + stage.title()
        output, argument = ('Varying', 'Input') if stage == 'vertex' else ('float4', 'Varying')
        code = '#include "_ludus_forward/' + recipe['source'] + '"\n'
        code += '[shader("' + stage + '")]\n' + output + ' rgEntry(' + argument + ' value)' + (' : SV_Target0' if stage == 'fragment' else '') + '\n{\n    return ' + recipe[stage] + '(value);\n}\n'
        result[identity] = {'id': identity, 'stage': stage, 'source': '_ludus_forward/' + recipe['source'],
                            'function': 'rgEntry', 'input': '', 'output': '', 'bindings': [], 'forward_wrapper': code,
                            'forward_group': 'ForwardSetup'}
    return result


def capture_closure(program, snapshot):
    roots = re.findall(r'^#include "([^"\n]+)"', program['forward_wrapper'], re.M)
    closure = {path for path in snapshot.files if path.startswith('_ludus_forward/')}
    for root in roots:
        if not root.startswith('_ludus_forward/'):
            closure.update(snapshot.shader_closure(root))
    return sorted(closure)


def check_interface(program, result, build):
    group, stage = program['forward_group'], program['stage']
    inputs = INPUTS if group == 'ForwardLit' and stage == 'vertex' else [
        {'location': i, 'format': 'Float3' if i == 0 else 'Float4'} for i in range(6)
    ] if group == 'ForwardSetup' and stage == 'vertex' else [{'location': 0, 'format': 'Float4'}] if group in ('ForwardOutput', 'DeferredLighting') and stage == 'vertex' else []
    actual = [{k: v for k, v in entry.items() if k != 'name'} for entry in result['interface']['inputs']]
    build.require(actual == inputs, 'Layout', program['id'], 'forward recipe vertex interface mismatch')
    entries = result['interface']['entries']
    is_clustered_profile = program.get('forward_layout') is not None and 'GridRows' in program['forward_layout']['offsets']
    if group.startswith('Deferred'):
        kinds = ('UniformBuffer', 'Texture2D', 'Sampler', 'Texture2D', 'Texture2D', 'Sampler', 'Texture2D', 'Texture2D', 'Texture2D', 'Sampler', 'Texture2D', 'Texture2D', 'Texture2D', 'Texture2D', 'TextureDepth2D', 'UniformBuffer')
        if group == 'DeferredLighting':
            kinds = kinds[:3] + ('StorageRead', 'StorageRead') + kinds[5:]
        allowed = {0: 'TextureDepth2D'} if group == 'DeferredDepthCopy' else dict(enumerate(kinds[:11] if group == 'DeferredGBuffer' else kinds))
        build.require(all(allowed.get(entry['binding']) == entry['kind'] for entry in entries),
                      'Layout', program['id'], 'deferred resources differ from the owned ABI')
        if stage == 'fragment' and group != 'DeferredDepthCopy':
            uniform = next((entry for entry in entries if entry['binding'] == 0), {})
            build.require(all(uniform.get(field) == value for field, value in program['forward_layout'].items()),
                          'Layout', program['id'], 'deferred material layout differs from the owned ABI')
        if group == 'DeferredLighting' and stage == 'fragment':
            view = next((entry for entry in entries if entry['binding'] == 15), {})
            layout = {'size': 112, 'offsets': {'ClipToWorldRows': 0, 'EyeWorld': 64, 'TargetSizePixel': 80, 'Options': 96},
                      'shapes': {'ClipToWorldRows': 'Float4Array', 'EyeWorld': 'Float4', 'TargetSizePixel': 'Float4', 'Options': 'Float4'}}
            build.require(all(view.get(field) == value for field, value in layout.items()),
                          'Layout', program['id'], 'deferred view layout differs from the owned ABI')
        return []
    expected = (11 if is_clustered_profile else 10) if group == 'ForwardLit' and stage == 'fragment' else 3 if stage == 'fragment' and group in ('ForwardShadow', 'ForwardOutput') else 0
    build.require(len(entries) == expected, 'Layout', program['id'], 'forward recipe binding interface mismatch')
    if expected:
        kinds = ('UniformBuffer', 'Texture2D', 'Sampler', 'Texture2D', 'Texture2D', 'Sampler', 'Texture2D', 'Texture2D', 'Sampler', 'Texture2D')
        if is_clustered_profile:
            kinds = kinds[:8] + ('Texture2D', 'Sampler', 'Texture2D')
        build.require([(entry['binding'], entry['kind']) for entry in entries] == list(enumerate(kinds[:expected])),
                      'Layout', program['id'], 'forward resource slots/kinds differ from the owned ABI')
        uniform = entries[0]
        layout = program.get('forward_layout') or {'size': 16, 'offsets': {'Output': 0}, 'shapes': {'Output': 'Float4'}}
        build.require(all(uniform.get(field) == value for field, value in layout.items()),
                      'Layout', program['id'], 'forward uniform layout differs from the host/shader owner')

    return []
