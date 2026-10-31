"""Legalize the pinned Slang Metal ray-stage ABI, with independently checked bindings.

Slang emits shared authored functions but has no Metal ray-stage entry support.
Two reserved structured-buffer token types stand for native function tables;
this adapter replaces only those types, attaches native stage visibility and
supplies Metal intersector operations. Unknown compiler shapes fail closed.

Thanks to Apple, WWDC20 sessions 10012 (triangle intersection functions, 12:16)
and 10013 (visible function tables), and WWDC22 session 10105 (call stack depth):
https://developer.apple.com/videos/play/wwdc2020/10012/
https://developer.apple.com/videos/play/wwdc2020/10013/
https://developer.apple.com/videos/play/wwdc2022/10105/
"""
from copy import deepcopy
from pathlib import Path
import re


def _require(condition, message):
    if not condition:
        raise RuntimeError('Native Metal ray ABI: ' + message)


def _one(pattern, code, description, flags=0):
    matches = list(re.finditer(pattern, code, flags))
    _require(len(matches) == 1, description + ' is missing or ambiguous')
    return matches[0]


def _mask(code):
    return re.sub(r'/\*.*?\*/|//[^\n]*|^[ \t]*#[^\n]*',
                  lambda match: re.sub(r'[^\n]', ' ', match[0]), code, flags=re.S | re.M)


def _close(code, start, opening='{', closing='}'):
    depth = 0
    for index in range(start, len(code)):
        depth += code[index] == opening
        depth -= code[index] == closing
        if depth == 0:
            return index
    raise RuntimeError('Native Metal ray ABI: unmatched delimiter')


def _arguments(signature):
    result, start, depth = [], 0, 0
    for index, character in enumerate(signature):
        if character in '([<':
            depth += 1
        elif character in ')]>':
            depth -= 1
        elif character == ',' and depth == 0:
            result.append(signature[start:index].strip())
            start = index + 1
    result.append(signature[start:].strip())
    return result


def _abi(path):
    code = Path(path).read_text()
    result = dict(re.findall(r'^#define\s+(LUDUS_METAL_RAY_\w+)\s+(\d+|"[^"]+")\s*$', code, re.M))
    return {key: int(value) if value.isdigit() else value[1:-1] for key, value in result.items()}


