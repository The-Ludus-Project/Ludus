"""Bounded raster and buffer-compute artifacts/reflection for the installed SDK."""
import hashlib
import json
from pathlib import Path
import re
import struct

from compile_shader import relative_line_directives

# Thanks to the Slang team, "Using the Reflection API", User Guide,
# https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-reflection.html
# Reflect each target independently. No cross-target CPU packing is inferred.
# Raster rejects storage; compute accepts bounded structured buffers. Both reject
# extra groups and unsupported inputs. Native descriptor arrays use explicit profiles.
# Must match rhi::RASTER_BINDING_COUNT in profile.h; regression-checked.
RASTER_BINDING_COUNT = 16
RUNTIME_BINDING_LENGTH_SLOT = RASTER_BINDING_COUNT - 2
RUNTIME_BINDING_ARRAY_SLOT = RASTER_BINDING_COUNT - 1

def uniform_shape(value):
    kind = value.get('kind')
    if kind == 'scalar' and value.get('scalarType') == 'float32':
        return 'Float32'
    if value.get('elementType', {}).get('scalarType') == 'float32':
        if kind == 'vector' and value.get('elementCount') in (2,3,4):
            return 'Float' + str(value['elementCount'])
        if kind == 'matrix' and value.get('rowCount') == value.get('columnCount') == 4:
            return 'Float4x4'
    if kind == 'array' and type(value.get('elementCount')) is int and 1 <= value['elementCount'] <= 1024:
        shape = uniform_shape(value.get('elementType', {}))
        if shape in ('Float4', 'Float4x4') and value['elementCount'] * (16 if shape == 'Float4' else 64) <= 16384:
            return shape + 'Array'
    return 'Opaque'


def external_text_source(namespace, label, data):
    """Store exact UTF-8 shader bytes in one compiled file, without giant literals."""
    encoded = data.encode("utf-8") + b"\0"
    values = [str(value) if value < 128 else f"static_cast<char>({value})" for value in encoded]
    rows = [", ".join(values[index:index + 32]) for index in range(0, len(values), 32)]
    declaration = f"extern const char {label}[{len(encoded)}];\n"
    definition = (f"namespace ludus::shaders::{namespace} {{\n"
                  f"extern const char {label}[{len(encoded)}] = {{\n" +
                  ",\n".join(rows) + "\n};\n}\n")
    return declaration, definition


