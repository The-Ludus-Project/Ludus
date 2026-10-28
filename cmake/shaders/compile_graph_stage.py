"""RG1 single-stage adapter at the existing pinned shader-helper boundary.

Thanks to the Slang authors, Using the Reflection API (User Guide),
https://shader-slang.org/slang/user-guide/reflection.html: validate each target's
layout independently. This bounded adapter supports float scalar/vector schemas;
matrices, arrays, textures, storage and ray stages require a separate profile.
"""
import hashlib
import json
from pathlib import Path
import re

from compile_raster import (glsl_interface, glsl_varyings, raster_interface,
                            uniform_shape, wgsl_contract)
from compile_shader import entry, relative_line_directives, run


def varying_interface(point, stage):
    """Extract typed stage links, excluding builtin position/depth semantics."""
    def fields(value):
        ty = value.get('type', {})
        if ty.get('kind') == 'struct':
            return [item for child in ty['fields'] for item in fields(child)]
        semantic = value.get('semanticName', '')
        if semantic.startswith('SV_'):
            return []
        binding = value.get('binding', {})
        shape = uniform_shape(ty)
        if binding.get('kind') not in ('varyingInput', 'varyingOutput') or shape not in ('Float32', 'Float2', 'Float3', 'Float4'):
            raise RuntimeError('RG1 stage links require reflected float scalar/vector locations')
        return [{'location': binding['index'], 'type': shape}]
    values = [point.get('result', {})] if stage == 'vertex' else point.get('parameters', [])
    result = [item for value in values if value.get('type', {}).get('kind') != 'void' for item in fields(value)]
    if len({x['location'] for x in result}) != len(result):
        raise RuntimeError('RG1 duplicate stage link location')
    return sorted(result, key=lambda x: x['location'])


def verify_glsl_links(reflected, emitted, stage):
    """Allow optimized-away fragment inputs, preserving typed vertex outputs."""
    provided = {value['location']: value['type'] for value in reflected}
    consumed = {value['location']: value['type'] for value in emitted}
    if (len(provided) != len(reflected) or len(consumed) != len(emitted)
            or any(provided.get(location) != kind for location, kind in consumed.items())
            or (stage == 'vertex' and provided != consumed)):
        raise RuntimeError('RG1 Slang and emitted GLSL stage links differ')
    return emitted


def metal_layouts(code, interface, document, allows_arrays=False):
    """Independently derive emitted scalar/vector Metal uniform struct layouts."""
    for entry_value in interface['entries']:
        if entry_value.get('kind', 'UniformBuffer') != 'UniformBuffer':
            continue
        resource = next(p for p in document['parameters'] if p['name'] == entry_value['name'])
        type_name = resource['type']['elementType'].get('name')
        if type_name is None:
            arguments = re.findall(r'\b(\w+)\s+constant\s*\*\s*\w+\s*\[\[buffer\(' + str(entry_value['binding']) + r'\)\]\]', code)
            if len(arguments) != 1:
                raise RuntimeError('RG1 emitted Metal cbuffer type is missing or ambiguous')
            type_name = arguments[0]
        bodies = re.findall(r'struct\s+' + re.escape(type_name) + r'(?:_\d+)?\s*\{([^}]+)\}', code)
        if len(bodies) != 1:
            raise RuntimeError('RG1 emitted Metal uniform struct is missing or ambiguous')
        offset, alignment, offsets = 0, 1, {}
        for declaration in bodies[0].split(';'):
            if not declaration.strip():
                continue
            field = re.fullmatch(r'\s*(float[234]?)\s+(\w+)\s*', declaration)
            count = 1
            if field is None and allows_arrays:
                array = re.fullmatch(r'\s*array<(float4),\s*int\(([0-9]+)\)>\s+(\w+)\s*', declaration)
                if array:
                    ty, count, emitted = array.groups()
                    count = int(count)
                    if not 0 < count <= 512:
                        raise RuntimeError('Forward Metal array exceeds bounded material ABI')
                else:
                    raise RuntimeError('Forward Metal uniforms require vectors or bounded float4 arrays')
            elif field is None:
                raise RuntimeError('RG1 Metal uniforms require scalar/vector fields')
            else:
                ty, emitted = field.groups()
            width = int(ty[-1]) if ty[-1].isdigit() else 1
            size = 16 if width == 3 else 4 * width
            align = size
            alignment = max(alignment, align)
            offset = (offset + align - 1) // align * align
            stem = re.sub(r'_\d+$', '', emitted)
            if stem in offsets:
                raise RuntimeError('RG1 Metal uniform field is duplicated')
            offsets[stem] = offset
            offset += size * count
        size = (offset + alignment - 1) // alignment * alignment
        if offsets != entry_value['offsets'] or size != entry_value['size']:
            raise RuntimeError('RG1 emitted Metal packing differs from target reflection')


