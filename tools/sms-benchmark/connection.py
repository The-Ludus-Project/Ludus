"""CPU oracle for the camera-floor-gold-light contribution in Figure 17.

The full published integrator includes other path classes. This explicitly
isolated contribution permits matched CPU/GPU checks without changing assets.
Thanks to Zeltner, Georgiev and Jakob, SIGGRAPH 2020, section 4.5 and Figure 17,
https://doi.org/10.1145/3386569.3392408, for the offset-normal SMS construction.
"""
import numpy as np

from scene import normalize, interpolate_map


def make_receivers(scene, extent):
    origins, directions = scene.make_primary_rays(extent)
    distances, identities, coordinates = scene.intersect(origins, directions)
    positions = origins+distances[..., None]*directions
    colors = interpolate_map(scene.floor, coordinates*40)
    return positions.reshape(-1, 3), colors.reshape(-1, 3), (identities == 2).ravel()


def make_point(transform, coordinates):
    local = np.concatenate((2*np.asarray(coordinates)-1, np.zeros(np.shape(coordinates)[:-1]+(1,))), axis=-1)
    return local @ transform[:3, :3].T+transform[:3, 3]


def evaluate_connection(scene, receiver, color, coordinates, light_coordinates, alpha, eta, extinction):
    position = make_point(scene.transforms[1], coordinates)
    light_position = make_point(scene.transforms[0], light_coordinates)
    to_receiver = receiver-position
    to_light = light_position-position
    view = normalize(to_receiver)
    light = normalize(to_light)
    cos_floor = np.maximum(0, -np.sum(view*scene.normals[2], axis=-1))
    cos_reflector = np.maximum(0, np.sum(view*scene.normals[1], axis=-1))
    cos_emitter = np.maximum(0, -np.sum(light*scene.normals[0], axis=-1))
    solid_angle_uv = scene.areas[1]*cos_reflector/np.sum(to_receiver**2, axis=-1)
    emitted = scene.radiance*scene.areas[0]*cos_emitter/np.sum(to_light**2, axis=-1)
    bsdf = scene.evaluate_beckmann(coordinates, view, light, alpha, eta, extinction)
    return np.asarray(color)/np.pi*bsdf*(cos_floor*solid_angle_uv*emitted)[..., None]


def compute_normalized_derivative(value, derivative):
    unit = normalize(value)
    return (derivative-unit*np.sum(unit*derivative, axis=-1, keepdims=True))/np.linalg.norm(value, axis=-1, keepdims=True)


def evaluate_reflection_map(scene, receiver, light, coordinates, is_smoothed=False, return_valid=False):
    """Analytic UV Jacobian, within each clamped bilinear slope-map cell."""
    coordinates = np.asarray(coordinates)
    if is_smoothed:
        slope = np.broadcast_to(scene.mean_slopes, coordinates.shape)
        slope_u = np.zeros_like(slope)
        slope_v = np.zeros_like(slope)
    else:
        resolution = scene.slopes.shape[0]
        position = np.clip((coordinates-np.floor(coordinates))*resolution-.5, 0, resolution-1)
        lower = np.floor(position).astype(int)
        upper = np.minimum(lower+1, resolution-1)
        weight = position-lower
        a = scene.slopes[lower[..., 1], lower[..., 0]]
        b = scene.slopes[lower[..., 1], upper[..., 0]]
        c = scene.slopes[upper[..., 1], lower[..., 0]]
        d = scene.slopes[upper[..., 1], upper[..., 0]]
        slope = (a*(1-weight[..., 0, None])+b*weight[..., 0, None])*(1-weight[..., 1, None])+(c*(1-weight[..., 0, None])+d*weight[..., 0, None])*weight[..., 1, None]
        is_interior = ((coordinates-np.floor(coordinates))*resolution-.5 > 0) & ((coordinates-np.floor(coordinates))*resolution-.5 < resolution-1)
        slope_u = ((b-a)*(1-weight[..., 1, None])+(d-c)*weight[..., 1, None])*resolution*is_interior[..., 0, None]
        slope_v = ((c-a)*(1-weight[..., 0, None])+(d-b)*weight[..., 0, None])*resolution*is_interior[..., 1, None]
    base = normalize(scene.transforms[1, :3, :3].T)
    n_raw = base[2]-slope[..., 0, None]*base[0]-slope[..., 1, None]*base[1]
    normal = normalize(n_raw)
    t_raw = base[0]-normal*np.sum(normal*base[0], axis=-1, keepdims=True)
    tangent = normalize(t_raw)
    bitangent = np.cross(normal, tangent)
    position = make_point(scene.transforms[1], coordinates)
    to_receiver = receiver-position
    to_light = light-position
    halfway_raw = normalize(to_receiver)+normalize(to_light)
    halfway = normalize(halfway_raw)
    numerator = np.stack((np.sum(halfway*tangent, axis=-1), np.sum(halfway*bitangent, axis=-1)), axis=-1)
    denominator = np.sum(halfway*normal, axis=-1)
    residual = numerator/denominator[..., None]
    derivatives = []
    for axis, derivative_slope in enumerate((slope_u, slope_v)):
        dnormal = compute_normalized_derivative(n_raw, -derivative_slope[..., 0, None]*base[0]-derivative_slope[..., 1, None]*base[1])
        dtangent = compute_normalized_derivative(t_raw, -dnormal*np.sum(normal*base[0], axis=-1, keepdims=True)-normal*np.sum(dnormal*base[0], axis=-1, keepdims=True))
        dbitangent = np.cross(dnormal, tangent)+np.cross(normal, dtangent)
        dposition = 2*scene.transforms[1, :3, axis]
        dhalfway_raw = compute_normalized_derivative(to_receiver, -dposition)+compute_normalized_derivative(to_light, -dposition)
        dhalfway = compute_normalized_derivative(halfway_raw, dhalfway_raw)
        dnumerator = np.stack((np.sum(dhalfway*tangent+halfway*dtangent, axis=-1), np.sum(dhalfway*bitangent+halfway*dbitangent, axis=-1)), axis=-1)
        ddenominator = np.sum(dhalfway*normal+halfway*dnormal, axis=-1)
        derivatives.append((dnumerator-residual*ddenominator[..., None])/denominator[..., None])
    result = residual, np.stack(derivatives, axis=-1)
    return (*result, denominator > 1e-6) if return_valid else result
