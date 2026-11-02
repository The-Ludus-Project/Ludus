"""Bounded, manifest-owned shader constants exposed by graph authoring tools."""

import math

# Settings become one static constant per manifest setting in each program's source. The bound
# admits every built-in template together; a regression checks their combined definitions
# against this limit.
MAX_SETTINGS = 64
MAX_NUMERIC_VALUE = 1000000
MAX_SETTING_LABEL_LENGTH = 128


def validate(settings, build):
    build.bounded_list(settings, MAX_SETTINGS, 'settings')
    identifiers = set()
    for setting in settings:
        build.require(type(setting) is dict, 'Schema', 'settings', 'setting object required')
        is_boolean = setting.get('type') == 'Bool'
        build.keys(setting, ('id', 'label', 'type', 'value') if is_boolean else
                   ('id', 'label', 'type', 'value', 'minimum', 'maximum'), 'setting')
        build.name(setting['id'], 'setting id')
        build.require(setting['id'] not in identifiers, 'Schema', setting['id'], 'duplicate setting')
        identifiers.add(setting['id'])
        build.enum(setting['type'], ('UInt32', 'Float32', 'Bool'), setting['id'])
        build.require(type(setting['label']) is str and 0 < len(setting['label']) <= MAX_SETTING_LABEL_LENGTH,
                      'Schema', setting['id'], 'bounded nonempty setting label required')
        if is_boolean:
            build.require(type(setting['value']) is bool, 'Schema', setting['id'], 'boolean setting required')
            continue
        for field in ('minimum', 'value', 'maximum'):
            value = setting[field]
            build.require(type(value) in (int, float) and 0 <= value <= MAX_NUMERIC_VALUE and
                          math.isfinite(value), 'Schema', setting['id'], 'finite bounded number required')
            if setting['type'] == 'UInt32':
                build.require(type(value) is int, 'Schema', setting['id'], 'integer setting required')
        build.require(setting['minimum'] <= setting['value'] <= setting['maximum'],
                      'Schema', setting['id'], 'setting is outside its authored range')


def shader_source(settings):
    lines = ['// Generated manifest settings; edit the graph settings, then rebuild.'] if settings else []
    for setting in settings:
        kind = {'UInt32': 'uint', 'Float32': 'float', 'Bool': 'bool'}[setting['type']]
        value = setting['value']
        literal = ('true' if value else 'false') if kind == 'bool' else str(value) + ('f' if kind == 'float' else 'u')
        if kind == 'float':
            literal = repr(float(value)) + 'f'
        lines.append('static const ' + kind + ' ' + setting['id'] + ' = ' + literal + ';')
    return '\n'.join(lines) + ('\n' if lines else '')
