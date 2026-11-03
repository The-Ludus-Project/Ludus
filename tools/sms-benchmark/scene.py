"""Pinned Figure 17 scene, preserving the original slope-map and Beckmann model.

Thanks to Tizian Zeltner, Iliyan Georgiev and Wenzel Jakob, 'Specular Manifold
Sampling for Rendering High-Frequency Caustics and Glints', SIGGRAPH 2020,
sections 4.5/5.5 and Figure 17, https://doi.org/10.1145/3386569.3392408.
The scalar normal-map interpolation follows their normalmap.h and normalmap.cpp.
This optional NumPy oracle is independent of the Slang implementation.
"""
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

import numpy as np

SOURCE = json.loads((Path(__file__).parent / 'source.json').read_text())


def normalize(values):
    values = np.asarray(values, dtype=np.float64)
    return values / np.maximum(np.linalg.norm(values, axis=-1, keepdims=True), np.finfo(float).tiny)


def verify_assets(directory):
    """Reject modified/missing source files before interpreting the scene."""
    for name, expected in SOURCE['files'].items():
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Missing or modified published asset: {path}; acquire the pinned revision.')
    return dict(SOURCE['files'])


def parse_vector(text):
    return np.asarray([float(value) for value in text.replace(',', ' ').split()])


def parse_transform(element, variables=None):
    """Apply Mitsuba's listed transforms in order (each left-multiplies)."""
    result = np.eye(4)
    if element is None:
        return result
    for operation in element:
        parameters = dict(operation.attrib)
        for key, value in parameters.items():
            if value.startswith('$'):
                parameters[key] = str(variables[value[1:]])
        matrix = np.eye(4)
        if operation.tag == 'scale':
            matrix[np.arange(3), np.arange(3)] = [float(parameters.get(axis, parameters.get('value', 1))) for axis in 'xyz']
        elif operation.tag == 'translate':
            matrix[:3, 3] = [float(parameters.get(axis, 0)) for axis in 'xyz']
        elif operation.tag == 'rotate':
            axis = normalize([float(parameters.get(axis, 0)) for axis in 'xyz'])
            angle = np.radians(float(parameters['angle']))
            skew = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
            matrix[:3, :3] = np.eye(3)*np.cos(angle) + (1-np.cos(angle))*np.outer(axis, axis) + np.sin(angle)*skew
        elif operation.tag == 'lookat':
            origin = parse_vector(parameters['origin'])
            direction = normalize(parse_vector(parameters['target'])-origin)
            left = normalize(np.cross(parse_vector(parameters['up']), direction))
            matrix[:3, :3] = np.stack((left, np.cross(direction, left), direction), axis=1)
            matrix[:3, 3] = origin
        else:
            raise ValueError(f'Unsupported scene transform {operation.tag}; do not approximate it.')
        result = matrix @ result
    return result


def interpolate_map(pixels, coordinates):
    """Texel-centred bilinear lookup; the source normal map clamps after UV wrap."""
    height, width = pixels.shape[:2]
    position = np.clip((coordinates-np.floor(coordinates))*[width, height]-.5, 0, [width-1, height-1])
    lower = np.floor(position).astype(int)
    upper = np.minimum(lower+1, [width-1, height-1])
    weight = position-lower
    a = pixels[lower[..., 1], lower[..., 0]]
    b = pixels[lower[..., 1], upper[..., 0]]
    c = pixels[upper[..., 1], lower[..., 0]]
    d = pixels[upper[..., 1], upper[..., 0]]
    return ((a*(1-weight[..., 0, None])+b*weight[..., 0, None])*(1-weight[..., 1, None]) +
            (c*(1-weight[..., 0, None])+d*weight[..., 0, None])*weight[..., 1, None])


def compute_smith_beckmann(cosine, alpha):
    sine = np.sqrt(np.maximum(0, 1-cosine*cosine))
    a = np.abs(cosine)/np.maximum(alpha*sine, np.finfo(float).tiny)
    bounded = np.minimum(a, 1.6)
    return np.where(a >= 1.6, 1, (3.535*bounded+2.181*bounded*bounded)/(1+2.276*bounded+2.577*bounded*bounded))


def compute_fresnel(cosine, eta, extinction):
    cosine = np.asarray(cosine)[..., None]
    sine_squared = 1-cosine*cosine
    difference = eta*eta-extinction*extinction-sine_squared
    magnitude = np.sqrt(difference*difference+4*eta*eta*extinction*extinction)
    real = np.sqrt(np.maximum(0, (magnitude+difference)*.5))
    first = magnitude+cosine*cosine
    second = 2*cosine*real
    perpendicular = (first-second)/(first+second)
    third = cosine*cosine*magnitude+sine_squared*sine_squared
    fourth = second*sine_squared
    return .5*perpendicular*(1+(third-fourth)/(third+fourth))