def raster_interface(document, stage, metal=False, native_arrays=False, runtime_arrays=False):
    points = [x for x in document['entryPoints'] if x['stage'] == stage]
    if len(points) != 1:
        raise RuntimeError('Expected one entry per raster stage')
    point = points[0]
    used = {}
    for resource in point.get('bindings', []):
        bindings = resource.get('bindings', [resource.get('binding', {})])
        if any('used' not in b for b in bindings):
            raise RuntimeError('Raster stage usage is missing; compile stages separately')
        used[resource['name']] = any(b['used'] for b in bindings)
    if 'nativeEntryPointResourceUsage' in document:
        usage = document['nativeEntryPointResourceUsage']
        names = [resource['name'] for resource in document.get('parameters', [])]
        if not isinstance(usage, dict) or set(usage) != set(names) or any(type(value) is not bool for value in usage.values()):
            raise RuntimeError('Native raster stage usage is malformed')
        used = usage
    entries = []
    for resource in document.get('parameters', []):
        if not used.get(resource['name'], False):
            continue
        bindings = resource.get('bindings', [resource.get('binding', {})])
        descriptors = [b for b in bindings if b.get('kind') == 'descriptorTableSlot']
        if metal and not descriptors:
            native_kinds = ('constantBuffer',) if stage == 'compute' else ('constantBuffer', 'shaderResource', 'samplerState')
            descriptors = [b for b in bindings if b.get('kind') in native_kinds]
        if len(descriptors) != 1 or descriptors[0].get('space', 0) != 0:
            raise RuntimeError('Raster resources require one descriptor in group/set zero')
        index = descriptors[0]['index']
        if not 0 <= index < RASTER_BINDING_COUNT or any(e['binding'] == index for e in entries):
            raise RuntimeError('Raster binding is duplicated or outside the bounded profile')
        resource_type = resource['type']
        count = 1
        is_runtime = False
        if resource_type['kind'] == 'array':
            count = resource_type.get('elementCount')
            is_runtime = count == 0
            if is_runtime:
                if metal or not runtime_arrays or index != RUNTIME_BINDING_ARRAY_SLOT:
                    raise RuntimeError('Runtime descriptor arrays require the explicit Vulkan runtime profile and reserved table slot')
                count = 1
            if not native_arrays or type(count) is not int or not (1 if is_runtime else 2) <= count < 0xffffffff:
                raise RuntimeError('Resource arrays require the native binding array profile with a bounded count')
            resource_type = resource_type.get('elementType', {})
        kind = resource_type['kind']
        size = 0
        if kind == 'constantBuffer':
            kind = 'UniformBuffer'
            size = resource_type['elementVarLayout']['binding']['size']
            if not 0 < size <= 16384:
                raise RuntimeError('Uniform occupied range exceeds the raster profile')
        elif stage == 'compute' and kind == 'resource' and resource_type.get('baseShape') == 'structuredBuffer':
            result = resource_type.get('resultType', {})
            scalar = result if result.get('kind') == 'scalar' else result.get('elementType', {})
            width = 1 if result.get('kind') == 'scalar' else result.get('elementCount')
            if result.get('kind') not in ('scalar', 'vector') or scalar.get('scalarType') not in ('float32', 'uint32', 'int32') or width not in (1, 2, 4):
                raise RuntimeError('Compute storage requires scalar32, vector2 or vector4 elements')
            if resource_type.get('access', 'read') not in ('read', 'readWrite'):
                raise RuntimeError('Unsupported compute storage access')
            kind = 'StorageReadWrite' if resource_type.get('access') == 'readWrite' else 'StorageRead'
            size = 4 * width
        elif kind == 'samplerState':
            if resource_type.get('isComparison', False) or 'comparison' in str(resource_type.get('flavor', '')).lower():
                raise RuntimeError('Comparison samplers are outside the raster profile')
            kind = 'Sampler'
        elif kind == 'resource' and resource_type.get('baseShape') in ('texture2D', 'textureCube', 'texture3D'):
            if resource_type.get('access', 'read') not in ('read', 'readOnly') or resource_type.get('multisample', False):
                raise RuntimeError('Raster textures require non-multisampled read-only sampling')
            result = resource_type.get('resultType', {})
            scalar = result.get('scalarType') if result.get('kind') == 'scalar' else result.get('elementType', {}).get('scalarType')
            scalar_aspect = result.get('kind') == 'scalar'
            if scalar != 'float32' and not (scalar_aspect and scalar == 'uint32'):
                raise RuntimeError('Raster textures require float vectors or scalar depth/stencil loads')
            shape = resource_type['baseShape']
            array = resource_type.get('array', False)
            if array and shape != 'texture2D':
                raise RuntimeError('Only 2D sampled texture arrays are in the R9 profile')
            kind = 'Texture2DArray' if array else {'texture2D': 'Texture2D', 'textureCube': 'TextureCube', 'texture3D': 'Texture3D'}[shape]
            if scalar_aspect:
                if array or shape != 'texture2D':
                    raise RuntimeError('Depth/stencil scalar loads require a 2D aspect view')
                kind = 'TextureDepth2D' if scalar == 'float32' else 'TextureStencil2D'
        else:
            raise RuntimeError('Unsupported raster resource shape: ' + kind)
        if stage == 'compute' and kind not in ('UniformBuffer', 'StorageRead', 'StorageReadWrite'):
            raise RuntimeError('Compute profile accepts buffer bindings only')
        if metal:
            native_kind = {'StorageRead': 'constantBuffer', 'StorageReadWrite': 'constantBuffer', 'UniformBuffer': 'constantBuffer', 'Texture2D': 'shaderResource', 'Texture2DArray': 'shaderResource', 'TextureCube': 'shaderResource', 'Texture3D': 'shaderResource', 'TextureDepth2D': 'shaderResource', 'TextureStencil2D': 'shaderResource', 'Sampler': 'samplerState'}[kind]
            native = [b for b in bindings if b.get('kind') == native_kind]
            if len(native) != 1 or native[0]['index'] != index:
                raise RuntimeError('Metal binding remapping is outside this raster profile')
        value = {'binding': index, 'kind': kind, 'size': size, 'name': resource['name']}
        if is_runtime and kind not in ('Texture2D', 'Texture2DArray', 'TextureCube', 'Texture3D'):
            raise RuntimeError('Runtime arrays require sampled color texture descriptors')
        if count > 1 or is_runtime:
            if kind in ('TextureDepth2D', 'TextureStencil2D'):
                raise RuntimeError('Depth/stencil descriptor arrays are outside this profile')
            value['count'] = count
        if is_runtime:
            value['runtime_sized'] = True
        if kind in ('StorageRead', 'StorageReadWrite'):
            value['scalar'] = scalar['scalarType']
            value['width'] = width
        if kind == 'UniformBuffer':
            value['offsets'] = {field['name']: field['binding']['offset'] for field in resource_type['elementType']['fields']}
            value['shapes'] = {field['name']: uniform_shape(field.get('type', {})) for field in resource_type['elementType']['fields']}
        entries.append(value)
    if sum(value.get('count', 1) for value in entries) >= 0xffffffff:
        raise RuntimeError('Native descriptor snapshot count cannot be represented')
    inputs = []
    def input_field(field):
        binding = field.get('binding', {})
        if 'semanticName' in field and field['semanticName'].startswith('SV_'):
            return
        ty = field['type']
        if ty['kind'] == 'struct':
            for child in ty['fields']:
                input_field(child)
            return
        if binding.get('kind') != 'varyingInput':
            raise RuntimeError('Unreflected vertex input')
        count = ty.get('elementCount')
        if ty['kind'] != 'vector' or count not in (2, 3, 4) or ty['elementType'].get('scalarType') != 'float32':
            raise RuntimeError('Vertex input lies outside float2/3/4 profile')
        location = binding['index']
        if not 0 <= location < 16 or any(x['location'] == location for x in inputs):
            raise RuntimeError('Vertex input location is duplicated or out of range')
        inputs.append({'location': location, 'format': 'Float' + str(count), 'name': field['name']})
    if stage == 'vertex':
        for parameter in point.get('parameters', []):
            input_field(parameter)
    result = {'entries': entries, 'inputs': inputs}
    if stage == 'compute':
        group = point.get('threadGroupSize')
        if not isinstance(group, list) or len(group) != 3 or any(type(x) is not int or x <= 0 for x in group) or group[0] * group[1] * group[2] > 256 or any(x > cap for x, cap in zip(group, (256, 256, 64))):
            raise RuntimeError('Compute local workgroup dimensions exceed the bounded profile')
        if sum(x['kind'].startswith('Storage') for x in entries) > RASTER_BINDING_COUNT:
            raise RuntimeError('Compute exceeds the implemented storage binding metadata capacity')
        result['workgroup'] = group
    return result


def glsl_interface(code, reflected, cross_reflection):
    from compile_shader import glsl_es_layout
    if not code.lstrip().startswith('#version 300 es') or re.search(r'\bvoid\s+main\s*\(', code) is None:
        raise RuntimeError('Raster GLSL ES needs a version 300 main entry')
    for token in ('gl_BaseVertex', 'gl_BaseInstance', 'GL_ARB_shader_draw_parameters'):
        if token in code:
            raise RuntimeError('Raster GLSL ES exceeds portable draw semantics')
    blocks = re.findall(r'(layout\(std140\)\s+uniform\s+(\w+)\s*\{[^}]+\}\s*\w*\s*;)', code)
    names = {}
    for entry in reflected['entries']:
        if entry['kind'] != 'UniformBuffer':
            continue
        resources = [x for x in cross_reflection.get('ubos', []) if x.get('binding') == entry['binding']]
        if len(resources) != 1:
            raise RuntimeError('GLSL ES uniform reflection is missing or ambiguous')
        name = resources[0]['name']
        matches = [block for block, block_name in blocks if block_name == name]
        if len(matches) != 1:
            raise RuntimeError('GLSL ES std140 block is missing')
        wrappers = '\n'.join(re.findall(r'struct\s+\w+\s*\{[^}]+\}\s*;', code))
        derived = glsl_es_layout(wrappers + '\n' + matches[0])
        # The parser derives this emitted target's occupied size independently.
        if derived['offsets'] != entry['offsets']:
            raise RuntimeError('GLSL ES emitted uniform offsets differ from target reflection')
        entry['size'] = derived['size']
        names[entry['binding']] = name
    images = cross_reflection.get('separate_images', [])
    samplers = cross_reflection.get('separate_samplers', [])
    textures = {}
    for shape, name in re.findall(r'uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(usampler2D|sampler2DArray|sampler2D|samplerCube|sampler3D)\s+(\w+)\s*;', code):
        pairs = [(image, sampler) for image in images for sampler in samplers
                 if name == 'SPIRV_Cross_Combined' + image['name'] + sampler['name']]
        if not pairs:
            pairs = [(image, {'binding': 0xffffffff}) for image in images
                     if name == 'SPIRV_Cross_Combined' + image['name'] + 'SPIRV_Cross_DummySampler']
        if len(pairs) != 1:
            raise RuntimeError('Cannot verify GLSL ES combined texture/sampler identity')
        image, sampler = pairs[0]
        entries = [x for x in reflected['entries'] if x['binding'] == image['binding']]
        expected = {'sampler2D': 'Texture2D', 'sampler2DArray': 'Texture2DArray', 'samplerCube': 'TextureCube', 'sampler3D': 'Texture3D', 'usampler2D': 'TextureStencil2D'}[shape]
        if len(entries) == 1 and entries[0]['kind'] == 'TextureDepth2D' and shape == 'sampler2D':
            expected = 'TextureDepth2D'
        if len(entries) != 1 or entries[0]['kind'] != expected:
            raise RuntimeError('GLSL ES emitted texture dimension differs from reflection')
        if image['binding'] in textures:
            raise RuntimeError('One texture with multiple samplers in one stage is outside this profile')
        textures[image['binding']] = {'name': name, 'sampler': sampler['binding']}
    return names, textures


