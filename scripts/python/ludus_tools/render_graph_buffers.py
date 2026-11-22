"""Graph-owned structured storage and shader-derived persistent buffer sizes."""

import re

MAX_BUFFERS = 2
FIRST_SLOT = 6
MAX_STRIDE = 4096
MAX_ELEMENTS = 16 * 1024 * 1024


def requests_named_resources(program, snapshot):
    """Recognize the authored Resources parameter; retain explicit legacy manifest choices."""
    if 'graph_resources' in program:
        return program['graph_resources']
    if program['stage'] not in ('compute', 'ray_generation') or program['output'] != 'Radiance':
        return False
    source = program.get('language_source', program['source'])
    text = snapshot.load(source).decode('utf-8')
    text = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', ' ', text, flags=re.S)
    function = re.escape(program['function'])
    return re.search(rf'\b{function}\s*\([^()]*\b{function}Resources\s+[A-Za-z_]\w*\s*\)', text) is not None


def link_persistent_structs(plans, programs, snapshot, build, closures=None):
    """Resolve named shader arguments from pass-local port declarations."""
    variants, signatures = {}, {}
    for plan in plans:
        instances = {context['instance']: context for context in plan['activity']}
        for operation in plan['passes']:
            if operation['kind'] != 'RayTracing':
                continue
            resources = plan['resources']
            pipeline = operation['pipeline']
            variant = build.ray.get_callback_variant(pipeline)
            program_id = variant[0]
            program = programs.get(program_id, {})
            is_named = bool(program) and requests_named_resources(program, snapshot)
            ports = [port for port in pipeline['inputs'] if port['type'] == 'Buffer']
            source_is_lusl = program.get('language_source', program.get('source', '')).endswith('.lusl')
            uses_legacy_word_binding = program.get('graph_resources', False) and not source_is_lusl
            fields = []
            for use in operation.get('persistent_buffers', []):
                record = resources[use['resource']].get('shader_struct')
                is_writable = use['output_version'] is not None
                build.require(is_writable or is_named, 'ShaderInterface', operation['id'],
                              f"add a final {program.get('function', 'ShaderMain')}Resources resources parameter to "
                              f"'{program_id}' to use its read-only Buffer port; see Shader API")
                if record is not None and program:
                    source = record['source']
                    if source == program.get('language_source', program['source']):
                        instance, node = use['resource'].rsplit('/', 1)
                        context = instances[instance]
                        repair = ('and choose it in Buffer sizing' if is_named else
                                  'include it from the program, and choose it in Buffer sizing')
                        raise build.ToolingError(
                            'RenderGraphBuildShaderInterface',
                            f"{use['resource']}: '{source}' is the entry point for '{program_id}'. Move the struct "
                            f"to a shared Slang file, {repair}.",
                            location={'source': context['source'], 'configuration': context['configuration'],
                                      'node': node, 'field': 'shader_struct'})
                    if closures is not None and not is_named and source not in closures[program_id]:
                        raise build.ToolingError(
                            'RenderGraphBuildShaderInterface',
                            f"{use['resource']}: add a quoted #include for shared file '{source}' to "
                            f"'{program_id}', with a path relative to the program source.",
                            location={'source': program['source'], 'program': program_id, 'line': 1, 'column': 1})
                    build.name(record['type'], 'buffer struct type')
                if is_named:
                    port = ports[use['slot']]['id']
                    field = port[0].upper() + port[1:]
                    build.name(field, f"shader member for Buffer port '{port}'")
                    build.require(all(previous['field'] != field for previous in fields),
                                  'ShaderInterface', operation['id'],
                                  f"Buffer ports map to the same shader member '{field}'. Rename one port.")
                    fields.append({'field': field, 'slot': use['slot'], 'record': record,
                                   'writable': is_writable,
                                   'word_view': record is None and not uses_legacy_word_binding})
            if not is_named:
                continue
            ordered = sorted(fields, key=lambda field: field['field'])
            signature = [(field['field'], field['writable'], field['record'] and
                          (field['record']['source'], field['record']['type'])) for field in ordered]
            key = (program['source'], program['function'])
            build.require(signatures.setdefault(key, signature) == signature,
                          'ShaderInterface', operation['id'],
                          'reused shader function has different named Buffer fields; use matching ports or another function')
            build.require(variants.setdefault(variant, ordered) == ordered, 'ShaderInterface', operation['id'],
                          'reused callback variant has different Buffer slots or capacities; use matching ports or another program')
    return variants