class Scene:
    """Exact published rectangle geometry and unfiltered level-zero slope map."""
    def __init__(self, directory):
        import OpenEXR
        from PIL import Image
        self.hashes = verify_assets(directory)
        self.directory = directory
        self.xml = ET.parse(directory / 'plane_sms.xml').getroot()
        self.defaults = {node.attrib['name']: node.attrib['value'] for node in self.xml.findall('default')}
        self.transforms = np.array([parse_transform(node.find('transform'), self.defaults) for node in self.xml.findall('shape')])
        self.inverse_transforms = np.linalg.inv(self.transforms)
        self.normals = normalize(np.cross(self.transforms[:, :3, 0], self.transforms[:, :3, 1]))
        self.areas = 4*np.linalg.norm(np.cross(self.transforms[:, :3, 0], self.transforms[:, :3, 1]), axis=1)
        self.camera = parse_transform(self.xml.find('sensor/transform'))
        self.field_of_view = float(self.xml.find("sensor/float[@name='fov']").attrib['value'])
        self.radiance = float(self.xml.find("shape/emitter/spectrum[@name='radiance']").attrib['value'])
        pixels = OpenEXR.File(str(directory / 'textures/normalmap_gaussian.exr')).channels()['RGB'].pixels.astype(np.float64)
        decoded = pixels*2-1
        if not np.all(np.isfinite(decoded)) or np.any(np.abs(decoded[..., 2]) < 1e-12):
            raise ValueError('The normal map contains nonfinite or horizontal normals.')
        self.slopes = -decoded[..., :2]/decoded[..., 2, None]
        self.mean_slopes = self.slopes.mean(axis=(0, 1))
        srgb = np.asarray(Image.open(directory / 'textures/[2K]Tiles71/Tiles71_col.jpg').convert('RGB'), dtype=np.float64)/255
        self.floor = np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4)

    def make_primary_rays(self, extent):
        rows, columns = np.mgrid[:extent, :extent]
        scale = np.tan(np.radians(self.field_of_view)*.5)
        local = np.stack((1-2*(columns+.5)/extent, 1-2*(rows+.5)/extent, np.ones_like(rows)), axis=-1)
        local[..., :2] *= scale
        return np.broadcast_to(self.camera[:3, 3], local.shape), normalize(local @ self.camera[:3, :3].T)

    def intersect(self, origin, direction):
        shape = np.broadcast_shapes(np.shape(origin)[:-1], np.shape(direction)[:-1])
        origin = np.broadcast_to(origin, shape+(3,))
        direction = np.broadcast_to(direction, shape+(3,))
        distance = np.full(shape, np.inf)
        identity = np.full(shape, -1, dtype=np.int32)
        coordinates = np.zeros(shape+(2,))
        for index, inverse in enumerate(self.inverse_transforms):
            local_origin = origin @ inverse[:3, :3].T+inverse[:3, 3]
            local_direction = direction @ inverse[:3, :3].T
            t = np.divide(-local_origin[..., 2], local_direction[..., 2], out=np.full(shape, np.inf), where=np.abs(local_direction[..., 2]) > 1e-12)
            with np.errstate(invalid='ignore'):
                local = local_origin[..., :2]+t[..., None]*local_direction[..., :2]
            valid = (t > 1e-5) & (t < distance) & np.all(np.abs(local) <= 1, axis=-1)
            distance = np.where(valid, t, distance)
            identity = np.where(valid, index, identity)
            coordinates = np.where(valid[..., None], (local+1)*.5, coordinates)
        return distance, identity, coordinates

    def make_frame(self, coordinates):
        slope = interpolate_map(self.slopes, coordinates)
        base = normalize(self.transforms[1, :3, :3].T)
        normal = normalize(np.concatenate((-slope, np.ones(slope.shape[:-1]+(1,))), axis=-1) @ base)
        tangent = normalize(base[0]-np.sum(normal*base[0], axis=-1, keepdims=True)*normal)
        return np.stack((tangent, np.cross(normal, tangent), normal), axis=-2)

    def evaluate_beckmann(self, coordinates, view_world, light_world, alpha, eta, extinction):
        frame = self.make_frame(coordinates)
        view = np.einsum('...ij,...j->...i', frame, view_world)
        light = np.einsum('...ij,...j->...i', frame, light_world)
        halfway = normalize(view+light)
        cosine = halfway[..., 2]
        cosine_squared = np.maximum(cosine*cosine, np.finfo(float).tiny)
        distribution = np.exp(-np.sum(halfway[..., :2]**2, axis=-1)/(alpha*alpha*cosine_squared))/(np.pi*alpha*alpha*cosine_squared*cosine_squared)
        visibility = compute_smith_beckmann(view[..., 2], alpha)*compute_smith_beckmann(light[..., 2], alpha)
        fresnel = compute_fresnel(np.abs(np.sum(view*halfway, axis=-1)), np.asarray(eta), np.asarray(extinction))
        value = fresnel*(distribution*visibility/(4*np.maximum(view[..., 2], np.finfo(float).tiny)))[..., None]
        valid = ((view[..., 2] > 0) & (light[..., 2] > 0) & (cosine > 0) &
                 (np.sum(light_world*self.normals[1], axis=-1) > 0) &
                 (np.sum(view_world*self.normals[1], axis=-1) > 0))
        return np.where(valid[..., None], value, 0)


def read_gold_ior(upstream):
    """Reproduce the reference RGB conversion of the pinned gold optical data."""
    for name, expected in SOURCE['representation_files'].items():
        path = upstream / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Missing or modified reference optical data: {path}; restore the pinned source/submodule.')
    source = (upstream / 'src/libcore/spectrum.cpp').read_text()
    table = source.split('static const Float cie1931_tbl', 1)[1].split('};', 1)[0]
    cie = np.array([float(value) for value in re.findall(r'Float\(([^)]+)\)', table)]).reshape(3, -1)
    wavelengths = np.linspace(360, 830, 1000)
    sampled_cie = np.stack([np.interp(wavelengths, np.linspace(360, 830, cie.shape[1]), row) for row in cie], axis=-1)
    matrix = np.array([[3.240479, -1.537150, -.498535], [-.969256, 1.875991, .041556], [.055648, -.204043, 1.057311]])
    colors = []
    for suffix in ('eta', 'k'):
        data = np.loadtxt(upstream / f'resources/data/ior/Au.{suffix}.spd')
        values = np.interp(wavelengths, data[:, 0], data[:, 1], left=0, right=0)/106.7502593994140625
        colors.append(np.maximum(0, matrix @ np.sum(sampled_cie*values[:, None], axis=0)*470/1000))
    return colors
