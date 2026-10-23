"""Upscaling interfaces: executable bilinear/MetalFX Spatial and unavailable vendor adapters.

Thanks to NVIDIA, DLSS Programming Guide (https://github.com/NVIDIA/DLSS/tree/main/doc),
AMD, FidelityFX Super Resolution 2.3.3, Inputs
(https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/),
Apple, MTLFXTemporalScaler (https://developer.apple.com/documentation/metalfx/mtlfxtemporalscaler),
and Intel, XeSS Developer Guide (https://github.com/intel/xess/tree/main/doc).
We adapt their spatial/temporal input distinction into a draft graph contract;
these records neither wrap SDK calls nor promise compatible native texture formats.
"""

# Stable authored identities, independent of installed SDK versions or runtime detection.
ALGORITHMS = {
    'bilinear': ('Bilinear', False),
    'dlss-sr': ('DLSS Super Resolution', True),
    'fsr-1': ('FSR 1 spatial', False),
    'fsr-2': ('FSR 2 temporal', True),
    'fsr-3': ('FSR 3 upscaling', True),
    'metalfx-spatial': ('MetalFX Spatial', False),
    'metalfx-temporal': ('MetalFX Temporal', True),
    'xess-sr': ('XeSS Super Resolution', True),
}


def new_node(algorithm, identity):
    """Create an independent preset record; validation owns identity/algorithm legality."""
    node = {'id': identity, 'kind': 'Upscale', 'algorithm': algorithm}
    if algorithm == 'metalfx-spatial':
        node['color_processing'] = 'Linear'
    return node


def input_ports(node):
    """Return the fixed interface. Output storage is an explicit, separate input."""
    ports = {'color': 'ColorTexture', 'destination': 'ColorTexture'}
    if ALGORITHMS[node['algorithm']][1]:
        ports.update(depth='DepthTexture', motion='ColorTexture', jitter_pixel='Float2',
                     motion_scale='Float2', exposure='Float32', reset='UInt32')
    return ports


def validate(node, where, graph):
    """Validate saved drafts without requiring wires, an SDK, or a device."""
    graph._keys(node, ('id', 'kind', 'algorithm'), where, optional=('color_processing',))
    if 'color_processing' in node:
        graph._require(node['algorithm'] == 'metalfx-spatial', 'Schema', where,
                       'color_processing is only supported by MetalFX Spatial')
        graph._enum(node['color_processing'], ('Linear', 'Perceptual'), where)
    graph._identifier(node['id'], where)
    graph._require(node['id'] != 'input', 'Schema', where, 'input is reserved')
    graph._enum(node['algorithm'], tuple(ALGORITHMS), where)


def reject_execution(node, where, graph):
    """Fail at the selected node, preserving the current executable generation."""
    if node['algorithm'] in ('bilinear', 'metalfx-spatial'):
        return
    label = ALGORITHMS[node['algorithm']][0]
    repair = ('Keep this as an unused draft, or disconnect its output and reconnect the '
              'previous color path. SDK adapters and runtime upscaling are not implemented.')
    graph._fail('Capability', where,
                f'{label} is an authoring preset only; its upscaling backend is unavailable '
                f'on every current Ludus execution profile. {repair}',
                field='algorithm', expected='implemented upscaling backend', actual=node['algorithm'], repair=repair)