def record_view_name(field):
    record = field['record']
    if record is None:
        if not field.get('word_view'):
            return 'RWStructuredBuffer<uint4>'
        return 'Graph' + ('Writable' if field['writable'] else 'ReadOnly') + 'BufferWordsView'
    return 'Graph' + ('Writable' if field['writable'] else 'ReadOnly') + record['type'] + 'View'


def emit_record_layout(shape, expression, address, loads, stores, depth, build, view):
    """Generate scalar accesses at the reflected offsets, including fixed arrays."""
    kind = shape['kind']
    if kind == 'struct':
        for field in shape['fields']:
            offset = field.get('binding', {}).get('offset')
            build.require(type(offset) is int and offset >= 0 and offset % 4 == 0, 'ShaderInterface', view,
                          'named Buffer fields need word-aligned storage offsets')
            emit_record_layout(field['type'], expression + '.' + field['name'], f'{address} + {offset // 4}u',
                               loads, stores, depth, build, view)
        return
    if kind == 'array':
        index = f'element{depth}'
        count = shape['elementCount']
        stride = shape.get('uniformStride')
        build.require(type(count) is int and count > 0 and type(stride) is int and stride > 0 and stride % 4 == 0,
                      'ShaderInterface', view,
                      'named Buffer arrays need fixed capacity and word-aligned elements')
        loop = f'for (uint {index} = 0; {index} < {count}u; ++{index}) {{'
        loads.append(loop)
        stores.append(loop)
        emit_record_layout(shape['elementType'], f'{expression}[{index}]', f'{address} + {index} * {stride // 4}u',
                           loads, stores, depth + 1, build, view)
        loads.append('}')
        stores.append('}')
        return
    scalar = (shape['elementType'] if kind == 'vector' else shape).get('scalarType')
    cast = {'float32': 'asfloat', 'int32': 'asint', 'uint32': 'asuint'}.get(scalar)
    build.require(kind in ('scalar', 'vector') and cast is not None, 'ShaderInterface', view,
                  'named Buffer record fields must use 32-bit float, int, uint, vectors, structs or fixed arrays')
    for component in range(shape['elementCount'] if kind == 'vector' else 1):
        field = f'{expression}[{component}]' if kind == 'vector' else expression
        word = f'({address} + {component}u)'
        bound = f'Words[{word} / 4u][{word} % 4u]'
        loads.append(f'{field} = {cast}({bound});')
        stores.append(f'{bound} = asuint({field});')


def record_view_argument(field):
    handle = f'GraphBuffer{field["slot"]}'
    if field['record']:
        return f'{record_view_name(field)}({handle}, {field["record"]["count"]}u)'
    return f'{record_view_name(field)}({handle})' if field.get('word_view') else handle


def collect_record_views(contracts, build):
    views = {}
    for fields in contracts:
        for field in fields:
            record = field['record']
            if record is None:
                continue
            view = record_view_name(field)
            key = (record['source'], record['type'], field['writable'])
            build.require(views.setdefault(view, key) == key, 'ShaderInterface', view,
                          'include one shared Slang declaration per Buffer record type')
    return views