def wgsl_uniform_type(code, kind, depth=0, *, compute=False):
    """Only scalar/vector/mat4 and bounded fixed vec4/mat4 storage shapes.

    Pinned Slang lowers std140 matrices to a single aligned vec4[4] field,
    and matrix arrays to a one-field array wrapper. Derive those emitted shapes
    instead of trusting reflected sizes; reject arbitrary nested structures.
    """
    kinds = {'f32': (4,4), 'vec2<f32>': (8,8), 'vec3<f32>': (12,16),
             'vec4<f32>': (16,16), 'mat4x4<f32>': (64,16)}
    # Compute uploads the uniform buffer's raw bytes on WebGPU; unlike WebGL
    # raster uploads it does not reinterpret fields through float uniform calls.
    # Keep integer admission local to compute and verify its emitted alignment.
    if compute:
        kinds.update({'i32': (4,4), 'u32': (4,4)})
        for scalar in ('i32', 'u32'):
            for width, size, alignment in ((2,8,8), (3,12,16), (4,16,16)):
                kinds[f'vec{width}<{scalar}>'] = (size, alignment)
    if kind in kinds:
        return kinds[kind]
    if depth > 3:
        raise RuntimeError('Browser raster uniform wrappers exceed bounded nesting')
    array = re.fullmatch(r'array<\s*(.+?)\s*,\s*(?:[iu]32\(\s*(\d+)\s*\)|(\d+))\s*>', kind)
    if array:
        element, cast_count, bare_count = array.groups()
        count = int(cast_count or bare_count)
        if not 1 <= count <= 1024:
            raise RuntimeError('Browser raster uniform array count exceeds bounded profile')
        allowed_arrays = ('vec4<f32>', 'mat4x4<f32>', 'vec4<i32>', 'vec4<u32>') if compute else ('vec4<f32>', 'mat4x4<f32>')
        if element not in allowed_arrays and not re.fullmatch(r'_MatrixStorage_float4x4_(?:ColMajor|RowMajor)std140_\d+', element):
            raise RuntimeError('Browser raster uniforms require bounded vec4/mat4 arrays')
        stride, alignment = wgsl_uniform_type(code, element, depth+1, compute=compute)
        if stride * count > 16384:
            raise RuntimeError('Browser raster uniform array exceeds 16384-byte profile')
        return stride * count, alignment
    matrix = re.fullmatch(r'_MatrixStorage_float4x4_(?:ColMajor|RowMajor)std140_\d+', kind)
    wrapped_array = re.fullmatch(r'_Array_std140_matrixx3Cfloatx2C4x2C4x3E\d+_\d+', kind)
    if matrix or wrapped_array:
        structures = re.findall(r'struct\s+' + re.escape(kind) + r'\s*\{([^}]+)\}', code)
        if len(structures) == 1:
            field = re.fullmatch(r'\s*@align\(16\)\s+data_\d+\s*:\s*(array<.+>)\s*,\s*;?\s*', structures[0])
            if field:
                inner = field[1]
                if matrix and re.fullmatch(r'array<vec4<f32>,\s*(?:[iu]32\(4\)|4)>', inner) is None:
                    raise RuntimeError('Browser raster matrix storage requires exactly four vec4 columns/rows')
                return wgsl_uniform_type(code, inner, depth+1, compute=compute)
    raise RuntimeError('Browser raster uniforms require float/vector/mat4 or bounded vec4/mat4 arrays')


