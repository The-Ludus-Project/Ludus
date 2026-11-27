"""Predict the label export of a saved, translation-only World from its projected geometry.

The prediction shares no code with the renderer. It projects the World's cube meshes through the
saved orthographic camera at each pixel centre and keeps the nearest surface. The label of a pixel is
the visible object's position among the World's renderables plus one; the background is zero. This is
the identity the visibility codec stores in alpha (modules/graphics/renderer/shaders/visibility_buffer.slang),
and RGB stores the mesh-local triangle.

Edge-coverage rule: a pixel centre must lie strictly inside or strictly outside every object's projected
silhouette, so each label is unambiguous. Inside one face, a centre on the edge shared by its two
triangles may carry either triangle; the check accepts both and no other.
"""
import json
import math

# Barycentric tolerance for a pixel centre on a triangle edge, far below one pixel at these scales.
EDGE_TOLERANCE = 1e-9
TRIANGLE_VERTICES = 3
BYTE_BITS = 8
BYTE_MASK = 255


def read_mesh(path):
    mesh = json.loads(path.read_text())
    positions = [tuple(float(value) for value in position) for position in mesh['positions']]
    indices = mesh['indices']
    return [tuple(positions[i] for i in indices[start:start + TRIANGLE_VERTICES])
            for start in range(0, len(indices), TRIANGLE_VERTICES)]


def read_scene(world):
    """Return the renderables' translations and tints in World order, and the camera."""
    objects = []
    camera = None
    for entity in world['entities']:
        components = entity['components']
        transform = components['transform']
        if any(transform[axis] != 0 for axis in ('rx', 'ry', 'rz')) or any(transform[axis] != 1 for axis in ('sx', 'sy', 'sz')):
            raise ValueError(f'{entity["id"]}: the prediction handles translation-only transforms')
        position = (transform['x'], transform['y'], transform['z'])
        if 'renderable' in components:
            surface = components['renderable']
            if surface['mesh'] != 'mesh.cube' or surface['texture'] or not surface['visible']:
                raise ValueError(f'{entity["id"]}: the prediction handles visible untextured cubes')
            objects.append({'id': entity['id'], 'position': position,
                            'color': tuple(surface[channel] for channel in 'rgb')})
        elif 'camera' in components:
            lens = components['camera']
            if lens['projection'] != 'orthographic' or camera is not None:
                raise ValueError('the prediction handles one orthographic camera')
            camera = {'position': position, 'span': lens['span']}
    if camera is None or not objects:
        raise ValueError('the World needs renderables and an orthographic camera')
    return objects, camera


def locate(point, triangle):
    """Return (barycentric weights, depth) of point in the triangle's XY projection, or None if degenerate."""
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = triangle
    area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
    if area == 0:
        return None  # edge-on in an orthographic view along Z: no pixel area
    x, y = point
    u = ((bx - x) * (cy - y) - (cx - x) * (by - y)) / area
    v = ((cx - x) * (ay - y) - (ax - x) * (cy - y)) / area
    w = 1 - u - v
    return (u, v, w), u * az + v * bz + w * cz


def predict(world, mesh, extent):
    """Return per-pixel labels, accepted triangle sets and the flat colour of each visible object.

    The camera looks along -Z, so the nearest surface has the largest Z.
    """
    objects, camera = read_scene(world)
    width, height = extent
    pixels_per_metre = height / camera['span']
    labels = [[0] * width for _ in range(height)]
    triangles = [[frozenset()] * width for _ in range(height)]
    for row in range(height):
        for column in range(width):
            point = (camera['position'][0] + (column + 0.5 - width / 2) / pixels_per_metre,
                     camera['position'][1] - (row + 0.5 - height / 2) / pixels_per_metre)
            nearest = None
            for index, item in enumerate(objects):
                hits = {}
                for primitive, local in enumerate(mesh):
                    placed = tuple(tuple(a + b for a, b in zip(vertex, item['position'])) for vertex in local)
                    located = locate(point, placed)
                    if located is not None and min(located[0]) >= -EDGE_TOLERANCE:
                        hits[primitive] = located
                if not hits:
                    continue
                depth = max(depth for _, depth in hits.values())
                front = frozenset(p for p, (_, d) in hits.items() if math.isclose(d, depth, abs_tol=EDGE_TOLERANCE))
                # On the visible face, an edge point is unambiguous only on an edge two triangles share.
                if len(front) < 2 and min(hits[next(iter(front))][0]) <= EDGE_TOLERANCE:
                    raise ValueError(f'pixel ({column}, {row}) lies on the silhouette of {item["id"]}')
                if nearest is not None and math.isclose(depth, nearest[1], abs_tol=EDGE_TOLERANCE):
                    raise ValueError(f'pixel ({column}, {row}) has a depth tie')
                if nearest is None or depth > nearest[1]:
                    nearest = (index, depth, front)
            if nearest is not None:
                labels[row][column] = nearest[0] + 1
                triangles[row][column] = nearest[2]
    return labels, triangles, [item['color'] for item in objects]


def decode(rgba):
    """Split an 8-bit RGBA label pixel into (label, mesh-local triangle)."""
    red, green, blue, alpha = rgba
    return alpha, red | green << BYTE_BITS | blue << (2 * BYTE_BITS)


def check(image, labels, triangles):
    """Compare a captured label image with the prediction; return the report and a label-only grid."""
    width, height = image.size
    mismatched = []
    wrong_triangle = []
    shared_edge = 0
    captured = [[0] * width for _ in range(height)]
    for row in range(height):
        for column in range(width):
            pixel = image.getpixel((column, row))
            label, primitive = decode(pixel)
            captured[row][column] = label
            expected = labels[row][column]
            if label != expected:
                mismatched.append([column, row, expected, label])
            elif expected == 0 and pixel != (0, 0, 0, 0):
                wrong_triangle.append([column, row, 'background must be zero', list(pixel)])
            elif expected and primitive not in triangles[row][column]:
                wrong_triangle.append([column, row, sorted(triangles[row][column]), primitive])
            shared_edge += len(triangles[row][column]) > 1
    counts = {}
    for row in labels:
        for label in row:
            counts[str(label)] = counts.get(str(label), 0) + 1
    return {
        'passed': not mismatched and not wrong_triangle,
        'expected_pixels_per_label': counts,
        'shared_edge_pixels': shared_edge,
        'label_mismatches': len(mismatched),
        'triangle_mismatches': len(wrong_triangle),
        'first_label_mismatches': mismatched[:16],
        'first_triangle_mismatches': wrong_triangle[:16],
    }, captured


def paint_colors(labels, colors):
    """Return the expected flat colour export, as 8-bit RGB rows, for the predicted labels."""
    def byte(value):
        return int(round(value * BYTE_MASK))
    palette = [(0, 0, 0)] + [tuple(byte(channel) for channel in color) for color in colors]
    return [[palette[label] for label in row] for row in labels]
