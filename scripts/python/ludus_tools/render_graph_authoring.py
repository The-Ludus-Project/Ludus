"""RG3 schema comparison and parameter migration between cooked generations.

The editor and CLI share these functions; neither rewrites saved documents or
packages. A value survives only when its stable field identity, role and type
agree, or when the author declares an explicit default for a changed/added field.
Field names are the stable identities of the version-1 build format, so a rename
is a removal plus an addition, never a reinterpretation of existing bytes.

Thanks to Christopher Maughan, "The Design of FX Composer", GPU Gems, chapter 30,
sections 30.3.1 and 30.6.2-30.6.4: parameter reconciliation across recompiled
effects. Ludus departs by refusing name-similarity matching and implicit byte
reinterpretation; see docs/architecture/render-graph-authoring.md.
"""
from __future__ import annotations

import math

from . import render_graph_build as build
from . import render_graph_execution as execution

MAX_VALUES = 256


def _package(path):
    return build.verify_package(execution.resolve_generation(path))


def _schemas(package):
    return {schema['id']: schema for schema in package['schemas']}


def _field_key(schema, field):
    # Vertex/varying identity includes its interface placement; uniform identity is id/type.
    if schema['role'] in ('Uniform', 'Storage'):
        return field['type']
    return field['type'], field['semantic'], field['location']


def compare_packages(old, new):
    """Compare logical schemas and target layouts of two verified generations.

    Returns per-schema field differences, per-program/target binding layout
    changes and a CPU interface verdict: Compatible when every Uniform/Vertex
    schema and emitted layout is unchanged, otherwise RestartRequired. A changed
    CPU interface needs a rebuilt game/host interface before activation; this
    function never synthesizes an adapter. Both packages are integrity checked.
    """
    old_package, new_package = _package(old), _package(new)
    before, after = _schemas(old_package), _schemas(new_package)
    schemas = []
    for sid in sorted(before.keys() | after.keys()):
        if sid not in after or sid not in before:
            role = (before.get(sid) or after[sid])['role']
            schemas.append({'id': sid, 'role': role, 'status': 'Removed' if sid not in after else 'Added', 'fields': []})
            continue
        old_schema, new_schema = before[sid], after[sid]
        if old_schema['role'] != new_schema['role']:
            schemas.append({'id': sid, 'role': new_schema['role'], 'status': 'Incompatible', 'fields': [],
                            'reason': 'schema role changed from ' + old_schema['role']})
            continue
        old_fields = {f['id']: f for f in old_schema['fields']}
        new_fields = {f['id']: f for f in new_schema['fields']}
        fields = []
        for fid in sorted(old_fields.keys() | new_fields.keys()):
            if fid not in new_fields:
                fields.append({'id': fid, 'status': 'Removed'})
            elif fid not in old_fields:
                fields.append({'id': fid, 'status': 'Added', 'type': new_fields[fid]['type']})
            elif _field_key(old_schema, old_fields[fid]) != _field_key(new_schema, new_fields[fid]):
                fields.append({'id': fid, 'status': 'Changed', 'from': old_fields[fid]['type'], 'type': new_fields[fid]['type']})
            else:
                fields.append({'id': fid, 'status': 'Retained', 'type': new_fields[fid]['type']})
        order_changed = [f['id'] for f in old_schema['fields']] != [f['id'] for f in new_schema['fields']]
        status = 'Unchanged' if old_schema == new_schema else 'Changed'
        schemas.append({'id': sid, 'role': new_schema['role'], 'status': status, 'fields': fields,
                        'order_changed': order_changed})

    def layouts(package):
        return {(p['id'], p['profile']): p['descriptor'].get('layouts', []) for p in package['programs']}

    old_layouts, new_layouts = layouts(old_package), layouts(new_package)
    layout_changes = [{'program': pid, 'profile': profile}
                      for pid, profile in sorted(old_layouts.keys() | new_layouts.keys())
                      if old_layouts.get((pid, profile)) != new_layouts.get((pid, profile))]
    cpu_changed = any(s['role'] in ('Uniform', 'Vertex') and s['status'] != 'Unchanged' for s in schemas)
    return {'version': 1, 'stage': 'RG3', 'from': old_package['id'], 'to': new_package['id'],
            'schemas': schemas, 'layout_changes': layout_changes,
            'cpu_interface': 'RestartRequired' if cpu_changed or layout_changes else 'Compatible'}


