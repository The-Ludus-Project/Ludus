"""Graph-owned output and render resolution policies; no GPU allocation."""
from math import gcd

KINDS = ('OutputSize', 'RenderSize')
NOMINAL_VIEW_EXTENT = (640, 480)


def input_ports(node):
    if node['kind'] == 'RenderSize':
        if node['extent'] == 'Texture':
            return {'texture': 'Texture'}
        if node['extent'] == 'OutputSize':
            return {'output': 'RenderSize'}
    return {}


def validate(node, where, graph):
    derived = node['kind'] == 'RenderSize' and node.get('extent') == 'OutputSize'
    graph._keys(node, ('id', 'kind', 'extent', 'scale') if derived else ('id', 'kind', 'extent'), where)
    graph._identifier(node['id'], where)
    graph._require(node['id'] != 'input', 'Schema', where, 'input is reserved')
    if node['kind'] == 'OutputSize':
        if node['extent'] != 'Viewport':
            graph._validate_extent(node['extent'], where)
    elif derived:
        graph._validate_extent(node['scale'], where)
    elif node['extent'] != 'Texture':
        graph._validate_size(node['extent'], where)


def resolve(node, inputs, resources, where, graph):
    """Return extent and canonical view fraction; ceil once against physical output pixels.

    A derived render policy accepts fixed output dimensions or the full drawable
    reference. Chaining view-relative scales is rejected, avoiding double rounding
    and ambiguous ownership of the output resolution.
    """
    extent = node['extent']
    scale = ()
    if node['kind'] == 'OutputSize' and extent == 'Viewport':
        scale = (1, 1)
    elif node['kind'] == 'RenderSize' and extent == 'OutputSize':
        output = inputs['output']
        graph._require(not output.scale or output.scale == (1, 1), 'SizePolicy', where,
                       'RenderSize needs an unscaled OutputSize reference. Connect OutputSize.size, '
                       'rather than another scaled RenderSize.', port='output', direction='input')
        numerator, denominator = node['scale']
        divisor = gcd(numerator, denominator)
        fraction = (numerator // divisor, denominator // divisor)
        if output.scale:
            scale = fraction
        else:
            extent = [(dimension * fraction[0] + fraction[1] - 1) // fraction[1] for dimension in output.extent]
            graph._validate_extent(extent, where)
    elif extent == 'Texture':
        texture = resources[inputs['texture'].resource]
        extent = texture['extent']
        scale = tuple(texture.get('view_scale', ()))
    elif type(extent) is dict:
        numerator, denominator = extent['scale']
        divisor = gcd(numerator, denominator)
        scale = (numerator // divisor, denominator // divisor)
    if scale:
        # Nominal reporting dimensions never replace runtime validation against the drawable.
        extent = [min(graph.MAX_TARGET_EXTENT, max(1, (dimension * scale[0] + scale[1] - 1) // scale[1]))
                  for dimension in NOMINAL_VIEW_EXTENT]
    return tuple(extent), scale
