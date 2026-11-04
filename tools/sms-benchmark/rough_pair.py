"""Float64 oracle for the fixed-endpoint, two-rough-reflection benchmark.

Thanks to Shlomi Steinberg, Slice Sampling: Importance sampling with an auxiliary
map (University of Waterloo draft), sections 4 and 7, for the coupled seed law.
Thanks to Eric Veach, Robust Monte Carlo Methods for Light Transport Simulation,
chapters 9–10 (https://graphics.stanford.edu/papers/veach_thesis/), for the BDPT
connection strategies and one-sample balance heuristic.
"""

import numpy as np

SOURCE_RECEIVER_OFFSET = float(np.float32(0.4))


def warp(slope, curvature):
    return slope * (1 + curvature * np.abs(slope))


def derivative(slope, curvature):
    return 1 + 2 * curvature * np.abs(slope)


def unwarp(value, curvature):
    return 2 * value / (1 + np.sqrt(1 + 4 * curvature * np.abs(value)))


def gaussian(value, width):
    return np.exp(-np.sum(value**2, axis=-1) / (2 * width**2)) / (2 * np.pi * width**2)


def projected_density(slope):
    return 1 / (np.pi * (1 + np.sum(slope**2, axis=-1))**2)


def normals(uniform):
    radius = np.sqrt(-2 * np.log(uniform[..., 0]))
    angle = 2 * np.pi * uniform[..., 1]
    return radius[..., None] * np.stack((np.cos(angle), np.sin(angle)), axis=-1)


def cosine_slopes(uniform):
    radius = np.sqrt(uniform[..., 0] / (1 - uniform[..., 0]))
    angle = 2 * np.pi * uniform[..., 1]
    return radius[..., None] * np.stack((np.cos(angle), np.sin(angle)), axis=-1)


def evaluate(first, second, receiver, light, width, curvature):
    """Return the physical contribution and proposal densities per dA1 dA2."""
    camera_slope = first - receiver
    light_slope = second - light
    middle_slope = first - second
    middle_warped = warp(middle_slope, curvature)
    first_lobe = gaussian(warp(camera_slope, curvature) + middle_warped, width)
    second_lobe = gaussian(warp(light_slope, curvature) - middle_warped, width)
    camera_density = projected_density(camera_slope)
    light_density = projected_density(light_slope)
    middle_density = projected_density(middle_slope)
    contribution = first_lobe * second_lobe * camera_density * light_density * middle_density * np.pi**3
    middle_derivative = derivative(middle_slope, curvature)
    camera_derivative = derivative(camera_slope, curvature)
    light_derivative = derivative(light_slope, curvature)
    determinant = np.prod(camera_derivative * light_derivative +
                          middle_derivative * (camera_derivative + light_derivative), axis=-1)
    slice_density = first_lobe * second_lobe * determinant
    middle_jacobian = np.prod(middle_derivative, axis=-1)
    bdpt_density = (camera_density * light_density + middle_jacobian *
                    (camera_density * first_lobe + light_density * second_lobe)) / 3
    return contribution, slice_density, bdpt_density


def solve(first_seed, second_seed, receiver, light, curvature):
    """Solve two monotone scalar equations by bisection, independently of GPU Newton."""
    # At each axis the zero is bracketed between the two uncoupled roots'
    # difference and zero. Beyond this interval the residual is strictly signed.
    difference = receiver - light + unwarp(first_seed, curvature) - unwarp(second_seed, curvature)
    lower = np.minimum(difference, 0)
    upper = np.maximum(difference, 0)
    for _ in range(55):
        middle = (lower + upper) * 0.5
        middle_warped = warp(middle, curvature)
        first = receiver + unwarp(first_seed - middle_warped, curvature)
        second = light + unwarp(second_seed + middle_warped, curvature)
        is_above = middle > first - second
        upper = np.where(is_above, middle, upper)
        lower = np.where(is_above, lower, middle)
    middle_warped = warp((lower + upper) * 0.5, curvature)
    return (receiver + unwarp(first_seed - middle_warped, curvature),
            light + unwarp(second_seed + middle_warped, curvature))