def _vector(value, width, where):
    build.require(type(value) is list and len(value) == width and
                  all(type(x) in (int, float) and type(x) is not bool and math.isfinite(x) for x in value),
                  'Migration', where, f'expected {width} finite numbers')
    return [float(x) for x in value]


def _values(table, schemas, where):
    build.require(type(table) is dict, 'Migration', where, 'object of schema values required')
    count = 0
    for sid, fields in table.items():
        build.require(sid in schemas and schemas[sid]['role'] == 'Uniform', 'Migration', where,
                      f'{sid} is not a Uniform schema of this generation')
        build.require(type(fields) is dict, 'Migration', where + '/' + sid, 'object of field values required')
        declared = {f['id']: f for f in schemas[sid]['fields']}
        for fid, value in fields.items():
            count += 1
            build.require(count <= MAX_VALUES, 'Capacity', where, f'at most {MAX_VALUES} values')
            build.require(fid in declared, 'Migration', f'{where}/{sid}', f'unknown field {fid}')
            _vector(value, build.SHAPES[declared[fid]['type']][1], f'{where}/{sid}.{fid}')


def migrate_values(old, new, values, defaults=None):
    """Migrate Uniform parameter values from the old generation's schemas.

    values/defaults map schema -> field -> list of finite numbers (Float32 uses
    one entry). Retained requires the same field identity and type; Added and
    Changed fields require an explicit author default; Removed values are dropped
    and reported. Returns Migrated with new values, or Rejected with the diff and
    no values. Inputs are not modified and nothing is written.
    """
    defaults = {} if defaults is None else defaults
    comparison = compare_packages(old, new)
    before, after = _schemas(_package(old)), _schemas(_package(new))
    _values(values, before, 'values')
    _values(defaults, after, 'defaults')
    diff, result, rejected = [], {}, False
    for schema in comparison['schemas']:
        if schema['role'] != 'Uniform':
            continue
        sid = schema['id']
        if schema['status'] in ('Removed', 'Incompatible'):
            for fid in sorted(values.get(sid, {})):
                diff.append({'schema': sid, 'field': fid, 'action': 'Dropped', 'reason': 'schema ' + schema['status'].lower()})
            continue
        fields = schema['fields'] or [{'id': f['id'], 'status': 'Added', 'type': f['type']} for f in after[sid]['fields']]
        migrated = {}
        for field in fields:
            fid, status = field['id'], field['status']
            supplied = values.get(sid, {}).get(fid)
            declared = defaults.get(sid, {}).get(fid)
            if status == 'Removed':
                if supplied is not None:
                    diff.append({'schema': sid, 'field': fid, 'action': 'Dropped', 'reason': 'field removed'})
            elif status == 'Retained' and supplied is not None:
                migrated[fid] = list(supplied)
                diff.append({'schema': sid, 'field': fid, 'action': 'Retained'})
            elif declared is not None:
                migrated[fid] = list(declared)
                reason = 'type changed from ' + field['from'] if status == 'Changed' else (
                    'field added' if status == 'Added' else 'no previous value')
                diff.append({'schema': sid, 'field': fid, 'action': 'Defaulted', 'reason': reason})
            else:
                rejected = True
                reason = ('type changed from ' + field['from'] if status == 'Changed' else
                          'field added' if status == 'Added' else 'no previous value')
                diff.append({'schema': sid, 'field': fid, 'action': 'Rejected',
                             'reason': reason + '; declare an explicit default'})
        result[sid] = migrated
    return {'version': 1, 'stage': 'RG3', 'status': 'Rejected' if rejected else 'Migrated',
            'cpu_interface': comparison['cpu_interface'], 'diff': diff,
            **({} if rejected else {'values': result})}