def record_view_definition(view, type_name, is_writable, *, loads=None, stores=None):
    """Describe the view for source assistance, or define it for an executable build."""
    getter = f'{type_name} record; {" ".join(loads)} return record;' if loads is not None else f'return ({type_name})0;'
    store = f'void Store(uint index, {type_name} record) {{ {" ".join(stores or [])} }}' if is_writable else ''
    setter = 'set { Store(index, newValue); }' if is_writable else ''
    return f"""
struct {view}
{{
    private RWStructuredBuffer<uint4> Words;
    private uint Count;
    __init(RWStructuredBuffer<uint4> words, uint count) {{ Words = words; Count = count; }}
    uint GetCount() {{ return Count; }}
    {type_name} Load(uint index) {{ {getter} }}
    {store}
    __subscript(uint index) -> {type_name}
    {{
        get {{ return Load(index); }}
        {setter}
    }}
}};
"""


def word_view_definitions(contracts):
    """Raw Buffer views take their capacity from the bound allocation at runtime."""
    variants = {field['writable']: record_view_name(field) for fields in contracts for field in fields
                if field.get('word_view')}
    return ''.join(f"""
struct {view}
{{
    private RWStructuredBuffer<uint4> Words;
    __init(RWStructuredBuffer<uint4> words) {{ Words = words; }}
    uint GetCount() {{ uint count; uint stride; Words.GetDimensions(count, stride); return count; }}
    uint4 Load(uint index) {{ return Words[index]; }}
    {'void Store(uint index, uint4 record) { Words[index] = record; }' if writable else ''}
    __subscript(uint index) -> uint4
    {{
        get {{ return Load(index); }}
        {'set { Store(index, newValue); }' if writable else ''}
    }}
}};
""" for writable, view in sorted(variants.items()))


def define_record_views(program, layouts, build):
    wrapper = program['ray_wrapper']
    for view, (source, type_name, is_writable) in program['named_record_views'].items():
        stride, shape = layouts[(source, type_name)]
        build.require(stride % 4 == 0, 'ShaderInterface', view, 'named Buffer record stride must be word-aligned')
        address = f'(index * {stride // 4}u)'
        loads, stores = [], []
        emit_record_layout(shape, 'record', address, loads, stores, 0, build, view)
        # A reflected scalar is accessed as one scalar on both native targets, even
        # when adjacent records share one uint4. Padding remains untouched.
        declaration = record_view_definition(view, type_name, is_writable)
        build.require(wrapper.count(declaration) == 1, 'ShaderInterface', view,
                      'linked shader is missing its generated Buffer view declaration')
        wrapper = wrapper.replace(declaration,
                                  record_view_definition(view, type_name, is_writable, loads=loads, stores=stores), 1)
    return wrapper