def wgsl_contract(code, reflected, stage):
    """Verify emitted uniform packing and normalize bounded vertex input locations.

    Slang 2026.1.2 emits WGSL input locations differently from its JSON for some
    semantic combinations. Match every named float input in the selected entry's
    flat input struct, then use that target's reflected locations explicitly.
    Inter-stage outputs and builtins are never rewritten.
    """
    if stage == 'vertex' and reflected['inputs']:
        match = re.search(r'@vertex\s+fn\s+\w+\s*\(\s*\w+\s*:\s*(\w+)\s*\)', code)
        if match is None:
            raise RuntimeError('WGSL vertex input requires one reflected flat struct')
        structure = re.search(r'struct\s+' + re.escape(match[1]) + r'\s*\{([^}]+)\}', code)
        if structure is None:
            raise RuntimeError('WGSL vertex input struct is missing')
        seen = set()
        def location(field):
            original, name, width = field.groups()
            stem = re.sub(r'_\d+$', '', name)
            matches = [x for x in reflected['inputs'] if x['name'] == stem and x['format'] == 'Float' + width]
            if len(matches) != 1 or stem in seen:
                raise RuntimeError('WGSL emitted vertex input differs from reflection')
            seen.add(stem)
            return f"@location({matches[0]['location']}) {name} : vec{width}<f32>"
        body = re.sub(r'@location\((\d+)\)\s+(\w+)\s*:\s*vec([234])<f32>', location, structure[1])
        if seen != {x['name'] for x in reflected['inputs']} or len(re.findall(r'@location', body)) != len(seen):
            raise RuntimeError('WGSL vertex interface is incomplete')
        code = code[:structure.start(1)] + body + code[structure.end(1):]
    for entry in reflected['entries']:
        if entry['kind'].startswith('Texture'):
            shape = {'Texture2D': '2d', 'Texture2DArray': '2d_array', 'TextureCube': 'cube', 'Texture3D': '3d', 'TextureDepth2D': '2d', 'TextureStencil2D': '2d'}[entry['kind']]
            scalar = 'u32' if entry['kind'] == 'TextureStencil2D' else 'f32'
            declaration = re.search(r'@binding\(' + str(entry['binding']) + r'\)\s+@group\(0\)\s+var\s+\w+\s*:\s*texture_' + shape + r'<' + scalar + r'>\s*;', code)
            if declaration is None:
                raise RuntimeError('WGSL emitted texture dimension differs from reflection')
            if entry['kind'] in ('TextureDepth2D', 'TextureStencil2D'):
                variable = re.search(r'var\s+(\w+)\s*:', declaration[0])[1]
                if re.search(r'\btexture(?:Sample|Gather)\w*\s*\(\s*' + re.escape(variable) + r'\b', code):
                    raise RuntimeError('Depth/stencil views require unfiltered loads')

        if entry['kind'] != 'UniformBuffer':
            continue
        declaration = re.search(r'@binding\(' + str(entry['binding']) + r'\)\s+@group\(0\)\s+var<uniform>\s+\w+\s*:\s*(\w+)\s*;', code)
        if declaration is None:
            raise RuntimeError('WGSL emitted uniform binding is missing')
        structure = re.search(r'struct\s+' + re.escape(declaration[1]) + r'\s*\{([^}]+)\}', code)
        if structure is None:
            raise RuntimeError('WGSL emitted uniform struct is missing')
        occupied, alignment, offsets = 0, 1, {}
        # Commas inside array<T,N> are not field separators. Reject nested
        # structures/runtime arrays; fixed vec4/mat4 arrays have verified strides.
        members, start, depth = [], 0, 0
        for index, char in enumerate(structure[1]):
            depth += (char == '<') - (char == '>')
            if depth < 0:
                raise RuntimeError('WGSL uniform type brackets are malformed')
            if char == ',' and depth == 0:
                members.append(structure[1][start:index]); start = index + 1
        members.append(structure[1][start:])
        for member in members:
            if not member.strip():
                continue
            field = re.fullmatch(r'\s*(?:@align\((\d+)\)\s*)?(\w+)\s*:\s*(.+?)\s*', member)
            if field is None:
                raise RuntimeError('Browser raster uniform member is malformed')
            explicit, name, kind = field.groups()
            size, natural = wgsl_uniform_type(code, kind, compute=stage == 'compute')
            align = max(int(explicit or 1), natural)
            if align & (align-1):
                raise RuntimeError('WGSL uniform alignment is invalid')
            alignment = max(alignment, align)
            occupied = (occupied+align-1)//align*align
            stem = re.sub(r'_\d+$', '', name)
            if stem in offsets:
                raise RuntimeError('Duplicate WGSL uniform member')
            offsets[stem] = occupied
            occupied += size
        size = (occupied+alignment-1)//alignment*alignment
        if size != entry['size'] or offsets != entry['offsets']:
            raise RuntimeError('WGSL emitted uniform packing differs from target reflection')
    return code


def glsl_varyings(code, reflection, stage):
    """ES 3.00 links varying identifiers rather than Vulkan location decorations."""
    direction = 'outputs' if stage == 'vertex' else 'inputs'
    qualifier = 'out' if stage == 'vertex' else 'in'
    interface = []
    for value in reflection.get(direction, []):
        location, kind = value.get('location'), value.get('type')
        if not isinstance(location, int) or kind not in ('float', 'vec2', 'vec3', 'vec4') or value.get('array'):
            raise RuntimeError('GLSL ES varyings require bounded scalar/vector locations')
        old = re.sub(r'[^A-Za-z_0-9]', '_', value['name'])
        new = f'ludusVarying{location}'
        if re.search(r'\b' + qualifier + r'\s+(?:highp\s+|mediump\s+|lowp\s+)?' + kind + r'\s+' + re.escape(old) + r'\s*;', code) is None:
            raise RuntimeError('GLSL ES emitted varying differs from location reflection')
        if old != new and re.search(r'\b' + new + r'\b', code):
            raise RuntimeError('GLSL ES canonical varying identifier conflicts with source')
        code = re.sub(r'\b' + re.escape(old) + r'\b', new, code)
        interface.append((location, kind))
    return code, sorted(interface)


def verify_varyings(vertex, fragment):
    """A fragment may consume a typed subset of vertex outputs (e.g. shadows)."""
    if len(dict(vertex)) != len(vertex) or len(dict(fragment)) != len(fragment):
        raise RuntimeError('GLSL ES raster stage varying locations repeat')
    outputs = dict(vertex)
    if any(outputs.get(location) != kind for location, kind in fragment):
        raise RuntimeError('GLSL ES raster stage varying interfaces differ')


