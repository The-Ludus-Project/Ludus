"""CPU oracle for a two-stage offset-normal SMS connection sampler.

Thanks to Zeltner, Georgiev and Jakob, SIGGRAPH 2020, sections 4.1--4.5,
https://doi.org/10.1145/3386569.3392408. This port uses slope constraints and
an analytic Jacobian of the composed offset-normal map. Its Newton basins need
not match the authors' angle-difference solver; repeated independent re-hit
trials estimate this implementation's own basin probability.
"""
import numpy as np

from connection import evaluate_reflection_map

from layout import (SOLVER_ITERATIONS, SOLVER_TOLERANCE, MINIMUM_MAP_DETERMINANT,
                    REHIT_DISTANCE_UV, MAXIMUM_BERNOULLI_TRIALS)


def solve(scene, receiver, light, target, coordinates, is_smoothed):
    coordinates = np.array(coordinates, dtype=np.float64)
    beta = 1.
    for _ in range(SOLVER_ITERATIONS):
        residual, derivative, is_valid = evaluate_reflection_map(scene, receiver, light, coordinates, is_smoothed, True)
        if not is_valid or not np.all(np.isfinite(residual)) or not np.all(np.isfinite(derivative)):
            return None
        difference = residual-target
        if np.linalg.norm(difference) < SOLVER_TOLERANCE:
            return coordinates
        if abs(np.linalg.det(derivative)) < MINIMUM_MAP_DETERMINANT:
            return None
        candidate = coordinates-beta*np.linalg.solve(derivative, difference)
        if np.any(candidate < 0) or np.any(candidate >= 1):
            beta *= .5
            continue
        coordinates = candidate
        beta = min(1, 2*beta)
    return None


def prepare_smoothing(scene):
    slopes = scene.slopes.reshape(-1, 2)
    mean = slopes.mean(axis=0)
    covariance = np.cov(slopes.T, bias=True)+np.eye(2)*.5e-8
    return mean, np.linalg.cholesky(covariance)


def solve_two_stage(scene, receiver, light, target, initial_uv, normal_draw, smoothing):
    mean, cholesky = smoothing
    # The released normal-map plugin returns a global LEAN level for a seed
    # ray without UV differentials. Offset slopes there have the opposite sign
    # to the positive halfway-vector slope used by this map.
    smooth_target = -.5*(mean-target)-cholesky @ normal_draw
    first = solve(scene, receiver, light, smooth_target, initial_uv, True)
    return None if first is None else solve(scene, receiver, light, target, first, False)


def solve_many(scene, receiver, light, target, initial_uv, is_smoothed):
    """Vectorized independent scalar solves; preserves each solve's own state."""
    coordinates = np.array(initial_uv, dtype=np.float64)
    receiver = np.broadcast_to(receiver, (len(coordinates), 3))
    light = np.broadcast_to(light, (len(coordinates), 3))
    active = np.ones(len(coordinates), dtype=bool)
    successful = np.zeros(len(coordinates), dtype=bool)
    beta = np.ones(len(coordinates))
    for _ in range(SOLVER_ITERATIONS):
        indices = np.flatnonzero(active)
        if len(indices) == 0:
            break
        residual, derivative, valid = evaluate_reflection_map(scene, receiver[indices], light[indices], coordinates[indices], is_smoothed, True)
        valid &= np.all(np.isfinite(residual), axis=1) & np.all(np.isfinite(derivative), axis=(1, 2))
        difference = residual-target[indices]
        converged = valid & (np.linalg.norm(difference, axis=1) < SOLVER_TOLERANCE)
        successful[indices[converged]] = True
        determinant = derivative[:, 0, 0]*derivative[:, 1, 1]-derivative[:, 0, 1]*derivative[:, 1, 0]
        can_step = valid & ~converged & (np.abs(determinant) >= MINIMUM_MAP_DETERMINANT)
        active[indices[~can_step]] = False
        indices = indices[can_step]
        if len(indices) == 0:
            continue
        derivative = derivative[can_step]
        difference = difference[can_step]
        determinant = determinant[can_step]
        step = np.c_[difference[:, 0]*derivative[:, 1, 1]-difference[:, 1]*derivative[:, 0, 1],
                     derivative[:, 0, 0]*difference[:, 1]-derivative[:, 1, 0]*difference[:, 0]]/determinant[:, None]
        candidate = coordinates[indices]-beta[indices, None]*step
        inside = np.all((candidate >= 0) & (candidate < 1), axis=1)
        beta[indices[~inside]] *= .5
        beta[indices[inside]] = np.minimum(1, 2*beta[indices[inside]])
        coordinates[indices[inside]] = candidate[inside]
    return coordinates, successful


def solve_two_stage_many(scene, receiver, light, target, initial_uv, normal_draw, smoothing):
    mean, cholesky = smoothing
    smooth_target = -.5*(mean-target)-normal_draw @ cholesky.T
    first, success = solve_many(scene, receiver, light, smooth_target, initial_uv, True)
    second = np.zeros_like(first)
    indices = np.flatnonzero(success)
    if len(indices) > 0:
        receiver = np.broadcast_to(receiver, (len(first), 3))
        light = np.broadcast_to(light, (len(first), 3))
        second[indices], second_success = solve_many(scene, receiver[indices], light[indices], target[indices], first[indices], False)
        success[indices] = second_success
    return second, success
