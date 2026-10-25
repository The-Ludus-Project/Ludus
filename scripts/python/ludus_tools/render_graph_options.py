"""Author-declared finite choices, resolved independently for each graph instance.

Options select existing node fields/programs. They never create shader programs,
compiler macros or device capabilities. See docs/architecture/render-graph-authoring.md.
"""
from copy import deepcopy

MAX_OPTIONS = 16
MAX_VALUES = 16
MAX_COMBINATIONS = 64
MAX_LABEL_LENGTH = 128
MAX_ENUM_LENGTH = 63
MAX_NUMERIC_VALUE = 1000000
TYPES = ('Bool', 'UInt32', 'Float32', 'Enum')


def validate(declarations, overrides, where, graph):
    graph._list(declarations, MAX_OPTIONS, where)
    graph._require(type(overrides) is dict, 'Option', where, 'option overrides must be an object')
    claimed, targets = set(), set()
    combinations = 1
    for option in declarations:
        graph._keys(option, ('id', 'label', 'type', 'default', 'values', 'target'), where)
        graph._identifier(option['id'], where)
        graph._require(option['id'] not in claimed, 'Option', where, 'duplicate option ' + option['id'])
        claimed.add(option['id'])
        graph._require(type(option['label']) is str and 0 < len(option['label']) <= MAX_LABEL_LENGTH,
                       'Option', where, 'option label must contain 1–128 characters')
        graph._enum(option['type'], TYPES, where)
        graph._list(option['values'], MAX_VALUES, where)
        graph._require(option['values'], 'Option', where, 'option needs at least one allowed value')
        combinations *= len(option['values'])
        graph._require(combinations <= MAX_COMBINATIONS, 'Option', where,
                       f'at most {MAX_COMBINATIONS} combinations per interface; prefer one named mode')
        seen = set()
        for value in option['values']:
            valid = (type(value) is bool if option['type'] == 'Bool' else
                     type(value) is int and 0 <= value <= MAX_NUMERIC_VALUE if option['type'] == 'UInt32' else
                     type(value) in (int, float) and -MAX_NUMERIC_VALUE <= value <= MAX_NUMERIC_VALUE if option['type'] == 'Float32' else
                     type(value) is str and 0 < len(value) <= MAX_ENUM_LENGTH)
            graph._require(valid, 'Option', where, 'allowed value does not match ' + option['type'])
            key = (type(value).__name__ if option['type'] != 'Float32' else 'number', value)
            graph._require(key not in seen, 'Option', where, 'duplicate allowed value')
            seen.add(key)
        graph._require(contains(option, option['default']), 'Option', where, 'default is outside allowed values')
        graph._require(type(option['target']) is str and option['target'] and option['target'] not in targets,
                       'Option', where, 'option targets must be nonempty and unique')
        targets.add(option['target'])
    for identity, value in overrides.items():
        option = next((item for item in declarations if item['id'] == identity), None)
        graph._require(option is not None and contains(option, value), 'Option', where,
                       f'override {identity} is undeclared or outside its allowed values')


def contains(option, value):
    return any(value == item and (type(value) is type(item) or
               option['type'] == 'Float32' and type(value) in (int, float) and type(item) in (int, float))
               for item in option['values'])


def validate_pipeline(node, where, graph):
    validate(node.get('options', []), node.get('overrides', {}), where, graph)
    for option in node.get('options', []):
        target = option['target'].split('.')
        scalar = (len(target) == 2 and target[0] in ('state', 'invocation') and
                  target[1] in node[target[0]] and type(node[target[0]][target[1]]) in (str, bool, int, float))
        shader = (len(target) == 3 and target[0] == 'shaders' and target[2] == 'program' and
                  any(item['id'] == target[1] for item in node['shaders']))
        graph._require(scalar or shader, 'Option', where,
                       f"unknown option target {option['target']}; choose a scalar state/invocation field or shaders.ID.program")
        original = '' if shader else node[target[0]][target[1]]
        graph._require(all(type(value) is type(original) or type(original) in (int, float) and
                           type(value) in (int, float) for value in option['values']), 'Option', where,
                       f"option {option['id']} has a different type from its target")


def resolve_pipeline(node, where, graph):
    validate_pipeline(node, where, graph)
    result = deepcopy(node)
    for option in result.pop('options', []):
        value = node.get('overrides', {}).get(option['id'], option['default'])
        target = option['target'].split('.')
        if target[0] == 'shaders':
            next(item for item in result['shaders'] if item['id'] == target[1])['program'] = value
        else:
            result[target[0]][target[1]] = value
    result.pop('overrides', None)
    graph.pipeline.validate(result, where, graph)
    return result


def resolve_configuration(configuration, overrides, where, graph, library):
    """Copy and resolve only this instance; cached library documents stay immutable."""
    validate(configuration.get('options', []), overrides, where, graph)
    result = deepcopy(configuration)
    nodes = {node['id']: node for node in result['nodes']}
    for option in result.get('options', []):
        target = option['target'].split('.')
        graph._require(len(target) == 2 and target[0] in nodes, 'Option', where,
                       'configuration targets must name node.OptionId')
        node = nodes[target[0]]
        if node['kind'] == 'Subgraph':
            child = graph._configuration(library.assets[library.inside(node['asset'], where)],
                                         node['configuration'], where)
            declarations = child.get('options', [])
        else:
            graph._require(node['kind'] in graph.pipeline.KINDS, 'Option', where,
                           'configuration options must forward to a pipeline or subgraph option')
            declarations = node.get('options', [])
        forwarded = next((item for item in declarations if item['id'] == target[1]), None)
        graph._require(forwarded is not None and forwarded['type'] == option['type'] and
                       all(contains(forwarded, value) for value in option['values']), 'Option', where,
                       f"{option['target']} does not declare a compatible option and allowed values")
        node.setdefault('overrides', {})[target[1]] = overrides.get(option['id'], option['default'])
    for index, node in enumerate(result['nodes']):
        location = where + '/' + node['id']
        if node['kind'] in graph.pipeline.KINDS:
            result['nodes'][index] = resolve_pipeline(node, location, graph)
        elif node['kind'] == 'Subgraph':
            child = graph._configuration(library.assets[library.inside(node['asset'], location)],
                                         node['configuration'], location)
            validate(child.get('options', []), node.get('overrides', {}), location, graph)
    return result
