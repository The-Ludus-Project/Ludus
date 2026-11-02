"""Finite-branch slice proposal using an explicit affine inverse-map surrogate.

Thanks to Shlomi Steinberg, 'Slice Sampling: Importance sampling with an auxiliary
map', draft, sections 3--5: retain null outcomes and enumerate every branch of
the declared seed map. This diagnostic replaces the nonlinear inverse-BSDF map
by a continuous triangulated interpolant. Its density is for that interpolant,
not a Newton approximation to the physical map. The physical BSDF stays intact.
"""
import numpy as np

from scene import normalize

from layout import DEFENSIVE_FRACTION, MINIMUM_TRIANGLE_DETERMINANT, ACCELERATION_GRID_SIDE


def evaluate_residual(scene, receiver, coordinates, light):
    coordinates = np.asarray(coordinates)
    local = np.concatenate((2*coordinates-1, np.zeros(coordinates.shape[:-1]+(1,))), axis=-1)
    position = local @ scene.transforms[1, :3, :3].T+scene.transforms[1, :3, 3]
    halfway = normalize(normalize(receiver-position)+normalize(light-position))
    frame = scene.make_frame(coordinates)
    local_halfway = np.einsum('...ij,...j->...i', frame, halfway)
    # A positive hemisphere chart is required. A large finite sentinel gives a
    # deliberately poor but supported proposal on back-facing patches; it never
    # changes the BSDF or fills the image with invented illumination.
    return local_halfway[..., :2]/np.maximum(local_halfway[..., 2, None], 1e-6)


