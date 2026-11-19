"""Analytic surfaces, cooked mesh invariants and double-precision camera projections."""
import math
import struct

SHAPES = ('smooth', 'crease', 'unequal', 'unequal-reversed', 'unequal-crease',
          'unequal-crease-reversed', 'plane', 'sparse', 'bulge', 'mixed')
TOLERANCES = (.035, .006)
EXTENTS = (96, 192)
CAMERAS = ((1, .38, 3, (1, 1, 1)), (-1, .38, 3, (1, 1, 1)), (1, .32, 3.2, (1.4, 1, 1.5)),
           (1, .08, 2.24, (1.4, 1, 1.5)))
FOCAL, NEAR, FAR = 1.6, .25, 8
NORMAL_EPSILON = 2e-6  # LUGEO narrows the authored controls to binary32.


def require(condition, message):
    if not condition:
        raise ValueError(message)


def width(shape, patch):
    if shape == 'mixed':
        return 1.35 if patch == 1 else .65
    return .5 if shape == 'plane' and patch == 2 else 1.


def polynomial(shape, patch):
    """Coefficients of x^2 and x for the cylindrical controls."""
    if shape == 'mixed':
        return (-.04 if patch == 1 else -2), 0.
    if shape in ('smooth', 'crease'):
        return -1., (-.75 if shape == 'crease' and patch == 2 else 0.)
    return 0., 0.


def height(shape, patch, x, y):
    """Authored scalar field and its two spatial partials, independent of Bernstein evaluation."""
    if patch == 2:
        if shape.startswith('unequal'):
            slope = -.75 if 'crease' in shape else 0.
            return -x*x*(1-y*y)+slope*x, -2*x*(1-y*y)+slope, 2*x*x*y
        if shape == 'bulge':
            return 4*x*(1-x)*(1-y*y), 4*(1-2*x)*(1-y*y), -8*x*(1-x)*y
        if shape == 'sparse':
            slope = 2*y*(y*y-1)
            return x*slope, slope, x*(6*y*y-2)
    a, b = polynomial(shape, patch)
    return (a*x+b)*x, 2*a*x+b, 0.


def unit(vector):
    length = math.hypot(*vector)
    return tuple(x/length for x in vector)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def read_mesh(directory):
    records = {key: [] for key in 'pdvf'}
    for line in (directory/'mesh.txt').read_text().splitlines():
        key, *fields = line.split()
        records[key].append(tuple(map(float, fields)))
    return records