def verify_ray_compute_metal(code, interface, requires_native_queries=True):
    """Verify translated compute bindings and the selected native/software traversal profile."""
    scenes = [entry for entry in interface['entries'] if entry['kind'] == 'RayScene']
    if requires_native_queries and (len(scenes) != 1 or 'intersection_query<' not in code):
        raise RuntimeError('Native ray Metal artifact lacks a single TLAS/query interface')
    if not requires_native_queries and (scenes or 'intersection_query<' in code or 'metal_raytracing' in code):
        raise RuntimeError('Software ray Metal artifact must not require native ray traversal')
    for binding in interface['entries']:
        resource = 'texture' if binding['kind'] in ('Texture2D', 'TextureDepth2D') else 'buffer'
        if '[[' + resource + '(' + str(binding['binding']) + ')]]' not in code:
            raise RuntimeError('Ray compute Metal binding differs from target reflection')
    names = re.findall(r'kernel\s+void\s+(\w+)\s*\(', code)
    if len(names) != 1:
        raise RuntimeError('Ray compute Metal artifact requires one compute entry')
    return names[0]


def verify_native_ray_metal(code, interface):
    return verify_ray_compute_metal(code, interface)


def compile_graph_stage(args, lock):
    target, stage = args.graph_target, args.graph_stage
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    native_pipeline = getattr(args, 'native_ray_pipeline', False)
    native_rays = getattr(args, 'native_ray_queries', False) and not native_pipeline
    if native_pipeline and (target != 'metal' or stage != 'compute'):
        raise RuntimeError('Native ray stages require the Metal ray pipeline backend')
    software_rays = getattr(args, 'graph_software_rays', False)
    translated_compute = native_rays or software_rays
    if software_rays and (native_rays or stage != 'compute' or target not in ('spirv', 'metal')):
        raise RuntimeError('Software ray traversal requires an ordinary native compute target')
    if native_rays and (stage != 'compute' or target not in ('spirv', 'metal')):
        raise RuntimeError('Hardware ray queries require a native Metal or Vulkan compute target')
    metal = target == 'metal'
    slang_target = 'spirv' if target == 'glsl' or translated_compute else target
    if target in ('spirv', 'glsl') or (native_rays and args.validator):
        if not args.validator or hashlib.sha256(Path(args.validator).read_bytes()).hexdigest() != lock['spirv_tools']['val_sha256']:
            raise RuntimeError('RG1 SPIR-V validator differs from SDK pin')
    if target == 'glsl' or (translated_compute and metal):
        if not args.spirv_cross or not args.spirv_cross_lock:
            raise RuntimeError('RG1 GLSL requires the pinned SPIRV-Cross translator')
        pin = json.loads(Path(args.spirv_cross_lock).read_text())['spirv_cross']
        tool = Path(args.spirv_cross).resolve()
        build = json.loads(Path(str(tool) + '.build.json').read_text())
        if any(build.get(k) != pin[k] for k in ('tag', 'commit', 'source_sha256')) or build.get('binary_sha256') != hashlib.sha256(tool.read_bytes()).hexdigest():
            raise RuntimeError('RG1 SPIRV-Cross identity differs from SDK pin')
    artifact = output / ('artifact.' + {'spirv': 'spv', 'wgsl': 'wgsl', 'metal': 'metal', 'glsl': 'essl'}[target])
    intermediate = output / 'intermediate.spv' if target == 'glsl' or (translated_compute and metal) else artifact
    reflection, depfile = output / 'reflection.json', output / 'stage.d'
    profile = ('spirv_1_5' if native_rays else 'spirv_1_3') if slang_target == 'spirv' else 'sm_6_0'
    command = [args.compiler, args.source, '-target', slang_target, '-profile', profile,
               '-entry', args.name, '-stage', stage, '-o', str(intermediate),
               '-reflection-json', str(reflection), '-depfile', str(depfile)]
    if native_rays:
        command += ['-capability', 'spvRayQueryKHR']
    if getattr(args, 'worker_socket', None) and not native_pipeline:
        from native_worker import compile_stage
        compile_stage(args.worker_socket, args, lock, intermediate, reflection, depfile)
    else:
        run(command)
    document = json.loads(reflection.read_text())
    points = document.get('entryPoints', [])
    if len(points) != 1 or points[0].get('stage') != stage or points[0].get('name') != args.name:
        raise RuntimeError('RG1 requested entry/stage differs from target reflection')
    if native_pipeline:
        from compile_metal_ray import legalize
        code, document = legalize(artifact.read_text(), document, Path(args.source).parent / '_ludus_ray/metal_ray_pipeline_layout.h', json.loads(args.native_ray_storage))
        artifact.write_text(code)
        reflection.write_text(json.dumps(document))
    interface = raster_interface(document, stage, metal and not translated_compute and not native_pipeline, structured_records=translated_compute or native_pipeline)
    if translated_compute:
        from compile_raster import verify_storage_record_strides
        verify_storage_record_strides(intermediate.read_bytes(), interface)
    allowed = ('UniformBuffer', 'StorageRead') if stage == 'fragment' and getattr(args, 'fragment_storage', False) else ('UniformBuffer',)
    if getattr(args, 'graph_forward_material', False) and stage == 'fragment':
        allowed = ('UniformBuffer', 'Texture2D', 'TextureDepth2D', 'Sampler')
    if stage != 'compute' and any(x['kind'] not in allowed for x in interface['entries']):
        raise RuntimeError('RG1 initial schema profile permits uniform buffers only')
    links = [] if stage == 'compute' else varying_interface(points[0], stage)
    if slang_target == 'spirv' and (not metal or args.validator):
        run([args.validator, '--target-env', 'vulkan1.2' if native_rays else 'vulkan1.1', str(intermediate)])
    if target == 'spirv':
        emitted = entry(artifact)
    elif target == 'wgsl':
        code = wgsl_contract(artifact.read_text(), interface, stage)
        names = re.findall(r'@' + stage + r'\s+(?:@workgroup_size\([^)]*\)\s+)?fn\s+(\w+)', code)
        if len(names) != 1:
            raise RuntimeError('RG1 requires exactly one emitted WGSL stage entry')
        artifact.write_text(code)
        emitted = names[0]
    elif metal and translated_compute:
        run([args.spirv_cross, str(intermediate), '--msl', '--msl-version', '20400', '--msl-decoration-binding', '--output', str(artifact)])
        code = relative_line_directives(artifact.read_text(), [Path(args.source).parent])
        emitted = verify_ray_compute_metal(code, interface, native_rays)
        artifact.write_text(code)
    elif metal:
        native_stage = 'kernel' if stage == 'compute' else stage
        names = re.findall(r'\[\[' + native_stage + r'\]\]\s+\w+\s+(\w+)\s*\(', artifact.read_text())
        if len(names) != 1:
            raise RuntimeError('RG1 requires exactly one emitted Metal stage entry')
        code = artifact.read_text()
        if stage in ('compute', 'fragment'):
            from compile_raster import normalize_metal_storage
            code = normalize_metal_storage(code, interface)
        if stage != 'compute':
            metal_layouts(code, interface, document, getattr(args, 'graph_forward_material', False))
        code = relative_line_directives(code, [Path(args.source).parent])
        artifact.write_text(code)
        emitted = names[0]
    else:
        run([args.spirv_cross, str(intermediate), '--version', '300', '--es', '--fixup-clipspace', '--output', str(artifact)])
        cross = json.loads(run([args.spirv_cross, str(intermediate), '--reflect']))
        code, varyings = glsl_varyings(artifact.read_text(), cross, stage)
        interface['blocks'], interface['textures'] = glsl_interface(code, interface, cross)
        cross_links = [{'location': loc, 'type': {'float': 'Float32', 'vec2': 'Float2', 'vec3': 'Float3', 'vec4': 'Float4'}[ty]} for loc, ty in varyings]
        links = verify_glsl_links(links, cross_links, stage)
        artifact.write_text(code)
        emitted = 'main'
    # Result contains no machine-specific commands or temporary paths. Raw
    # reflection is used by the shared schema validator before publication.
    result = {'version': 1, 'target': target, 'stage': stage, 'source_entry': args.name,
              'entry': emitted, 'artifact': artifact.name, 'interface': interface,
              'links': links, 'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()}
    (output / 'stage.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
