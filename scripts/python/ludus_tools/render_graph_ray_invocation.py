"""Resolve logical ray-node invocation independently of its graph node kind.

The graph retains authored ray callbacks in every mode. Older documents omitted
the execution type because only compute lowering existed; retain that behavior
until the author explicitly selects a different invocation.
"""
from pathlib import PurePosixPath

HARDWARE_PIPELINE = 'RayTracing'
COMPUTE_PIPELINE = 'Compute'
NATIVE_QUERY = 'NativeQuery'
TRIANGLE_LOOP = 'TriangleLoop'
CUSTOM_QUERY = 'Custom'
EXECUTION_TYPES = (HARDWARE_PIPELINE, COMPUTE_PIPELINE)
TRAVERSAL_TYPES = (NATIVE_QUERY, TRIANGLE_LOOP, CUSTOM_QUERY)


def resolve_type(node):
    """Return the explicitly requested execution type, preserving legacy compute."""
    return node['invocation'].get('type', COMPUTE_PIPELINE)


def resolve_traversal(node):
    """Resolve compute traversal; hardware pipelines own their native traversal."""
    invocation = node['invocation']
    if resolve_type(node) == HARDWARE_PIPELINE:
        return None
    if 'type' in invocation:
        return invocation.get('traversal', TRIANGLE_LOOP)
    has_kernel = any(shader['stage'] == 'compute' for shader in node['shaders'])
    legacy_default = TRIANGLE_LOOP if node['kind'] == 'ComputePipeline' and not has_kernel else NATIVE_QUERY
    return invocation.get('traversal', legacy_default)


def validate(node, where, graph):
    """Reject contradictory settings before shader generation or publication."""
    invocation = node['invocation']
    if 'type' in invocation:
        graph._enum(invocation['type'], EXECUTION_TYPES, where)
    execution = resolve_type(node)
    graph._require(node['kind'] == 'RayTracingPipeline' or execution == COMPUTE_PIPELINE,
                   'Capability', where, 'hardware ray-stage invocation requires a logical RayTracingPipeline node')
    if execution == HARDWARE_PIPELINE:
        graph._require('traversal' not in invocation and 'traversal_source' not in invocation,
                       'Capability', where,
                       'hardware ray-stage invocation owns native traversal; choose Compute to customize queries')
        return
    if 'traversal' in invocation:
        graph._enum(invocation['traversal'], TRAVERSAL_TYPES, where)
    is_custom = resolve_traversal(node) == CUSTOM_QUERY
    graph._require(('traversal_source' in invocation) == is_custom, 'Schema', where,
                   'Custom compute traversal requires traversal_source; other strategies do not accept it')
    if is_custom:
        source = invocation['traversal_source']
        graph._require(type(source) is str and source != '' and '\\' not in source and
                       not PurePosixPath(source).is_absolute() and '..' not in PurePosixPath(source).parts and
                       PurePosixPath(source).suffix == '.slang', 'Source', where,
                       'traversal_source must be a package-relative .slang source without parent traversal')