def geometry(shape, records, tolerance):
    patches = {int(p): (int(before), int(after), bound, int(count))
               for p, before, after, bound, count in records['p']}
    require(set(patches) == {1, 2}, 'missing incident patch')
    if shape.startswith('unequal') or shape == 'bulge':
        require(patches[1][0] < patches[2][0], 'fixture failed to demand unequal edge levels')
    require(patches[1][1] == patches[2][1], 'unmatched final edge levels')
    seams = {1: {}, 2: {}}
    boundary_normals = {}
    for p, u, v, index, x, y, z, nx, ny, nz in records['v']:
        direction = 1 if p == 1 else -1
        xx = direction*(u-1)*width(shape, p)
        yy = direction*(2*v-1)
        zz, dx, dy = height(shape, p, xx, yy)
        require(math.dist((xx, yy, zz), (x, y, z)) < NORMAL_EPSILON, 'loaded vertex disagrees with analytic surface')
        require(math.dist(unit((-dx, -dy, 1)), (nx, ny, nz)) < NORMAL_EPSILON, 'loaded vertex normal mismatch')
        if u == 1:
            t = v if p == 1 else 1-v
            require(t not in seams[p], 'repeated boundary vertex')
            seams[p][t] = (index, struct.pack('<fff', x, y, z))
            boundary_normals[p, t] = (nx, ny, nz)
    expected = {j/2**patches[1][1] for j in range(2**patches[1][1]+1)}
    require(seams[1].keys() == expected and seams[1] == seams[2], 'canonical edge identity or position bits differ')
    # Thanks to Schmitt and Du, Bezier Patches with Local Shape Control Parameters,
    # EG 1987, sec. 2.3, for the tangent-plane coplanarity criterion along the entire join.
    # https://diglib.eg.org/items/61cf1b5e-05f4-4a87-868b-7f1da4d15019
    derivatives = {}
    for p, v, *sample in records['d']:
        sign = 1 if p == 1 else -1
        y = sign*(2*v-1)
        _, dx, dy = height(shape, p, 0, y)
        speed = sign*width(shape, p)
        du, dv, normal = sample[3:6], sample[6:9], sample[9:12]
        require(math.dist(sample[:3], (0, y, 0)) < NORMAL_EPSILON and
                math.dist(du, (speed, 0, speed*dx)) < NORMAL_EPSILON and
                math.dist(dv, (0, 2*sign, 2*sign*dy)) < NORMAL_EPSILON and
                math.dist(normal, unit((-dx, -dy, 1))) < NORMAL_EPSILON, 'boundary frame disagrees with analytic partials')
        derivatives[p, y] = (du, normal)
    for y in (-1, -.5, 0, .5, 1):
        left, nl = derivatives[1, y]
        right, nr = derivatives[2, y]
        angle = math.atan2(math.hypot(*cross(nl, nr)), sum(a*b for a, b in zip(nl, nr)))
        slope = height(shape, 2, 0, y)[1]
        triple = cross(left, right)[1]  # dot with the unit canonical edge tangent (0,1,0)
        require(abs(angle-abs(math.atan(slope))) < NORMAL_EPSILON and
                abs(triple-width(shape, 1)*width(shape, 2)*slope) < NORMAL_EPSILON, 'seam continuity classification')
    vertices = records['v']
    deviations = {1: 0., 2: 0.}
    counts = {1: 0, 2: 0}
    drawn_edges = {1: [], 2: []}
    for face in records['f']:
        require(all(i.is_integer() and 0 <= i < len(vertices) for i in face), 'invalid triangle index')
        a, b, c = (vertices[int(i)] for i in face)
        p = a[0]
        require(p == b[0] == c[0], 'triangle spans two source patches')
        counts[p] += 1
        for start, end in ((a, b), (b, c), (c, a)):
            if start[1] == end[1] == 1:
                ts = [q[2] if p == 1 else 1-q[2] for q in (start, end)]
                drawn_edges[p].append(tuple(sorted(ts)))
        for weights in ((1/3, 1/3, 1/3), (.5, .5, 0), (.5, 0, .5), (0, .5, .5)):
            x, y, z = (sum(w*q[i] for w, q in zip(weights, (a, b, c))) for i in (4, 5, 6))
            deviations[p] = max(deviations[p], abs(z-height(shape, p, x, y)[0]))
    ordered = sorted(expected)
    segments = list(zip(ordered, ordered[1:]))
    for p, (_, _, bound, count) in patches.items():
        require(sorted(drawn_edges[p]) == segments, 'draw indices skip or repeat a canonical seam segment')
        require(count == counts[p] and deviations[p] <= bound+1e-7 and bound <= tolerance, 'triangle error or budget')
    if shape == 'bulge':
        require(height(shape, 2, .5, 0)[0] > tolerance and 0 < deviations[2] <= tolerance, 'interior bulge missed')
    return {t: bits for t, (_, bits) in seams[1].items()}, boundary_normals


class Projection:
    def __init__(self, camera, extent):
        self.side, slope, self.distance, self.scale = camera
        self.extent = extent
        self.cosine = 1/math.hypot(1, slope)
        self.sine = slope*self.cosine

    def project(self, point):
        x, y, z = (a*b for a, b in zip(point, self.scale))
        w = self.distance + self.side*self.cosine*x - self.sine*z
        u = -self.side*y
        v = self.side*self.sine*x + self.cosine*z
        return ((1+FOCAL*u/w)*self.extent/2, (1-FOCAL*v/w)*self.extent/2, self.depth(w), w)

    @staticmethod
    def depth(w):
        return FAR/(FAR-NEAR)*(1-NEAR/w)

    def intersect(self, shape, column, row):
        # Thanks to J. Kajiya, Ray Tracing Parametric Patches, SIGGRAPH 1982,
        # pp. 245–254 (doi:10.1145/800064.801287), for the direct-hit silhouette comparison.
        # Ray parameter is positive clip W. No face culling, conventional Less depth.
        u = (2*column/self.extent-1)/FOCAL
        v = (1-2*row/self.extent)/FOCAL
        sx, sy, sz = self.scale
        ox = -self.distance*self.side*self.cosine/sx
        oz = self.distance*self.sine/sz
        dx = self.side*(self.cosine+v*self.sine)/sx
        dz = (-self.sine+v*self.cosine)/sz
        hits = []
        for p in (1, 2):
            a, b = polynomial(shape, p)
            for w in roots(a*dx*dx, (2*a*ox+b)*dx-dz, (a*ox+b)*ox-oz):
                x = ox+w*dx
                y = -self.side*u*w/sy
                if NEAR <= w <= FAR and abs(y) <= 1 and 0 <= (x if p == 2 else -x) <= width(shape, p):
                    hits.append(w)
        return min(hits, default=None)

    def silhouette(self, shape, p):
        # At y=0 the projected height is a quadratic divided by positive clip W.
        # Include interior stationary points as well as both patch ends.
        a, b = polynomial(shape, p)
        sx, _, sz = self.scale
        A, B = self.cosine*sz*a, self.side*self.sine*sx+self.cosine*sz*b
        C, D = -self.sine*sz*a, self.side*self.cosine*sx-self.sine*sz*b
        edge = (-1 if p == 1 else 1)*width(shape, p)
        lo, hi = sorted((edge, 0.))
        candidates = [lo, hi] + [x for x in roots(A*D-B*C, 2*A*self.distance, B*self.distance) if lo < x < hi]
        return min((self.project((x, 0, (a*x+b)*x))[1], x) for x in candidates)