def sample(method, draws, receiver, light, width, curvature, half_extent):
    first_draw = draws[..., :2]
    second_draw = draws[..., 2:4]
    if method == 'slice':
        first, second = solve(width * normals(first_draw), width * normals(second_draw),
                              receiver, light, curvature)
    else:
        first = receiver + cosine_slopes(first_draw)
        second = light + cosine_slopes(second_draw)
        split = draws[..., 4, None]
        first = np.where(split >= 2/3, second - unwarp(width * normals(first_draw) -
                         warp(second - light, curvature), curvature), first)
        second = np.where((split >= 1/3) & (split < 2/3), first - unwarp(width * normals(second_draw) -
                          warp(first - receiver, curvature), curvature), second)
    contribution, slice_density, bdpt_density = evaluate(first, second, receiver, light, width, curvature)
    density = slice_density if method == 'slice' else bdpt_density
    is_on_patch = (np.maximum(np.abs(first), np.abs(second)) <= half_extent).all(axis=-1)
    estimate = np.divide(contribution, density, out=np.zeros_like(density), where=is_on_patch & (density > 0))
    return first, second, np.where(is_on_patch, density, 0), estimate


def partition(lower, upper, seams, nodes, weights):
    edges = np.sort(np.stack((np.full_like(seams[..., 0], lower),
                              *np.moveaxis(np.clip(seams, lower, upper), -1, 0),
                              np.full_like(seams[..., 0], upper)), axis=-1), axis=-1)
    half_width = 0.5 * np.diff(edges, axis=-1)
    center = (edges[..., :-1] + edges[..., 1:]) * 0.5
    shape = (*center.shape[:-1], -1)
    return ((center[..., None] + half_width[..., None] * nodes).reshape(shape),
            (half_width[..., None] * weights).reshape(shape))


def quadrature_axis(receiver, light, width, curvature, extent, nodes, weights):
    # Split both nested integrations at every sign change of a warped slope.
    # First-position intervals also split at the light coordinate, where the
    # order of the two inner seams changes.
    first, first_weight = partition(-extent, extent, np.array([receiver, light]), nodes, weights)
    second, second_weight = partition(-extent, extent, np.column_stack((first, np.full_like(first, light))),
                                      nodes, weights)
    camera_slope = first[:, None] - receiver
    light_slope = second - light
    middle_slope = first[:, None] - second
    first_residual = warp(camera_slope, curvature) + warp(middle_slope, curvature)
    second_residual = warp(light_slope, curvature) - warp(middle_slope, curvature)
    variance = width**2
    lobe_weight = (np.exp(-(first_residual**2 + second_residual**2) / (2*variance)) /
                   (2*np.pi*variance)) * first_weight[:, None] * second_weight
    return (np.broadcast_to(camera_slope**2, second.shape).ravel(), light_slope.ravel()**2,
            middle_slope.ravel()**2, lobe_weight.ravel())


def quadrature(receivers, light, width, curvature, half_extent, order):
    """Integrate transport on the two patches without sampling or density evaluation."""
    nodes, weights = np.polynomial.legendre.leggauss(order)
    result = np.zeros(len(receivers))
    for index, receiver in enumerate(receivers):
        camera_x, light_x, middle_x, weight_x = quadrature_axis(
            receiver[0], light[0], width, curvature, half_extent, nodes, weights)
        camera_y, light_y, middle_y, weight_y = quadrature_axis(
            receiver[1], light[1], width, curvature, half_extent, nodes, weights)
        block_size = 64
        for offset in range(0, len(weight_x), block_size):
            block = slice(offset, offset+block_size)
            # Each edge contributes cos(theta)^2 / distance^2, which
            # is (1 + slope_x^2 + slope_y^2)^(-2) at unit plane separation.
            product = ((1 + camera_x[block, None] + camera_y) *
                       (1 + light_x[block, None] + light_y) *
                       (1 + middle_x[block, None] + middle_y))
            result[index] += np.sum((weight_x[block, None] * weight_y) / product**2)
    return result


def replay(first, second, receiver, light, curvature):
    middle = warp(first-second, curvature)
    return warp(first-receiver, curvature)+middle, warp(second-light, curvature)-middle


def shift(first, second, source, target, light, curvature):
    return solve(*replay(first, second, source, light, curvature), target, light, curvature)