def size_persistent_structs(plans, snapshot, stage, compiler, cancel, build):
    """Use the same SPIR-V storage layout that the ordered Metal/Vulkan shaders use."""
    records, layouts = dict(snapshot.buffer_record_layouts), []
    for plan in plans:
        instances = {context['instance']: context for context in plan['activity']}
        for identity, resource in plan['resources'].items():
            record = resource.get('shader_struct')
            if record is None:
                continue
            source, type_name = record['source'], record['type']
            key = (source, type_name)
            if key not in records:
                directory = stage / ('buffer-layout-' + str(len(records)))
                records[key] = reflect_record(source, type_name, snapshot, directory, compiler, cancel, build)
            stride, _ = records[key]
            bytes_total = stride * record['count']
            word_bytes = build.ray.BUFFER_WORD_BYTES
            capacity = ((bytes_total + word_bytes - 1) // word_bytes) * word_bytes
            build.require(0 < capacity <= build.ray.MAX_BUFFER_BYTES, 'Capacity', identity,
                          f'{record["count"]} × {stride} bytes exceeds the persistent buffer limit. '
                          'Reduce the element capacity or the shader struct size.')
            resource['extent'][0] = capacity
            resource['shader_struct'] = {**record, 'stride': stride}
            prefix, node = identity.rsplit('/', 1)
            context = instances[prefix]
            layouts.append({'node': node, 'graph': context['source'], 'configuration': context['configuration'],
                            'stride': stride, 'bytes': capacity})
    return layouts, records


def reflect_record(source, type_name, snapshot, directory, compiler, cancel, build):
    # Thanks to the Slang team, Using the Reflection API (Types and Layouts),
    # https://shader-slang.org/slang/user-guide/reflection.html.
    # Slang reflects the stride of a fixed array of T in a storage-buffer record.
    # This includes the tail alignment that the last reflected field alone omits.
    closure = snapshot.shader_closure(source)
    authored = directory / 'source'
    if build.os.name == 'nt':
        build.require_paths_within_windows_limit(source, [authored / relative for relative in closure])
    for relative in closure:
        path = authored / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(snapshot.files[relative])
    # Keep the generated root outside the package-relative namespace.
    wrapper = directory / 'layout_probe.slang'
    wrapper.write_text(
        f'#include "source/{source}"\n'
        f'struct LudusLayoutProbe {{ {type_name} Elements[2]; }};\n'
        'RWStructuredBuffer<LudusLayoutProbe> LudusLayoutRecords;\n'
        'RWStructuredBuffer<uint2> LudusLayoutResult;\n'
        '[numthreads(1, 1, 1)]\n'
        'void LudusLayoutMain()\n'
        '{ uint count; uint stride; LudusLayoutRecords.GetDimensions(count, stride);\n'
        '  LudusLayoutResult[0] = uint2(count, stride); }\n',
        encoding='utf-8')
    reflection = directory / 'reflection.json'
    depfile = directory / 'layout.d'
    command = [str(compiler), str(wrapper), '-target', 'spirv', '-profile', 'spirv_1_3',
               '-entry', 'LudusLayoutMain', '-stage', 'compute', '-o', str(directory / 'layout.spv'),
               '-reflection-json', str(reflection), '-depfile', str(depfile)]
    try:
        build.run_stage(command, cancel, describe_output=lambda message: message.replace(str(directory) + '/', ''),
                        where=f'{source}::{type_name}')
    except build.ToolingError as error:
        if error.code == 'RenderGraphBuildShader':
            error.location = {'source': source, 'line': 1, 'column': 1}
        raise
    build.verify_shader_dependencies(depfile, closure, authored, wrapper, f'{source}::{type_name}')
    data = build.document(build.read(reflection), source)
    parameters = data.get('parameters', [])
    probe = next((parameter['type'] for parameter in parameters if parameter['name'] == 'LudusLayoutRecords'), {})
    fields = probe.get('resultType', {}).get('fields', [])
    array = fields[0]['type'] if fields and fields[0]['name'] == 'Elements' else {}
    element = array.get('elementType', {})
    stride = array.get('uniformStride')
    build.require(element.get('kind') == 'struct' and type(stride) is int and
                  0 < stride <= build.ray.MAX_BUFFER_BYTES,
                  'ShaderInterface', f'{source}::{type_name}',
                  'expected a sized storage struct. Select a struct with value fields that fits the buffer limit.')
    return stride, element


def validate_node(node, where, graph):
    graph._keys(node, ('id', 'kind', 'stride', 'count'), where)
    graph._identifier(node['id'], where)
    graph._require(node['id'] != 'input', 'Schema', where, 'input is reserved')
    graph._integer(node['stride'], 4, MAX_STRIDE, where)
    graph._require(node['stride'] % 4 == 0, 'Schema', where, 'structured stride must be a multiple of four bytes')
    if node['count'] != 'ColorTarget':
        graph._integer(node['count'], 1, MAX_ELEMENTS, where)


def validate_bindings(program, schemas, build):
    build.bounded_list(program['bindings'], MAX_BUFFERS, 'ray storage bindings')
    names, slots = set(), set()
    for binding in program['bindings']:
        build.keys(binding, ('id', 'schema', 'slot', 'access'), 'storage binding')
        build.name(binding['id'], 'storage binding name')
        build.integer(binding['slot'], FIRST_SLOT, FIRST_SLOT + MAX_BUFFERS - 1, 'storage slot')
        build.enum(binding['access'], ('Read', 'ReadWrite'), 'storage access')
        build.require(binding['id'] not in names and binding['slot'] not in slots and binding['id'] not in schemas,
                      'Schema', program['id'], 'duplicate or colliding storage binding')
        build.require(type(binding['schema']) is str and binding['schema'] in schemas and
                      schemas[binding['schema']]['role'] == 'Storage', 'Schema', program['id'],
                      'ray bindings require a Storage schema')
        names.add(binding['id']); slots.add(binding['slot'])


def merge_bindings(selected, build):
    merged = {}
    slots = {}
    for program in selected.values():
        for binding in program['bindings']:
            name, slot = binding['id'], binding['slot']
            build.require(merged.get(name, binding) == binding and slots.get(slot, name) == name,
                          'ShaderInterface', name, 'linked ray functions disagree on storage schema, slot or access')
            merged[name] = binding
            slots[slot] = name
    build.require(len(merged) <= MAX_BUFFERS, 'Capacity', 'ray storage', 'at most two structured buffers are supported')
    return sorted(merged.values(), key=lambda value: value['slot'])


def declarations(bindings, schemas, build):
    lines = []
    for name in sorted({binding['schema'] for binding in bindings}):
        lines += ['struct ' + name, '{']
        lines += ['    ' + build.STORAGE_SHAPES[field['type']][0] + ' ' + field['id'] + ';'
                  for field in schemas[name]['fields']]
        lines += ['};']
    for binding in bindings:
        kind = 'StructuredBuffer' if binding['access'] == 'Read' else 'RWStructuredBuffer'
        lines.append(f'[[vk::binding({binding["slot"]}, 0)]] {kind}<{binding["schema"]}> {binding["id"]};')
    return '\n'.join(lines) + '\n'


def lower_bindings(plan, stages, build):
    operations = [operation for operation in plan['passes'] if operation['kind'] == 'RayTracing']
    uses = operations[0].get('buffers', {}) if operations else {}
    reflected = [program['descriptor']['interface']['entries'] for (role, stage, _), program in stages.items()
                 if role == 'RayTracing' and stage == 'compute']
    if any(entry['name'] == 'GraphBuffer0' for target in reflected for entry in target):
        build.require(not uses, 'Capability', 'ray storage', 'ordered Buffer storage cannot share the structured job profile')
        return []
    records = []
    for name, use in sorted(uses.items(), key=lambda item: item[0]):
        resource = plan['resources'][use['resource']]
        entries = [next((entry for entry in target if entry['name'] == name), None) for target in reflected]
        expected = 'StorageRead' if use['access'] == 'Read' else 'StorageReadWrite'
        build.require(entries and all(entry and entry['kind'] == expected and entry['size'] == resource['stride']
                                      and FIRST_SLOT <= entry['binding'] < FIRST_SLOT + MAX_BUFFERS for entry in entries),
                      'ShaderInterface', name, 'buffer port name, access or stride differs from shader reflection')
        build.require(len({entry['binding'] for entry in entries}) == 1,
                      'ShaderInterface', name, 'storage slot differs across targets')
        records.append({'Slot': entries[0]['binding'], 'Stride': resource['stride'],
                        'Count': 0 if resource['count'] == 'ColorTarget' else resource['count'],
                        'IsReadOnly': int(use['access'] == 'Read')})
    for target in reflected:
        build.require({entry['name'] for entry in target if FIRST_SLOT <= entry['binding'] < FIRST_SLOT + MAX_BUFFERS} == set(uses),
                      'ShaderInterface', 'ray storage', 'shader storage bindings must match connected Buffer ports exactly')
    return sorted(records, key=lambda value: value['Slot'])
