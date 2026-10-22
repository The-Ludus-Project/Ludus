"""Bounded graph-owned structured storage for the native triangle ray profile."""

MAX_BUFFERS = 2
FIRST_SLOT = 6
MAX_STRIDE = 4096
MAX_ELEMENTS = 16 * 1024 * 1024


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