def roots(a, b, c):
    if abs(a) < 1e-14:
        return [-c/b] if abs(b) > 1e-14 else []
    discriminant = b*b-4*a*c
    if discriminant < 0:
        return []
    q = -.5*(b+math.copysign(math.sqrt(discriminant), b))
    return [q/a, c/q] if q else [0.]


# Thanks to Abi-Ezzi and Shirman, Tessellation of Curved Surfaces under Highly
# Varying Transformations, EG 1991, secs. 3–4, for separating affine stretch
# from depth-dependent perspective magnification.
# https://diglib.eg.org/items/7f89a6e1-e831-41ec-8391-4922e01d202f
def projected_bound(view, hull, model_bound):
    """Lipschitz bound for perspective division on the entire convex comparison hull."""
    points = [(x, y, z) for x in hull[0] for y in hull[1] for z in hull[2]]
    w_min = min(view.distance + view.side*view.cosine*x*view.scale[0] - view.sine*z*view.scale[2]
                for x, _, z in points)
    if w_min <= NEAR:
        return None
    corners = [view.project(point) for point in points]
    # On a positive-W convex hull every projected point lies in the projected hull.
    radius = max(math.hypot(2*u/view.extent-1, 1-2*v/view.extent)/FOCAL for u, v, _, _ in corners)
    return view.extent/2*FOCAL*max(view.scale)*model_bound*math.hypot(1, radius)/w_min


def projected_deviation(shape, records, view):
    vertices = records['v']
    result = {}
    for p, _, _, bound, count in records['p']:
        end = (-1 if p == 1 else 1)*width(shape, p)
        z_min = height(shape, p, end, 0)[0]
        # The cylindrical Bernstein controls lie in this box. Enlarge it to
        # include binary32 conversion at the cooker and emitted vertices.
        slack = 1e-6
        hull = ((min(0, end)-slack, max(0, end)+slack), (-1-slack, 1+slack), (z_min-slack, slack))
        conservative = projected_bound(view, hull, bound+slack)
        require(conservative is not None, 'supported view crosses the near plane')
        if view.scale != (1, 1, 1) and p == 1:
            crossing = Projection((view.side, view.sine/view.cosine, .6, view.scale), view.extent)
            require(projected_bound(crossing, hull, bound) is None, 'near-plane control admitted')
        center_w = view.project((end/2, 0, height(shape, p, end/2, 0)[0]))[3]
        naive = FOCAL*view.extent/2*max(view.scale)*bound/center_w
        samples = [view.project(v[4:7]) for v in vertices if v[0] == p]
        span = math.hypot(*(max(q[k] for q in samples)-min(q[k] for q in samples) for k in (0, 1)))
        peak = 0.
        for face in records['f']:
            tri = [vertices[int(i)] for i in face]
            if tri[0][0] != p:
                continue
            for a, b in zip(tri, (*tri[1:], tri[0])):
                x, y, z = ((a[k]+b[k])/2 for k in (4, 5, 6))
                exact = view.project((x, y, height(shape, p, x, y)[0]))
                linear = view.project((x, y, z))
                peak = max(peak, math.dist(exact[:2], linear[:2]))
        require(peak <= conservative, 'projected deviation exceeds conservative hull bound')
        result[p] = (span, peak, naive, conservative, int(count))
    return result
