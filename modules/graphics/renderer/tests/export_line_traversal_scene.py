#!/usr/bin/env python3
"""Export glTF triangle geometry for the hidden line traversal measurement, not an engine importer.

Writes the file that LUDUS_LINE_TRAVERSAL_SCENE names for render_graph_ray_tests'
[.line-traversal-performance] case: a little-endian uint32 vertex count and index count, then
float32 x, y, z World positions and uint32 triangle indices. Node transforms are applied; materials,
textures and non-triangle primitives are ignored. --maximum-triangles keeps whole primitives in node
order while they fit. Pure Python, so it needs no packages; output belongs in the caller's out/ tree.
"""
import argparse
import array
import json
from pathlib import Path
import struct
import sys

COMPONENT_CODES = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}
TYPE_WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
TRIANGLES_MODE = 4
IDENTITY = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]


def read_accessor(document, binary, index):
    accessor = document['accessors'][index]
    view = document['bufferViews'][accessor['bufferView']]
    code = COMPONENT_CODES[accessor['componentType']]
    width = TYPE_WIDTHS[accessor['type']]
    size = struct.calcsize(code)
    offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
    stride = view.get('byteStride', 0) or size * width
    values = array.array(code)
    if stride == size * width:
        values.frombytes(binary[offset:offset + size * width * accessor['count']])
    else:
        for element in range(accessor['count']):
            start = offset + element * stride
            values.frombytes(binary[start:start + size * width])
    if sys.byteorder != 'little':
        values.byteswap()
    return values


def multiply(first, second):
    return [[sum(first[row][k] * second[k][column] for k in range(4)) for column in range(4)] for row in range(4)]


def get_local_transform(node):
    if 'matrix' in node:
        matrix = node['matrix']
        return [[matrix[column * 4 + row] for column in range(4)] for row in range(4)]
    translation = node.get('translation', [0, 0, 0])
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    scale = node.get('scale', [1, 1, 1])
    rotation = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[rotation[row][column] * scale[column] for column in range(3)] + [translation[row]]
            for row in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def export_scene(source, maximum_triangles):
    document = json.loads(source.read_text())
    buffers = document['buffers']
    if len(buffers) != 1 or 'uri' not in buffers[0]:
        raise ValueError('expected one external .bin buffer')
    binary = (source.parent / buffers[0]['uri']).read_bytes()
    positions = array.array('f')
    indices = array.array('I')

    def visit(node_index, parent):
        node = document['nodes'][node_index]
        transform = multiply(parent, get_local_transform(node))
        if 'mesh' in node:
            for primitive in document['meshes'][node['mesh']]['primitives']:
                if primitive.get('mode', TRIANGLES_MODE) != TRIANGLES_MODE or 'indices' not in primitive:
                    continue
                count = document['accessors'][primitive['indices']]['count'] // 3
                if len(indices) // 3 + count > maximum_triangles:
                    continue
                first = len(positions) // 3
                local = read_accessor(document, binary, primitive['attributes']['POSITION'])
                for vertex in range(0, len(local), 3):
                    x, y, z = local[vertex], local[vertex + 1], local[vertex + 2]
                    for row in range(3):
                        positions.append(transform[row][0] * x + transform[row][1] * y + transform[row][2] * z +
                                         transform[row][3])
                indices.extend(first + index for index in read_accessor(document, binary, primitive['indices']))
        for child in node.get('children', []):
            visit(child, transform)

    for root in document['scenes'][document.get('scene', 0)]['nodes']:
        visit(root, IDENTITY)
    return positions, indices


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', type=Path, help='.gltf file with one external .bin buffer')
    parser.add_argument('output', type=Path)
    parser.add_argument('--maximum-triangles', type=int, default=(1 << 32) // 3 - 1)
    args = parser.parse_args()
    positions, indices = export_scene(args.source, args.maximum_triangles)
    if sys.byteorder != 'little':
        positions.byteswap()
        indices.byteswap()
    with args.output.open('wb') as output:
        output.write(struct.pack('<II', len(positions) // 3, len(indices)))
        positions.tofile(output)
        indices.tofile(output)
    print(f'{len(positions) // 3} vertices, {len(indices) // 3} triangles')


if __name__ == '__main__':
    main()