class AffineSlice:
    def __init__(self, vertices):
        """vertices is a square (resolution+1)^2 grid of 2D residual values."""
        self.vertices = np.asarray(vertices, dtype=np.float64)
        self.resolution = self.vertices.shape[0]-1
        if self.resolution < 1 or self.vertices.shape != (self.resolution+1, self.resolution+1, 2):
            raise ValueError('Use a square residual grid with at least one cell.')
        if not np.all(np.isfinite(self.vertices)):
            raise ValueError('Residual grid must be finite.')
        row, column = np.mgrid[:self.resolution, :self.resolution]
        origin = np.stack((column, row), axis=-1)/self.resolution
        first = np.stack((self.vertices[:-1, :-1], self.vertices[:-1, 1:], self.vertices[1:, :-1]), axis=-2)
        second = np.stack((self.vertices[1:, 1:], self.vertices[1:, :-1], self.vertices[:-1, 1:]), axis=-2)
        self.triangles = np.stack((first, second), axis=-3).reshape(-1, 3, 2)
        self.origins = np.stack((origin, origin+1/self.resolution), axis=-2).reshape(-1, 2)
        self.signs = np.tile([1, -1], self.resolution*self.resolution)
        self.first = self.triangles[:, 1]-self.triangles[:, 0]
        self.second = self.triangles[:, 2]-self.triangles[:, 0]
        self.determinants = self.first[:, 0]*self.second[:, 1]-self.first[:, 1]*self.second[:, 0]
        self.is_regular = np.abs(self.determinants)*self.resolution**2 >= MINIMUM_TRIANGLE_DETERMINANT
        self.weights = np.divide(1, np.abs(self.determinants)*self.resolution**2,
                                 out=np.zeros(len(self.triangles)), where=self.is_regular)

    @classmethod
    def from_scene(cls, scene, receiver, resolution):
        row, column = np.mgrid[:resolution+1, :resolution+1]
        coordinates = np.stack((column, row), axis=-1)/resolution
        # The source map wraps at exactly one; use the left limit for the guide
        # boundary. Physical map evaluation and interior texture filtering stay
        # unchanged. Guides may differ from the integrand.
        coordinates = np.minimum(coordinates, np.nextafter(1., 0.))
        return cls(evaluate_residual(scene, receiver, coordinates, scene.transforms[0, :3, 3]))

    def enumerate_roots(self, residual):
        difference = np.asarray(residual)-self.triangles[:, 0]
        denominator = np.where(self.is_regular, self.determinants, 1)
        first = (difference[:, 0]*self.second[:, 1]-difference[:, 1]*self.second[:, 0])/denominator
        second = (self.first[:, 0]*difference[:, 1]-self.first[:, 1]*difference[:, 0])/denominator
        # Half-open ownership: the upper triangle owns the shared diagonal;
        # right/top cell edges belong to the adjacent cell. This also makes
        # chart ownership explicit; floating-point cancellation on chart edges
        # still prevents a certified finite-precision density claim.
        is_lower = self.signs > 0
        is_inside = self.is_regular & np.where(is_lower,
            (first >= 0) & (second >= 0) & (first+second < 1),
            (first > 0) & (second > 0) & (first+second <= 1))
        identities = np.flatnonzero(is_inside)
        coordinates = self.origins[is_inside] + self.signs[is_inside, None]*np.c_[first[is_inside], second[is_inside]]/self.resolution
        return identities, coordinates, self.weights[is_inside]

    def evaluate_map(self, coordinates):
        position = np.clip(np.asarray(coordinates)*self.resolution, 0, np.nextafter(float(self.resolution), 0.))
        column, row = np.floor(position).astype(int)
        fraction = position-[column, row]
        is_second = bool(fraction.sum() >= 1)
        identity = (row*self.resolution+column)*2+int(is_second)
        barycentric = 1-fraction if is_second else fraction
        return self.triangles[identity, 0]+barycentric[0]*self.first[identity]+barycentric[1]*self.second[identity], identity

    def evaluate_pdf(self, coordinates, alpha):
        residual, identity = self.evaluate_map(coordinates)
        if not self.is_regular[identity]:
            return DEFENSIVE_FRACTION
        _, _, weights = self.enumerate_roots(residual)
        weight = weights.sum()
        if weight <= 0:
            raise ValueError('Affine map replay lost its own regular triangle.')
        guide = np.exp(-np.dot(residual, residual)/(alpha*alpha))/(np.pi*alpha*alpha)
        return DEFENSIVE_FRACTION+(1-DEFENSIVE_FRACTION)*guide/weight

    def sample(self, residual, branch_uniform, alpha):
        identities, coordinates, weights = self.enumerate_roots(residual)
        weight = weights.sum()
        if weight == 0:
            return None, 0., 0
        selected = min(int(np.searchsorted(np.cumsum(weights), branch_uniform*weight, side='right')), len(weights)-1)
        guide = np.exp(-np.dot(residual, residual)/(alpha*alpha))/(np.pi*alpha*alpha)
        density = DEFENSIVE_FRACTION+(1-DEFENSIVE_FRACTION)*guide/weight
        return coordinates[selected], density, len(identities)

    def build_acceleration(self):
        """Conservative seed-space bins; every triangle is inserted by its AABB."""
        vertices = self.triangles.astype(np.float32)
        lower = np.nextafter(vertices.min(axis=(0, 1)), np.float32(-np.inf))
        upper = np.nextafter(vertices.max(axis=(0, 1)), np.float32(np.inf))
        if np.any(upper <= lower):
            upper = lower+1
        first = np.floor((vertices.min(axis=1)-lower)/(upper-lower)*ACCELERATION_GRID_SIDE).astype(int)
        last = np.floor((vertices.max(axis=1)-lower)/(upper-lower)*ACCELERATION_GRID_SIDE).astype(int)
        first = np.clip(first-1, 0, ACCELERATION_GRID_SIDE-1)
        last = np.clip(last+1, 0, ACCELERATION_GRID_SIDE-1)
        bins = [[] for _ in range(ACCELERATION_GRID_SIDE**2)]
        for triangle in range(len(vertices)):
            if not self.is_regular[triangle]:
                continue
            for row in range(first[triangle, 1], last[triangle, 1]+1):
                for column in range(first[triangle, 0], last[triangle, 0]+1):
                    bins[row*ACCELERATION_GRID_SIDE+column].append(triangle)
        records = []
        identities = []
        for values in bins:
            records.append([len(identities), len(values)])
            identities.extend(values)
        return np.r_[lower, upper].astype(np.float32), np.asarray(records, dtype=np.uint32), np.asarray(identities, dtype=np.uint32)