def sample_shift(draws, receiver, light, width, curvature, half_extent):
    """Push a source proposal through the partial bijection and mix with fresh."""
    source = receiver - [SOURCE_RECEIVER_OFFSET, 0]
    is_shift = draws[..., 4] < 0.5
    draw_receiver = np.where(is_shift[..., None], source, receiver)
    map_receiver = np.where(is_shift[..., None], receiver, source)
    first, second, draw_density, _ = sample('slice', draws, draw_receiver, light, width, curvature, half_extent)
    mapped_first, mapped_second = shift(first, second, draw_receiver, map_receiver, light, curvature)
    is_mapped_on_patch = (np.maximum(np.abs(mapped_first), np.abs(mapped_second)) <= half_extent).all(axis=-1)
    is_moved = is_shift & (draw_density > 0) & is_mapped_on_patch
    first = np.where(is_moved[..., None], mapped_first, first)
    second = np.where(is_moved[..., None], mapped_second, second)
    physical, target_density, _ = evaluate(first, second, receiver, light, width, curvature)
    is_valid = (draw_density > 0) & (~is_shift | is_moved)
    density = np.where(is_valid, target_density * (0.5 + 0.5 * is_mapped_on_patch), 0)
    estimate = np.divide(physical, density, out=np.zeros_like(density), where=density > 0)
    # The base Gaussian factors cancel on matched seeds. This independent
    # density ratio checks the shader's derivative-only Jacobian.
    jacobian = np.divide(draw_density, target_density, out=np.zeros_like(density), where=is_moved)
    return first, second, density, estimate, jacobian


def sample_cached_grid(draws, receivers, grid_width, light, width, curvature, half_extent):
    """Return fresh, neighbor, and balance-weighted estimates for each lane.

    Draws have receiver-major order with one column group per independent
    replica. The CPU generates all source paths once and uses its own bisection
    to evaluate forward and reverse maps.
    """
    receiver_count = len(receivers)
    if receiver_count != grid_width**2 or draws.shape[0] != receiver_count:
        raise ValueError('Cached grid requires one draw block per receiver in a square grid.')
    indices = np.arange(receiver_count)
    column = indices % grid_width
    has_neighbor = (column ^ 1) < grid_width
    neighbor = np.where(has_neighbor, indices + (column ^ 1) - column, indices)
    target = receivers[:, None, :]
    source = receivers[neighbor, None, :]
    fresh_first, fresh_second, fresh_density, fresh = sample(
        'slice', draws, target, light, width, curvature, half_extent)
    source_first, source_second = fresh_first[neighbor], fresh_second[neighbor]
    shifted_first, shifted_second = shift(source_first, source_second, source, target, light, curvature)
    reverse_first, reverse_second = shift(fresh_first, fresh_second, target, source, light, curvature)
    is_in_reverse = (np.maximum(np.abs(reverse_first), np.abs(reverse_second)) <= half_extent).all(axis=-1)
    is_in_target = (np.maximum(np.abs(shifted_first), np.abs(shifted_second)) <= half_extent).all(axis=-1)
    is_shifted = has_neighbor[:, None] & (fresh_density[neighbor] > 0) & is_in_target
    physical, target_density, _ = evaluate(shifted_first, shifted_second, target, light, width, curvature)
    shifted_density = np.where(is_shifted, target_density, 0)
    shifted = np.divide(physical, shifted_density, out=np.zeros_like(physical), where=is_shifted)
    jacobian = np.divide(fresh_density[neighbor], target_density, out=np.zeros_like(physical), where=is_shifted)
    weight = np.where(has_neighbor[:, None] & is_in_reverse & (fresh_density > 0), 0.5, 1)
    return dict(fresh_positions=np.concatenate((fresh_first, fresh_second), axis=-1),
                fresh_density=fresh_density, fresh=fresh,
                shifted_positions=np.concatenate((shifted_first, shifted_second), axis=-1),
                shifted_density=shifted_density, shifted=shifted, jacobian=jacobian,
                weight=weight, radiance=weight * fresh + 0.5 * shifted,
                has_neighbor=has_neighbor,
                null_source=has_neighbor[:, None] & (fresh_density[neighbor] == 0),
                off_target=has_neighbor[:, None] & (fresh_density[neighbor] > 0) & ~is_in_target)