def spirv_array_indexing(data, reflected):
    """Derive array access modes from the emitted target, rather than source spelling.

    Thanks to Khronos Group, SPIR-V Specification, OpAccessChain and NonUniform,
    https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html . The admitted
    profile rejects unknown descriptor pointer paths instead of guessing safety.
    """
    # SPIR-V / GLSL.std.450 enumerants used by this deliberately bounded verifier.
    op_ext_import, op_ext_inst, op_type_int = 11, 12, 21
    op_type_pointer, op_constant, op_variable = 32, 43, 59
    op_type_vector, op_type_struct, op_member_decorate = 23, 30, 72
    op_integer_subtract = 130
    op_access_chain, op_in_bounds_access_chain = 65, 66
    op_decorate, op_copy_object = 71, 83
    decoration_binding, decoration_non_uniform = 33, 5300
    glsl_unsigned_min, glsl_unsigned_clamp = 38, 44
    op_function, op_function_parameter, op_function_call = 54, 55, 57
    op_load, op_pointer_access_chain = 61, 67
    op_composite_construct, op_composite_extract, op_composite_insert = 80, 81, 82
    op_bitcast, op_select, op_phi = 124, 169, 245
    pointer_forward_operations = {op_function, op_function_parameter, op_function_call, op_load,
                                  op_access_chain, op_in_bounds_access_chain, op_pointer_access_chain,
                                  op_composite_construct, op_composite_extract, op_composite_insert,
                                  op_copy_object, op_bitcast, op_select, op_phi}
    if len(data) % 4 or len(data) < 20:
        raise RuntimeError('Malformed SPIR-V array target')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise RuntimeError('Invalid SPIR-V array target')
    instructions = []
    position = 5
    while position < len(words):
        length, operation = words[position] >> 16, words[position] & 65535
        if length == 0 or position + length > len(words):
            raise RuntimeError('Malformed SPIR-V array instruction')
        instructions.append((operation, words[position + 1:position + length]))
        position += length
    bindings, constants, non_uniform, integer_types, definitions, standard_sets = {}, {}, set(), {}, {}, set()
    for operation, values in instructions:
        if operation == op_decorate and len(values) >= 2:
            if values[1] == decoration_binding and len(values) == 3:
                bindings[values[0]] = values[2]
            if values[1] == decoration_non_uniform:
                non_uniform.add(values[0])
        if operation == op_type_int and len(values) == 3:
            integer_types[values[0]] = (values[1], values[2])
        if operation == op_ext_import and len(values) >= 2:
            name = struct.pack('<' + 'I' * (len(values) - 1), *values[1:]).split(b'\0', 1)[0]
            if name == b'GLSL.std.450':
                standard_sets.add(values[0])
        if operation == op_constant and len(values) == 3:
            constants[values[1]] = (values[0], values[2])
        if operation in (op_ext_inst, op_copy_object, op_load, op_access_chain, op_in_bounds_access_chain, op_composite_extract, op_integer_subtract) and len(values) >= 3:
            definitions[values[1]] = (operation, values)
    def is_length_pointer(identifier):
        operation, values = definitions.get(identifier, (0, ()))
        return (operation in (op_access_chain, op_in_bounds_access_chain) and len(values) == 5
                and values[2] == control_variable
                and all(constants.get(index, (None, None))[1] == 0 for index in values[3:]))

    def is_length_word(identifier):
        operation, values = definitions.get(identifier, (0, ()))
        if operation == op_load and len(values) == 3 and integer_types.get(values[0]) == (32, 0):
            return is_length_pointer(values[2])
        # Some targets load the whole uint4 then extract x.
        if operation == op_composite_extract and len(values) == 4 and values[3] == 0 and integer_types.get(values[0]) == (32, 0):
            load_operation, load_values = definitions.get(values[2], (0, ()))
            pointer_operation, pointer_values = definitions.get(load_values[2], (0, ())) if load_operation == op_load and len(load_values) == 3 else (0, ())
            return (pointer_operation in (op_access_chain, op_in_bounds_access_chain) and len(pointer_values) == 4
                    and pointer_values[2] == control_variable
                    and constants.get(pointer_values[3], (None, None))[1] == 0)
        return False

    def is_bounded(identifier, count, visited=None, runtime=False):
        visited = set() if visited is None else visited
        if identifier in visited:
            return False
        visited.add(identifier)
        if identifier in constants:
            kind, value = constants[identifier]
            return integer_types.get(kind) in ((32, 0), (32, 1)) and value < count
        operation, values = definitions.get(identifier, (0, ()))
        if runtime and operation == op_integer_subtract and len(values) == 4 and integer_types.get(values[0]) == (32, 0):
            return is_length_word(values[2]) and constants.get(values[3], (None, None))[1] == 1
        if operation == op_copy_object and integer_types.get(values[0]) == (32, 0):
            return is_bounded(values[2], count, visited, runtime)
        if operation == op_ext_inst and len(values) >= 4 and integer_types.get(values[0]) == (32, 0) and values[2] in standard_sets:
            if values[3] == glsl_unsigned_min and len(values) == 6:  # unsigned min
                return any(is_bounded(value, count, visited.copy(), runtime) for value in values[4:])
            if values[3] == glsl_unsigned_clamp and len(values) == 7:  # unsigned clamp
                lower, upper = constants.get(values[5]), constants.get(values[6])
                return lower is not None and upper is not None and integer_types.get(lower[0]) == (32, 0) and integer_types.get(upper[0]) == (32, 0) and lower[1] <= upper[1] < count
        return False
    pointer_types = {values[0]: values[2] for operation, values in instructions
                     if operation == op_type_pointer and len(values) == 3}
    variables = {values[1]: values[0] for operation, values in instructions if operation == op_variable and len(values) >= 3}
    control_variable = None
    if any(entry.get('runtime_sized', False) for entry in reflected['entries']):
        controls = [entry for entry in reflected['entries'] if entry['binding'] == RUNTIME_BINDING_LENGTH_SLOT]
        if len(controls) > 1 or (controls and (controls[0]['kind'] != 'UniformBuffer' or controls[0]['size'] != 16)):
            raise RuntimeError('Runtime descriptor arrays require the RHI-owned uint4 length record')
        if controls:
            control_variables = [identifier for identifier, binding in bindings.items()
                                 if binding == RUNTIME_BINDING_LENGTH_SLOT and identifier in variables]
            vectors = {values[0]: (values[1], values[2]) for operation, values in instructions
                       if operation == op_type_vector and len(values) == 3}
            structures = {values[0]: values[1:] for operation, values in instructions
                          if operation == op_type_struct and len(values) >= 1}
            offsets = {(values[0], values[1]): values[3] for operation, values in instructions
                       if operation == op_member_decorate and len(values) == 4 and values[2] == 35}
            if len(control_variables) != 1:
                raise RuntimeError('Runtime array length must have one immutable control binding')
            control_variable = control_variables[0]
            structure = pointer_types.get(variables[control_variable])
            fields = structures.get(structure, ())
            vector = vectors.get(fields[0]) if len(fields) == 1 else None
            if vector is None or vector[1] != 4 or integer_types.get(vector[0]) != (32, 0) or offsets.get((structure, 0)) != 0:
                raise RuntimeError('Runtime length target differs from the RHI-owned uint4 ABI')
    arrays = {entry['binding']: entry for entry in reflected['entries'] if entry.get('count', 1) > 1 or entry.get('runtime_sized', False)}
    for entry in arrays.values():
        entry['indexing'] = 'Constant'
    array_types = {pointer_types[variables[identifier]] for identifier, binding in bindings.items()
                   if binding in arrays and identifier in variables and variables[identifier] in pointer_types}
    # Whole descriptor-array loads and local copies would create another access
    # path. Reject both value and pointer forwarding, including a different storage class.
    protected_types = array_types | {pointer for pointer, element in pointer_types.items() if element in array_types}
    for operation, values in instructions:
        if operation in pointer_forward_operations and len(values) >= 2 and values[0] in protected_types:
            raise RuntimeError('Native descriptor array pointer forwarding is outside the verified profile')
    accessed = set()
    for operation, values in instructions:
        if operation in (op_access_chain, op_in_bounds_access_chain) and len(values) >= 4 and bindings.get(values[2]) in arrays:
            binding = bindings[values[2]]
            entry = arrays[binding]
            accessed.add(binding)
            if is_bounded(values[3], entry['count'], runtime=entry.get('runtime_sized', False)) is False:
                raise RuntimeError('Native descriptor index lacks a verified fixed-array bound; use unsigned min/clamp')
            # A resource array is one-dimensional in this profile. The first
            # pointer index selects its descriptor, independently of texture layers.
            mode = 'NonUniform' if values[1] in non_uniform or values[3] in non_uniform else 'Uniform' if values[3] not in constants else 'Constant'
            if ('Constant', 'Uniform', 'NonUniform').index(mode) > ('Constant', 'Uniform', 'NonUniform').index(entry['indexing']):
                entry['indexing'] = mode
            entry['verified_bounds'] = True
    if accessed != arrays.keys():
        raise RuntimeError('Native array access path is not independently reflected')