def legalize(code, document, layout, storage_bindings=None):
    """Return native MSL and normalized public reflection; no compiled source is guessed."""
    abi = _abi(layout)
    slots = {'Parameters': 0, 'sceneWords': 1, 'accumulation': 2, 'colors': 3, 'textureWords': 4,
             'rayScene': 5, 'GraphBuffer0': 6, 'GraphBuffer1': 7,
             'GraphVisibility': 8, 'GraphVisibilityDepth': 9, 'rayPathCapture': 10, 'graphFailures': 14,
             'NativeRayFunctions': abi['LUDUS_METAL_RAY_FUNCTION_TABLE_BINDING'],
             'NativeRayIntersections': abi['LUDUS_METAL_RAY_INTERSECTION_TABLE_BINDING']}
    storage_bindings = storage_bindings or {}
    _require(type(storage_bindings) is dict and len(storage_bindings) <= 2 and
             len(set(storage_bindings.values())) == len(storage_bindings), 'invalid private storage slots')
    for name, slot in storage_bindings.items():
        _require(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name) is not None and
                 name not in slots and type(slot) is int and 6 <= slot <= 7, 'invalid private storage ABI')
        slots[name] = slot
    private = {'NativeRayFunctions', 'NativeRayIntersections'}
    masked = _mask(code)
    context = _one(r'\bstruct\s+(KernelContext_\d+)\s*\{', masked, 'resource context')[1]
    packet = _one(r'\bstruct\s+(NativeRayStagePacket_\d+)\s*\{', masked, 'stage packet')[1]
    entry = _one(r'\[\[kernel\]\]\s+void\s+(\w+)\s*\((.*?)\)\s*\{', code, 'compute encoder entry', re.S)
    body_end = _close(masked, entry.end() - 1)
    entry_body = code[entry.end():body_end]
    assignments = dict((argument, member) for member, argument in re.findall(
        r'\(&\w+\)->(\w+)\s*=\s*(\w+)\s*;', entry_body))
    resources = {item['name']: item for item in document['parameters']}
    _require(set(resources) <= set(slots) and private <= set(resources), 'unrecognized resource interface')
    arguments, selected = [], {}
    for argument in _arguments(entry[2]):
        match = re.search(r'\b(\w+)\s*(?:\[\[.*\]\])?\s*$', argument)
        _require(match is not None, 'unrecognized encoder parameter')
        emitted = match[1]
        name = re.sub(r'_\d+$', '', emitted)
        if name not in resources:
            _require('[[thread_position_in_grid]]' in argument, 'unrecognized system parameter')
            arguments.append(argument)
            continue
        _require(emitted in assignments, 'resource context assignment is missing')
        category = 'texture' if resources[name]['type'].get('baseShape') == 'texture2D' else 'buffer'
        argument = re.sub(r'\s*\[\[(?:buffer|texture)\(\d+\)\]\]', '', argument)
        argument += f' [[{category}({slots[name]})]]'
        selected[name] = (emitted, assignments[emitted], argument)
        arguments.append(argument)
    _require(set(selected) == set(resources), 'resource argument/reflection mismatch')
    code = code[:entry.start(2)] + ', '.join(arguments) + code[entry.end(2):]
    for token, native_type in (('NativeRayFunctionTableToken', 'LudusNativeStageTable'),
                               ('NativeRayIntersectionTableToken', 'LudusNativeIntersectionTable')):
        pattern = r'\b' + token + r'_\d+\s+(?:const\s+)?device\s*\*'
        code, count = re.subn(pattern, native_type, code)
        _require(count >= 2, 'opaque table token is not represented by resource/context arguments')
    # Visible entry symbols belong to the fixed native ABI. Rename compiler-mangled
    # occurrences as whole tokens and verify the exact packet/context signature.
    for key in ('GENERATION', 'CLOSEST_HIT', 'MISS'):
        name = abi['LUDUS_METAL_RAY_' + key]
        function = _one(r'\bvoid\s+(' + name + r'_\d+)\s*\(([^{};]*)\)\s*\{', code,
                        name + ' definition')
        types = [re.sub(r'\b\w+\s*$', '', value).strip() for value in _arguments(function[2])]
        _require(types == [packet + ' thread*', context + ' thread*'], 'visible stage signature mismatch')
        code = re.sub(r'\b' + function[1] + r'\b', name, code)
        code = code.replace('void ' + name + '(', '[[visible]] void ' + name + '(', 1)
    # An intrinsic needing the resource context must occur in a compiler function
    # carrying that exact context pointer. Reject inlining or a compiler ABI change.
    masked = _mask(code)
    functions = list(re.finditer(r'^\s*(?:\[\[visible\]\]\s*)?\w+\s+\w+\s*\(([^{};]*)\)\s*\{',
                                 masked, re.M))
    for marker in reversed(list(re.finditer(r'\bLUDUS_NATIVE_CONTEXT\b', code))):
        function = next((value for value in reversed(functions) if value.end() <= marker.start() and
                         _close(masked, value.end() - 1) >= marker.end()), None)
        _require(function is not None, 'intrinsic is outside a supported resource-context function')
        argument = _one(r'\b' + context + r'\s+thread\s*\*\s*(\w+)', function[1], 'context parameter')[1]
        code = code[:marker.start()] + argument + code[marker.end():]
    stage_table = ('visible_function_table<void(' + packet + ' thread*, ' + context + ' thread*)>')
    prelude = f'''
#include <metal_raytracing>
#include <metal_visible_function_table>
struct {context};
struct {packet};
using LudusNativeStageTable = {stage_table};
using LudusNativeIntersectionTable = raytracing::intersection_function_table<raytracing::triangle_data, raytracing::instancing>;
void LudusDispatchNativeStage(LudusNativeStageTable functions, {packet} thread* packet, uint stage,
                              {context} thread* context);
uint4 LudusTraceNative(raytracing::acceleration_structure<raytracing::instancing> scene,
                      LudusNativeIntersectionTable intersections, float3 originWorld, float3 directionWorld,
                      float nearDistanceWorld, float farDistanceWorld, bool isVisibility, {context} thread* context);
'''
    code = code.replace('using namespace metal;', 'using namespace metal;\n' + prelude, 1)
    resolve = _one(r'\buint\s+(LudusNativeResolveTriangle_\d+)\s*\(', code, 'triangle mapping')[1]
    accept = _one(r'\bbool\s+(LudusNativeAcceptTriangle_\d+)\s*\(', code, 'alpha acceptance')[1]
    # Intersection callbacks receive exactly their immutable alpha-test buffers.
    # Check the transitive emitted helper closure before constructing that context;
    # an engine/compiler change must never introduce an unbound resource read.
    masked = _mask(code)
    definitions = {}
    for function in re.finditer(r'^\s*\w+\s+(\w+)\s*\(([^{};]*)\)\s*\{', masked, re.M):
        parameter = re.search(r'\b' + context + r'\s+thread\s*\*\s*(\w+)', function[2])
        if parameter:
            definitions[function[1]] = (parameter[1], masked[function.end():_close(masked, function.end() - 1)])
    pending, visited, used_members = [accept], set(), set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        _require(name in definitions, 'alpha helper resource closure is incomplete')
        parameter, body = definitions[name]
        used_members.update(re.findall(r'\b' + parameter + r'\s*->\s*(\w+)', body))
        pending.extend(name for name in re.findall(r'\b(\w+)\s*\(', body) if name in definitions)
    _require(used_members <= {selected[name][1] for name in ('Parameters', 'sceneWords', 'textureWords')},
             'alpha helper reads a resource outside its immutable intersection bindings')
    alpha_parameters = ',\n'.join(selected[name][2] for name in ('Parameters', 'sceneWords', 'textureWords'))
    alpha_assignments = '\n'.join(f'    context.{selected[name][1]} = {selected[name][0]};'
                                  for name in ('Parameters', 'sceneWords', 'textureWords'))
    code += f'''
void LudusDispatchNativeStage(LudusNativeStageTable functions, {packet} thread* packet, uint stage,
                              {context} thread* context)
{{
    functions[stage](packet, context);
}}
uint4 LudusTraceNative(raytracing::acceleration_structure<raytracing::instancing> scene,
                      LudusNativeIntersectionTable intersections, float3 originWorld, float3 directionWorld,
                      float nearDistanceWorld, float farDistanceWorld, bool isVisibility, {context} thread* context)
{{
    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    intersector.force_opacity(raytracing::forced_opacity::non_opaque);
    intersector.accept_any_intersection(isVisibility);
    raytracing::ray ray(originWorld, directionWorld, nearDistanceWorld, farDistanceWorld);
    auto hit = intersector.intersect(ray, scene, 255, intersections);
    if (hit.type == raytracing::intersection_type::none)
        return uint4(0xffffffffu, as_type<uint>(farDistanceWorld), 0, 0);
    return uint4({resolve}(hit.primitive_id, hit.geometry_id, context),
                 as_type<uint>(hit.distance), as_type<uint2>(hit.triangle_barycentric_coord));
}}
[[intersection(triangle, raytracing::triangle_data, raytracing::instancing)]]
bool {abi['LUDUS_METAL_RAY_INTERSECTION']}(uint primitive [[primitive_id]], uint geometry [[geometry_id]],
                                         float2 barycentric [[barycentric_coord]], {alpha_parameters})
{{
    {context} context = {{}};
{alpha_assignments}
    return {accept}(primitive, geometry, barycentric, &context);
}}
'''
    normalized = deepcopy(document)
    normalized['parameters'] = [item for item in normalized['parameters'] if item['name'] not in private]
    for value in [*normalized['parameters'], *normalized['entryPoints'][0].get('bindings', [])]:
        name = value['name']
        if name not in private:
            # A resource pinned to a native register reflects a list of bindings; any of them carries its usage.
            originals = value.pop('bindings', None) or [value.get('binding', {})]
            usage = [entry['used'] for entry in originals if 'used' in entry]
            value['binding'] = {'kind': 'descriptorTableSlot', 'index': slots[name],
                                **({'used': int(any(usage))} if usage else {})}
    normalized['entryPoints'][0]['bindings'] = [item for item in normalized['entryPoints'][0].get('bindings', [])
                                               if item['name'] not in private]
    _require('intersection_query<' not in code and 'LUDUS_NATIVE_CONTEXT' not in code,
             'inline-query or unresolved intrinsic remains in native stage output')
    return code, normalized
