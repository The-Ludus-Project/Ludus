"""Graph-owned structured storage and shader-derived persistent buffer sizes."""

MAX_BUFFERS = 2
FIRST_SLOT = 6
MAX_STRIDE = 4096
MAX_ELEMENTS = 16 * 1024 * 1024


def validate_persistent_struct_links(plans, programs, closures, build):
    """Link each use to the same package-local Slang declaration."""
    for plan in plans:
        resources = plan['resources']
        for operation in plan['passes']:
            for use in operation.get('persistent_buffers', []):
                record = resources[use['resource']].get('shader_struct')
                if record is None:
                    continue
                program = next(shader['program'] for shader in operation['pipeline']['shaders']
                               if shader['stage'] == 'compute')
                source = record['source']
                if program in programs and (programs[program]['source'] == source or source not in closures[program]):
                    raise build.ToolingError('RenderGraphBuildShaderInterface',
                                             f"{use['resource']}: include '{source}' from scene compute program "
                                             f"'{program}'. Select the shared file that declares the struct in Buffer sizing.",
                                             location={'source': programs[program]['source'], 'program': program,
                                                       'line': 1, 'column': 1})
                build.name(record['type'], 'buffer struct type')


def size_persistent_structs(plans, snapshot, stage, compiler, cancel, build):
    """Use the same SPIR-V storage layout that the ordered Metal/Vulkan shaders use."""
    strides, layouts = {}, []
    for plan in plans:
        instances = {context['instance']: context for context in plan['activity']}
        for identity, resource in plan['resources'].items():
            record = resource.get('shader_struct')
            if record is None:
                continue
            source, type_name = record['source'], record['type']
            key = (source, type_name)
            if key not in strides:
                directory = stage / ('buffer-layout-' + str(len(strides)))
                strides[key] = reflect_stride(source, type_name, snapshot, directory, compiler, cancel, build)
            stride = strides[key]
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
    return layouts


def reflect_stride(source, type_name, snapshot, directory, compiler, cancel, build):
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
    return stride


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