def verify_native_metal_arrays(code, interface):
    """Check emitted MSL resource register/count lowering independently."""
    for binding in interface['entries']:
        index = binding['binding']
        count = binding.get('count', 1)
        if count > 1:
            if binding['kind'] == 'Sampler':
                element, register = r'sampler', 'sampler'
            elif binding['kind'] in ('Texture2D', 'Texture2DArray', 'TextureCube', 'Texture3D'):
                shape = {'Texture2D': 'texture2d', 'Texture2DArray': 'texture2d_array', 'TextureCube': 'texturecube', 'Texture3D': 'texture3d'}[binding['kind']]
                element, register = shape + r'<\s*float\s*>', 'texture'
            else:
                raise RuntimeError('Metal buffer descriptor arrays are outside this profile')
            pattern = r'array<\s*' + element + r'\s*,\s*' + str(count) + r'\s*>\s+\w+\s*\[\[' + register + r'\(' + str(index) + r'\)\]\]'
            if re.search(pattern, code) is None:
                raise RuntimeError('MSL array count/register differs from target reflection')
        else:
            kind = 'buffer' if binding['kind'] == 'UniformBuffer' else 'sampler' if binding['kind'] == 'Sampler' else 'texture'
            if '[[' + kind + '(' + str(index) + ')]]' not in code:
                raise RuntimeError('MSL scalar register differs from target reflection')


def compile_raster(args, lock):
    from compile_shader import entry, run
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    extra = [part for path in args.include for part in ('-I', path)] + ['-D' + value for value in args.define]
    roots = [Path(args.source).parent, *args.include]
    commands, dependencies, artifacts, interfaces = [], [], {}, {}
    native_compilations = []
    stages = [('compute', args.compute)] if getattr(args, 'compute', None) else [('vertex', args.vertex), ('fragment', args.fragment)]
    compute = stages[0][0] == 'compute'
    native_arrays = getattr(args, 'native_binding_arrays', False)
    runtime_arrays = getattr(args, 'runtime_binding_arrays', False)
    if runtime_arrays and (not native_arrays or args.metal or compute):
        raise RuntimeError('Runtime descriptor arrays require the native Vulkan raster profile')
    if runtime_arrays:
        extra += ['-DLUDUS_RUNTIME_BINDING_LENGTH_SLOT=' + str(RUNTIME_BINDING_LENGTH_SLOT),
                  '-DLUDUS_RUNTIME_BINDING_ARRAY_SLOT=' + str(RUNTIME_BINDING_ARRAY_SLOT)]
    if native_arrays and args.metal and (compute or args.spirv_cross is None):
        raise RuntimeError('Metal native arrays require raster stages and the pinned SPIRV-Cross tool')
    if not args.metal:
        if not args.validator or hashlib.sha256(Path(args.validator).read_bytes()).hexdigest() != lock['spirv_tools']['val_sha256']:
            raise RuntimeError('Raster SPIR-V validator differs from SDK pin')
    if args.spirv_cross and not compute:
        pin = json.loads(Path(args.spirv_cross_lock).read_text())['spirv_cross']
        tool = Path(args.spirv_cross).resolve()
        build = json.loads(Path(str(tool) + '.build.json').read_text())
        if any(build.get(key) != pin[key] for key in ('tag', 'commit', 'source_sha256')) or build.get('binary_sha256') != hashlib.sha256(tool.read_bytes()).hexdigest():
            raise RuntimeError('Raster SPIRV-Cross identity differs from verified pin')
    target = 'metal' if args.metal and not native_arrays else 'spirv'
    for stage, name in stages:
        artifact = output / f'{args.name}.{stage}.{"metal" if target == "metal" else "spv"}'
        reflection, depfile = output / f'{stage}.reflection.json', output / f'{stage}.d'
        command = [args.compiler, args.source, '-target', target, '-profile', 'sm_6_0' if target == 'metal' else 'spirv_1_3',
                   '-entry', name, '-stage', stage, '-o', str(artifact), '-reflection-json', str(reflection), '-depfile', str(depfile), *extra]
        if getattr(args, 'worker_socket', None):
            from native_worker import compile_raster_stage
            native_compilations.append(compile_raster_stage(args.worker_socket, args, lock, stage, name, target, artifact, reflection, depfile, extra))
        else:
            commands.append(command)
            run(command)
        dependencies.append(depfile.read_text().split(':', 1)[1].strip())
        interfaces[(target, stage)] = raster_interface(json.loads(reflection.read_text()), stage, target == 'metal', native_arrays, runtime_arrays)
        if native_arrays:
            spirv_array_indexing(artifact.read_bytes(), interfaces[(target, stage)])
            for binding in interfaces[(target, stage)]['entries']:
                if binding.get('kind') in ('UniformBuffer', 'StorageRead', 'StorageReadWrite', 'Sampler') and binding.get('indexing', 'Constant') != 'Constant':
                    raise RuntimeError('Buffer and sampler descriptor arrays require constant selection')
        if args.metal and native_arrays:
            emitted = output / f'{args.name}.{stage}.metal'
            command = [args.spirv_cross, str(artifact), '--msl', '--msl-version', '24000', '--msl-decoration-binding', '--output', str(emitted)]
            commands.append(command); run(command)
            code = relative_line_directives(emitted.read_text(), roots)
            emitted.write_text(code)
            interface = interfaces[(target, stage)]
            verify_native_metal_arrays(code, interface)
            names = re.findall(r'\b' + stage + r'\s+\w+\s+(\w+)\s*\(', code)
            if len(names) != 1:
                raise RuntimeError('Expected one emitted native array Metal entry')
            artifacts[('metal', stage)] = (code, names[0])
            interfaces[('metal', stage)] = interface
        elif args.metal:
            code = relative_line_directives(artifact.read_text(), roots)
            for binding in interfaces[(target, stage)]['entries']:
                if binding['kind'] in ('TextureDepth2D', 'TextureStencil2D'):
                    variable = re.escape(binding['name']) + r'(?:_\d+)?'
                    if re.search(r'\b' + variable + r'\s*\)?\s*\.\s*(?:sample|gather)\b', code):
                        raise RuntimeError('Depth/stencil views require unfiltered loads')
            if compute:
                # Slang emits non-const device pointers for StructuredBuffer.
                # Preserve the independently reflected read-only contract in MSL,
                # including the compiler's context member and entry argument.
                for binding in interfaces[(target, stage)]['entries']:
                    if binding['kind'] != 'StorageRead':
                        continue
                    scalar = {'float32': 'float', 'uint32': 'uint', 'int32': 'int'}[binding['scalar']]
                    element = scalar if binding['width'] == 1 else scalar + str(binding['width'])
                    pattern = r'(?<!const )\b' + element + r'\s+device\s*\*\s+' + re.escape(binding['name']) + r'(?:_\d+)?\b'
                    code, count = re.subn(pattern, lambda match: 'const ' + match[0], code)
                    if count < 1:
                        raise RuntimeError('Metal emitted read-only storage parameter is missing')
            artifact.write_text(code)
            names = re.findall(r'\[\[' + ('kernel' if compute else stage) + r'\]\]\s+\w+\s+(\w+)\s*\(', code)
            if len(names) != 1:
                raise RuntimeError('Expected one emitted Metal raster entry')
            artifacts[(target, stage)] = (code, names[0])
        else:
            run([args.validator, '--target-env', 'vulkan1.1', str(artifact)])
            artifacts[(target, stage)] = (artifact.read_bytes(), entry(artifact))
            if args.spirv_cross and not compute and not native_arrays:
                essl = output / f'{args.name}.{stage}.essl'
                command = [args.spirv_cross, str(artifact), '--version', '300', '--es', '--fixup-clipspace', '--output', str(essl)]
                commands.append(command); run(command)
                cross = json.loads(run([args.spirv_cross, str(artifact), '--reflect']))
                interface = raster_interface(json.loads(reflection.read_text()), stage)
                code, varyings = glsl_varyings(essl.read_text(), cross, stage)
                essl.write_text(code)
                interface['varyings'] = varyings
                names, textures = glsl_interface(code, interface, cross)
                interface['blocks'], interface['textures'] = names, textures
                interfaces[('glsl', stage)] = interface
                artifacts[('glsl', stage)] = (essl.read_text(), 'main')
    if not args.metal and not native_arrays:
        for stage, source_entry in stages:
            artifact = output / f'{args.name}.{stage}.wgsl'
            reflection, depfile = output / f'{stage}.wgsl.reflection.json', output / f'{stage}.wgsl.d'
            command = [args.compiler, args.source, '-target', 'wgsl', '-profile', 'sm_6_0', '-entry', source_entry,
                       '-stage', stage, '-o', str(artifact), '-reflection-json', str(reflection), '-depfile', str(depfile), *extra]
            if getattr(args, 'worker_socket', None):
                from native_worker import compile_raster_stage
                native_compilations.append(compile_raster_stage(args.worker_socket, args, lock, stage, source_entry, 'wgsl', artifact, reflection, depfile, extra))
            else:
                commands.append(command)
                run(command)
            dependencies.append(depfile.read_text().split(':', 1)[1].strip())
            interface = raster_interface(json.loads(reflection.read_text()), stage)
            code = wgsl_contract(artifact.read_text(), interface, stage)
            if compute:
                local = re.search(r'@workgroup_size\((\d+),\s*(\d+),\s*(\d+)\)', code)
                if local is None or [int(x) for x in local.groups()] != interface['workgroup']:
                    raise RuntimeError('WGSL emitted workgroup differs from reflection')
                for binding in interface['entries']:
                    if not binding['kind'].startswith('Storage'):
                        continue
                    scalar = {'float32': 'f32', 'uint32': 'u32', 'int32': 'i32'}[binding['scalar']]
                    element = scalar if binding['width'] == 1 else f'vec{binding["width"]}<{scalar}>'
                    access = 'read' if binding['kind'] == 'StorageRead' else 'read_write'
                    pattern = r'@binding\(' + str(binding['binding']) + r'\)\s+@group\(0\)\s+var<storage,\s*' + access + r'>\s+\w+\s*:\s*array<' + re.escape(element) + r'>\s*;'
                    if re.search(pattern, code) is None:
                        raise RuntimeError('WGSL emitted storage packing/access differs from reflection')
            names = re.findall(r'@' + stage + (r'\s+@workgroup_size\([^)]*\)' if compute else '') + r'\s+fn\s+(\w+)', code)
            if len(names) != 1:
                raise RuntimeError('Expected one emitted WGSL raster entry')
            artifact.write_text(code)
            artifacts[('wgsl', stage)] = (code, names[0])
            interfaces[('wgsl', stage)] = interface
    if args.spirv_cross and not compute and not native_arrays:
        verify_varyings(interfaces[('glsl', 'vertex')]['varyings'], interfaces[('glsl', 'fragment')]['varyings'])
    companion = ''
    header = '#pragma once\n#include <ludus/foundation/base/config.h>\n#include <ludus/graphics/rhi/raster.h>\n'
    header += f'namespace ludus::shaders::{args.name} {{\n'
    for (backend, stage), (data, name) in artifacts.items():
        label = backend.upper() + '_' + stage.upper()
        interface = interfaces[(backend, stage)]
        if isinstance(data, bytes):
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            header += f'inline constexpr foundation::uint32 {label}[] = {{' + ','.join(f'0x{x:08x}U' for x in words) + '};\n'
        else:
            delimiter = 'R_' + label
            if f'){delimiter}"' in data:
                raise RuntimeError('Raster source conflicts with raw string delimiter')
            if compute and len(data.encode("utf-8")) >= 60000:
                declaration, definition = external_text_source(args.name, label, data)
                header += declaration
                companion += definition
            else:
                header += f'inline constexpr char {label}[] = R"{delimiter}(' + data + f'){delimiter}";\n'
        if interface['entries']:
            header += f'inline constexpr graphics::rhi::RasterBinding {label}_BINDINGS[] = {{\n'
            for binding in interface['entries']:
                header += '{' + f'.Binding={binding["binding"]},.Kind=graphics::rhi::RasterBindingKind::{binding["kind"]},.Visibility=graphics::rhi::RasterVisibility::{stage.title()},.MinSize={binding["size"]},.Count={binding.get("count",1)},.Indexing=graphics::rhi::RasterBindingIndexing::{binding.get("indexing","Constant")},.IsRuntimeSized={str(binding.get("runtime_sized",False)).lower()}' + '},\n'
            header += '};\n'
        members = [(entry['binding'], name, offset, entry['shapes'][name]) for entry in interface['entries']
                   for name, offset in entry.get('offsets', {}).items()]
        if len(members) > 64:
            raise RuntimeError('Uniform schema exceeds 64 top-level members')
        if members:
            header += f'inline constexpr graphics::rhi::RasterUniformMember {label}_MEMBERS[] = {{\n'
            for binding, member, offset, shape in members:
                if re.fullmatch(r'[A-Za-z_]\w{0,62}', member) is None:
                    raise RuntimeError('Uniform member name exceeds the schema profile')
                header += '{' + f'{binding},"{member}",{offset},graphics::rhi::RasterUniformShape::{shape}' + '},\n'
            header += '};\n'
        if interface['inputs']:
            header += f'inline constexpr graphics::rhi::RasterShaderInput {label}_INPUTS[] = {{\n'
            for value in interface['inputs']:
                header += '{' + f'{value["location"]},graphics::rhi::RasterVertexFormat::{value["format"]}' + '},\n'
            header += '};\n'
        header += f'inline graphics::rhi::RasterShaderDescription {backend.title()}{stage.title()}() noexcept {{\n graphics::rhi::RasterShaderDescription result;\n'
        header += f' result.Artifact.Stage = graphics::rhi::ShaderStage::{stage.title()};\n'
        field = {'metal': 'Msl', 'spirv': 'Spirv', 'wgsl': 'Wgsl', 'glsl': 'GlslEs'}[backend]
        header += f' result.Artifact.{field} = {label}; result.Artifact.{field}Entry = "{name}";\n'
        if interface['entries']:
            header += f' result.Bindings = {label}_BINDINGS;\n'
        if members:
            header += f' result.UniformMembers = {label}_MEMBERS;\n'
        if interface['inputs']:
            header += f' result.Inputs = {label}_INPUTS;\n'
        for binding, block in interface.get('blocks', {}).items():
            header += f' result.UniformBlocks[{binding}] = "{block}";\n'
        for binding, texture in interface.get('textures', {}).items():
            header += f' result.TextureNames[{binding}] = "{texture["name"]}"; result.TextureSamplers[{binding}] = {texture["sampler"]};\n'
        for axis, size in enumerate(interface.get('workgroup', [])):
            header += f' result.WorkgroupSize[{axis}] = {size};\n'
        if any(entry.get('count', 1) > 1 or entry.get('runtime_sized', False) for entry in interface['entries']):
            header += ' result.HasVerifiedArrayBounds = true;\n'
        header += ' return result;\n}\n'
    for stage, _ in stages:
        if args.metal:
            header += f'inline auto {stage.title()}() noexcept {{ return Metal{stage.title()}(); }}\n'
        elif native_arrays:
            header += f'inline graphics::rhi::RasterShaderDescription {stage.title()}() noexcept {{ return Spirv{stage.title()}(); }}\n'
        else:
            header += f'inline auto {stage.title()}() noexcept {{\n#if defined(LUDUS_PLATFORM_WEB)\n auto result = Wgsl{stage.title()}();\n'
            if args.spirv_cross and not compute:
                header += f' auto glsl = Glsl{stage.title()}(); result.Artifact.GlslEs = glsl.Artifact.GlslEs; result.Artifact.GlslEsEntry = glsl.Artifact.GlslEsEntry;\n'
                # Web source/metadata is selected after startup, so packing is
                # verified equal here; native target packing remains independent.
                a, b = interfaces[('wgsl', stage)], interfaces[('glsl', stage)]
                if [(x['binding'], x['kind'], x['size'], x.get('offsets')) for x in a['entries']] != [(x['binding'], x['kind'], x['size'], x.get('offsets')) for x in b['entries']] or a['inputs'] != b['inputs']:
                    raise RuntimeError('Browser raster uniform/layout packing differs between WGSL and GLSL ES')
                header += ' for (foundation::usize i = 0; i < graphics::rhi::RASTER_BINDING_COUNT; ++i) { result.UniformBlocks[i] = glsl.UniformBlocks[i]; result.TextureNames[i] = glsl.TextureNames[i]; result.TextureSamplers[i] = glsl.TextureSamplers[i]; }\n'
            header += f' return result;\n#else\n return Spirv{stage.title()}();\n#endif\n}}\n'
    header += '}\n'
    manifest = {'compiler': lock['slang'], 'commands': commands, 'native_compilations': native_compilations, 'interfaces': {backend + '_' + stage: info for (backend, stage), info in interfaces.items()},
                'sha256': {backend + '_' + stage: hashlib.sha256(data if isinstance(data, bytes) else data.encode()).hexdigest() for (backend, stage), (data, _) in artifacts.items()},
                'layout': 'See separate target reflection JSON. CPU packing must match the selected target; no native cross-target equality is assumed.'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    dest = output / f'{args.name}.h'
    escaped = str(dest).replace(' ', '\\ ').replace('#', '\\#')
    (output / 'shader.d').write_text(escaped + ': ' + ' '.join(dependencies) + '\n')
    dest.write_text(header)
    if compute:
        (output / f"{args.name}.cpp").write_text(companion or "// No external text artifacts in this profile.\n")
