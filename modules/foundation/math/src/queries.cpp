// Checked geometric queries (design §9). Precise FP flags; <cmath>/<limits>
// permitted (ADR 0003/0010). Everything computes in float64; no allocation.

#include <ludus/foundation/math/queries.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
namespace
{
constexpr float64 kInf = std::numeric_limits<float64>::infinity();
/// Largest normal length of an infinite far plane candidate still treated as zero.
constexpr float64 INFINITE_FAR_PLANE_NORMAL_TOLERANCE = 1e-5;

// Robustly normalize a float32 vector in double; false for zero / non-finite.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (ox, oy, oz) are the x/y/z out components.
[[nodiscard]] bool NormalizeD3(Vector3 v, float64& ox, float64& oy, float64& oz, float64& outLen) noexcept
{
    if (IsFinite(v) == false)
    {
        return false;
    }
    const float64 s = std::fmax(std::fmax(std::fabs((float64)v.X), std::fabs((float64)v.Y)), std::fabs((float64)v.Z));
    if (s == 0.0)
    {
        return false;
    }
    const float64 ux = (float64)v.X / s, uy = (float64)v.Y / s, uz = (float64)v.Z / s;
    const float64 len = s * std::sqrt(ux * ux + uy * uy + uz * uz);
    if (len == 0.0 || std::isfinite(len) == false)
    {
        return false;
    }
    ox = (float64)v.X / len;
    oy = (float64)v.Y / len;
    oz = (float64)v.Z / len;
    outLen = len;
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (nx, ny, nz, d) are the plane's normalized coefficient outs.
[[nodiscard]] bool PlaneUnitFinite(const Plane& p, float64& nx, float64& ny, float64& nz, float64& d) noexcept
{
    if (IsFinite(p.Normal) == false || IsFinite(p.D) == false)
    {
        return false;
    }
    const float64 len2 = (float64)p.Normal.X * static_cast<float64>(p.Normal.X) +
                         (float64)p.Normal.Y * static_cast<float64>(p.Normal.Y) +
                         (float64)p.Normal.Z * static_cast<float64>(p.Normal.Z);
    const float64 len = std::sqrt(len2);
    if (std::fabs(len - 1.0) > 1e-4)
    {
        return false;
    }
    nx = static_cast<float64>(p.Normal.X);
    ny = static_cast<float64>(p.Normal.Y);
    nz = static_cast<float64>(p.Normal.Z);
    d = static_cast<float64>(p.D);
    return true;
}
} // namespace

// ===========================================================================
// Plane
// ===========================================================================
// Thanks to John Olsen, "Vector and Plane Tricks", Game Programming Gems 2
// (2001), section 2.2, pp. 182-190, for signed distance, projection and reflection
// using a unit normal. Ludus derives signs from dot(n,p)+d=0, checks inputs and
// leaves collision response to callers. See docs/architecture/math-research.md.
MathStatus TryMakePlane(Vector3 normal, Vector3 point, Plane& out) noexcept
{
    if (IsFinite(normal) == false || IsFinite(point) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    float64 nx, ny, nz, len;
    if (NormalizeD3(normal, nx, ny, nz, len) == false)
    {
        return MathStatus::Degenerate;
    }
    const float64 d =
        -(nx * static_cast<float64>(point.X) + ny * static_cast<float64>(point.Y) + nz * static_cast<float64>(point.Z));
    const float32 fx = (float32)nx, fy = (float32)ny, fz = (float32)nz, fd = (float32)d;
    if (IsFinite(fx) == false || IsFinite(fy) == false || IsFinite(fz) == false || IsFinite(fd) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Plane{Vector3{fx, fy, fz}, fd};
    return MathStatus::Success;
}

MathStatus TryPlaneSignedDistance(const Plane& plane, Vector3 p, float32& out) noexcept
{
    if (IsFinite(p) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    float64 nx, ny, nz, d;
    if (PlaneUnitFinite(plane, nx, ny, nz, d) == false)
    {
        return MathStatus::Degenerate;
    }
    const float64 dist =
        nx * static_cast<float64>(p.X) + ny * static_cast<float64>(p.Y) + nz * static_cast<float64>(p.Z) + d;
    const float32 f = (float32)dist;
    if (IsFinite(f) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = f;
    return MathStatus::Success;
}

MathStatus TryProjectOntoPlane(const Plane& plane, Vector3 p, Vector3& out) noexcept
{
    float32 dist = 0.0f;
    const MathStatus status = TryPlaneSignedDistance(plane, p, dist);
    if (status != MathStatus::Success)
    {
        return status;
    }
    const Vector3 result = p - plane.Normal * dist;
    if (IsFinite(result) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}

MathStatus TryReflectVector(const Plane& plane, Vector3 v, Vector3& out) noexcept
{
    if (IsFinite(v) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    float64 nx, ny, nz, d;
    if (PlaneUnitFinite(plane, nx, ny, nz, d) == false)
    {
        return MathStatus::Degenerate;
    }
    const float64 dot =
        nx * static_cast<float64>(v.X) + ny * static_cast<float64>(v.Y) + nz * static_cast<float64>(v.Z);
    const Vector3 result{(float32)(static_cast<float64>(v.X) - 2.0 * dot * nx),
                         (float32)(static_cast<float64>(v.Y) - 2.0 * dot * ny),
                         (float32)(static_cast<float64>(v.Z) - 2.0 * dot * nz)};
    if (IsFinite(result) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}

// ===========================================================================
// AABB
// ===========================================================================
Aabb3 Merge(const Aabb3& a, const Aabb3& b) noexcept
{
    if (IsEmpty(a))
    {
        return b;
    }
    if (IsEmpty(b))
    {
        return a;
    }
    return Aabb3{Vector3{Min(a.Min.X, b.Min.X), Min(a.Min.Y, b.Min.Y), Min(a.Min.Z, b.Min.Z)},
                 Vector3{Max(a.Max.X, b.Max.X), Max(a.Max.Y, b.Max.Y), Max(a.Max.Z, b.Max.Z)}};
}
Aabb2 Merge(const Aabb2& a, const Aabb2& b) noexcept
{
    if (IsEmpty(a))
    {
        return b;
    }
    if (IsEmpty(b))
    {
        return a;
    }
    return Aabb2{Vector2{Min(a.Min.X, b.Min.X), Min(a.Min.Y, b.Min.Y)},
                 Vector2{Max(a.Max.X, b.Max.X), Max(a.Max.Y, b.Max.Y)}};
}
bool Contains(const Aabb3& box, Vector3 point) noexcept
{
    if (IsEmpty(box) || IsFinite(box) == false || IsFinite(point) == false)
    {
        return false;
    }
    return point.X >= box.Min.X && point.X <= box.Max.X && point.Y >= box.Min.Y && point.Y <= box.Max.Y &&
           point.Z >= box.Min.Z && point.Z <= box.Max.Z;
}
bool Contains(const Aabb2& box, Vector2 point) noexcept
{
    if (IsEmpty(box) || IsFinite(box) == false || IsFinite(point) == false)
    {
        return false;
    }
    return point.X >= box.Min.X && point.X <= box.Max.X && point.Y >= box.Min.Y && point.Y <= box.Max.Y;
}

MathStatus TryTransformAabb(const Affine3& a, const Aabb3& box, Aabb3& out) noexcept
{
    if (IsFinite(a) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsEmpty(box))
    {
        out = EmptyAabb3();
        return MathStatus::Success;
    }
    if (IsFinite(box) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    // Center/extents in double; transformed center = L*c + t, extents = |L|*e.
    const float64 cx = ((float64)box.Min.X + static_cast<float64>(box.Max.X)) * 0.5;
    const float64 cy = ((float64)box.Min.Y + static_cast<float64>(box.Max.Y)) * 0.5;
    const float64 cz = ((float64)box.Min.Z + static_cast<float64>(box.Max.Z)) * 0.5;
    const float64 ex = ((float64)box.Max.X - static_cast<float64>(box.Min.X)) * 0.5;
    const float64 ey = ((float64)box.Max.Y - static_cast<float64>(box.Min.Y)) * 0.5;
    const float64 ez = ((float64)box.Max.Z - static_cast<float64>(box.Min.Z)) * 0.5;
    const float64 L[3][3] = {
        {static_cast<float64>(a.Columns[0].X),
         static_cast<float64>(a.Columns[1].X),
         static_cast<float64>(a.Columns[2].X)},
        {static_cast<float64>(a.Columns[0].Y),
         static_cast<float64>(a.Columns[1].Y),
         static_cast<float64>(a.Columns[2].Y)},
        {static_cast<float64>(a.Columns[0].Z),
         static_cast<float64>(a.Columns[1].Z),
         static_cast<float64>(a.Columns[2].Z)},
    };
    const float64 t[3] = {static_cast<float64>(a.Translation.X),
                          static_cast<float64>(a.Translation.Y),
                          static_cast<float64>(a.Translation.Z)};
    const float64 c[3] = {cx, cy, cz};
    const float64 e[3] = {ex, ey, ez};
    float64 lo[3];
    float64 hi[3];
    for (int r = 0; r < 3; ++r)
    {
        const float64 tc = L[r][0] * c[0] + L[r][1] * c[1] + L[r][2] * c[2] + t[r];
        const float64 te = std::fabs(L[r][0]) * e[0] + std::fabs(L[r][1]) * e[1] + std::fabs(L[r][2]) * e[2];
        // Outward rounding: nudge lower down and upper up to stay conservative.
        lo[r] = static_cast<float64>(
            std::nextafter(static_cast<float32>(tc - te), -std::numeric_limits<float32>::infinity()));
        hi[r] = static_cast<float64>(
            std::nextafter(static_cast<float32>(tc + te), std::numeric_limits<float32>::infinity()));
    }
    const Vector3 mn{(float32)lo[0], (float32)lo[1], (float32)lo[2]};
    const Vector3 mx{(float32)hi[0], (float32)hi[1], (float32)hi[2]};
    if (IsFinite(mn) == false || IsFinite(mx) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Aabb3{mn, mx};
    return MathStatus::Success;
}

MathStatus TryTransformAabb(const Affine2& a, const Aabb2& box, Aabb2& out) noexcept
{
    if (IsFinite(a) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsEmpty(box))
    {
        out = EmptyAabb2();
        return MathStatus::Success;
    }
    if (IsFinite(box) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const float64 cx = ((float64)box.Min.X + static_cast<float64>(box.Max.X)) * 0.5;
    const float64 cy = ((float64)box.Min.Y + static_cast<float64>(box.Max.Y)) * 0.5;
    const float64 ex = ((float64)box.Max.X - static_cast<float64>(box.Min.X)) * 0.5;
    const float64 ey = ((float64)box.Max.Y - static_cast<float64>(box.Min.Y)) * 0.5;
    const float64 L[2][2] = {{static_cast<float64>(a.Columns[0].X), static_cast<float64>(a.Columns[1].X)},
                             {static_cast<float64>(a.Columns[0].Y), static_cast<float64>(a.Columns[1].Y)}};
    const float64 t[2] = {static_cast<float64>(a.Translation.X), static_cast<float64>(a.Translation.Y)};
    const float64 c[2] = {cx, cy};
    const float64 e[2] = {ex, ey};
    float64 lo[2];
    float64 hi[2];
    for (int r = 0; r < 2; ++r)
    {
        const float64 tc = L[r][0] * c[0] + L[r][1] * c[1] + t[r];
        const float64 te = std::fabs(L[r][0]) * e[0] + std::fabs(L[r][1]) * e[1];
        lo[r] = static_cast<float64>(
            std::nextafter(static_cast<float32>(tc - te), -std::numeric_limits<float32>::infinity()));
        hi[r] = static_cast<float64>(
            std::nextafter(static_cast<float32>(tc + te), std::numeric_limits<float32>::infinity()));
    }
    const Vector2 mn{(float32)lo[0], (float32)lo[1]};
    const Vector2 mx{(float32)hi[0], (float32)hi[1]};
    if (IsFinite(mn) == false || IsFinite(mx) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Aabb2{mn, mx};
    return MathStatus::Success;
}

// ===========================================================================
// Ray queries
// ===========================================================================
// Thanks to Jacco Bikker, "Fast Generic Ray Queries for Games", Game Programming
// Gems 7 (2008), section 2.2, pp. 127-141, for bounded parametric ray intervals.
// Ludus keeps primitive tests separate from traversal and checks zero direction
// components instead of using unchecked reciprocals or compact pointer tricks.
// Source scope: docs/architecture/math-research.md.
bool IsValidInterval(const RayInterval& interval) noexcept
{
    if (std::isnan(interval.MinT) || std::isnan(interval.MaxT))
    {
        return false;
    }
    if (std::isfinite(interval.MinT) == false || interval.MinT < 0.0)
    {
        return false;
    }
    // MaxT may be +inf, but not -inf, and must be >= MinT.
    if (interval.MaxT == -kInf)
    {
        return false;
    }
    return interval.MaxT >= interval.MinT;
}

namespace
{
[[nodiscard]] bool RayDirectionValid(const Ray3& ray) noexcept
{
    if (IsFinite(ray.Origin) == false || IsFinite(ray.Direction) == false)
    {
        return false;
    }
    // Valid when at least one component is non-zero (a wholly zero direction is
    // rejected; individual zero components are fine).
    return ray.Direction.X != 0.0f || ray.Direction.Y != 0.0f || ray.Direction.Z != 0.0f;
}
} // namespace

MathStatus TryRayAabb(const Ray3& ray, const Aabb3& box, const RayInterval& interval, RayHit& out) noexcept
{
    if (RayDirectionValid(ray) == false)
    {
        return (IsFinite(ray.Origin) == false || IsFinite(ray.Direction) == false) ? MathStatus::NonFiniteInput
                                                                                   : MathStatus::InvalidArgument;
    }
    if (IsValidInterval(interval) == false)
    {
        return MathStatus::InvalidArgument;
    }
    if (IsFinite(box) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsEmpty(box))
    {
        out = RayHit{HitResult::Miss, 0.0, 0.0};
        return MathStatus::Success;
    }

    float64 entry = interval.MinT;
    float64 exit = interval.MaxT;
    const float64 o[3] = {static_cast<float64>(ray.Origin.X),
                          static_cast<float64>(ray.Origin.Y),
                          static_cast<float64>(ray.Origin.Z)};
    const float64 dcomp[3] = {static_cast<float64>(ray.Direction.X),
                              static_cast<float64>(ray.Direction.Y),
                              static_cast<float64>(ray.Direction.Z)};
    const float64 lo[3] = {static_cast<float64>(box.Min.X),
                           static_cast<float64>(box.Min.Y),
                           static_cast<float64>(box.Min.Z)};
    const float64 hi[3] = {static_cast<float64>(box.Max.X),
                           static_cast<float64>(box.Max.Y),
                           static_cast<float64>(box.Max.Z)};

    for (int i = 0; i < 3; ++i)
    {
        if (dcomp[i] == 0.0) // +0 or -0: no bound; must be inside the slab.
        {
            if (o[i] < lo[i] || o[i] > hi[i])
            {
                out = RayHit{HitResult::Miss, 0.0, 0.0};
                return MathStatus::Success;
            }
            continue;
        }
        float64 t0 = (lo[i] - o[i]) / dcomp[i];
        float64 t1 = (hi[i] - o[i]) / dcomp[i];
        if (t0 > t1)
        {
            const float64 tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        if (t0 > entry)
        {
            entry = t0;
        }
        if (t1 < exit)
        {
            exit = t1;
        }
        if (entry > exit)
        {
            out = RayHit{HitResult::Miss, 0.0, 0.0};
            return MathStatus::Success;
        }
    }
    out = RayHit{HitResult::Hit, entry, exit};
    return MathStatus::Success;
}

MathStatus TryRaySphere(const Ray3& ray, const Sphere& sphere, const RayInterval& interval, RayHit& out) noexcept
{
    if (RayDirectionValid(ray) == false)
    {
        return (IsFinite(ray.Origin) == false || IsFinite(ray.Direction) == false) ? MathStatus::NonFiniteInput
                                                                                   : MathStatus::InvalidArgument;
    }
    if (IsValidInterval(interval) == false)
    {
        return MathStatus::InvalidArgument;
    }
    if (IsFinite(sphere.Center) == false || std::isfinite(sphere.Radius) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (sphere.Radius < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }

    const float64 ox = (float64)ray.Origin.X - static_cast<float64>(sphere.Center.X);
    const float64 oy = (float64)ray.Origin.Y - static_cast<float64>(sphere.Center.Y);
    const float64 oz = (float64)ray.Origin.Z - static_cast<float64>(sphere.Center.Z);
    const float64 dx = static_cast<float64>(ray.Direction.X), dy = static_cast<float64>(ray.Direction.Y),
                  dz = static_cast<float64>(ray.Direction.Z);
    const float64 a = dx * dx + dy * dy + dz * dz;
    const float64 b = ox * dx + oy * dy + oz * dz; // half-b
    const float64 c = ox * ox + oy * oy + oz * oz - (float64)sphere.Radius * static_cast<float64>(sphere.Radius);
    const float64 discriminant = b * b - a * c;
    if (discriminant < 0.0)
    {
        out = RayHit{HitResult::Miss, 0.0, 0.0};
        return MathStatus::Success;
    }
    const float64 sqrtDisc = std::sqrt(discriminant);
    // Cancellation-resistant roots: q = -b - copysign(sqrtDisc, b).
    const float64 q = -b - std::copysign(sqrtDisc, b == 0.0 ? 1.0 : b);
    float64 r0;
    float64 r1;
    if (q != 0.0)
    {
        r0 = q / a;
        r1 = c / q;
    }
    else
    {
        // Degenerate: both roots coincide (tangent through origin direction) or
        // a*c == 0. Fall back to the stable symmetric form.
        r0 = (-b) / a;
        r1 = r0;
    }
    float64 tmin = std::fmin(r0, r1);
    float64 tmax = std::fmax(r0, r1);

    // Intersect the sphere's closed inside interval [tmin, tmax] with the query.
    float64 entry = std::fmax(tmin, interval.MinT);
    float64 exit = std::fmin(tmax, interval.MaxT);
    if (entry > exit)
    {
        out = RayHit{HitResult::Miss, 0.0, 0.0};
        return MathStatus::Success;
    }
    out = RayHit{HitResult::Hit, entry, exit};
    return MathStatus::Success;
}

// ===========================================================================
// Closest points between segments
// ===========================================================================
namespace
{
struct D3
{
    float64 x, y, z;
};
[[nodiscard]] D3 Sub(Vector3 a, Vector3 b) noexcept
{
    return D3{(float64)a.X - static_cast<float64>(b.X),
              (float64)a.Y - static_cast<float64>(b.Y),
              (float64)a.Z - static_cast<float64>(b.Z)};
}
[[nodiscard]] float64 DotD(const D3& a, const D3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] D3 CrossD(const D3& a, const D3& b) noexcept
{
    return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] float64 ClampUnit(float64 v) noexcept
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}
// Point P projected onto segment [A, A+dir], parameter clamped to [0,1].
[[nodiscard]] float64 ProjectParam(const D3& ap, const D3& dir, float64 dirLen2) noexcept
{
    if (dirLen2 == 0.0)
    {
        return 0.0;
    }
    return ClampUnit(DotD(ap, dir) / dirLen2);
}
} // namespace

// Thanks to Graham Rhodes, "Fast, Robust Intersection of 3D Line Segments",
// Game Programming Gems 2 (2001), section 2.3, pp. 191-204, for constrained
// closest-point queries and explicit parallel/degenerate cases. Ludus instead
// enumerates an interior stationary candidate and four endpoint projections in
// double precision, with a fixed tie rule; this is not an exact predicate.
// Source scope and adaptations: docs/architecture/math-research.md.
MathStatus TryClosestPointsSegments(Vector3 p0, Vector3 p1, Vector3 q0, Vector3 q1, SegmentClosest& out) noexcept
{
    if (IsFinite(p0) == false || IsFinite(p1) == false || IsFinite(q0) == false || IsFinite(q1) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const D3 u = Sub(p1, p0); // segment 1 direction
    const D3 v = Sub(q1, q0); // segment 2 direction
    const D3 w0 = Sub(p0, q0);
    const float64 a = DotD(u, u);
    const float64 b = DotD(u, v);
    const float64 c = DotD(v, v);
    const float64 d = DotD(u, w0);
    const float64 e = DotD(v, w0);

    // Candidate accumulator: track best (sqDist, s, t) with tie rule lower s then t.
    float64 bestSq = kInf;
    float64 bestS = 0.0;
    float64 bestT = 0.0;
    auto consider = [&](float64 s, float64 t) {
        // Closest points for parameters s,t.
        const float64 cp1x = static_cast<float64>(p0.X) + s * u.x;
        const float64 cp1y = static_cast<float64>(p0.Y) + s * u.y;
        const float64 cp1z = static_cast<float64>(p0.Z) + s * u.z;
        const float64 cp2x = static_cast<float64>(q0.X) + t * v.x;
        const float64 cp2y = static_cast<float64>(q0.Y) + t * v.y;
        const float64 cp2z = static_cast<float64>(q0.Z) + t * v.z;
        const float64 dxp = cp1x - cp2x, dyp = cp1y - cp2y, dzp = cp1z - cp2z;
        const float64 sq = dxp * dxp + dyp * dyp + dzp * dzp;
        if (sq < bestSq || (sq == bestSq && (s < bestS || (s == bestS && t < bestT))))
        {
            bestSq = sq;
            bestS = s;
            bestT = t;
        }
    };

    const D3 w = w0; // p0 - q0

    // Interior stationary candidate (only when neither segment is a point).
    const D3 cr = CrossD(u, v);
    const float64 det = DotD(cr, cr); // = a*c - b*b
    if (a > 0.0 && c > 0.0 && det > 0.0)
    {
        const float64 s = (b * e - c * d) / det;
        const float64 t = (a * e - b * d) / det;
        if (s >= 0.0 && s <= 1.0 && t >= 0.0 && t <= 1.0)
        {
            consider(s, t);
        }
    }

    // Endpoint-to-segment candidates (cover the constraint boundary and the
    // parallel/degenerate cases). For each of the four endpoints, project onto
    // the opposite segment. The point on segment 2 closest to a point P (base
    // q0, direction v) minimises |P-(q0+t*v)|^2 at t=(P-q0).v / (v.v), clamped.
    // s=0: P = p0, and (p0-q0) = w, so project w (NOT -w) onto v.
    consider(0.0, ProjectParam(w, v, c));
    // s=1: project (p1 - q0) onto v.
    {
        const D3 p1q0 = Sub(p1, q0);
        consider(1.0, ProjectParam(p1q0, v, c));
    }
    // t=0: project (q0 - p0) onto u.
    {
        const D3 q0p0 = Sub(q0, p0);
        consider(ProjectParam(q0p0, u, a), 0.0);
    }
    // t=1: project (q1 - p0) onto u.
    {
        const D3 q1p0 = Sub(q1, p0);
        consider(ProjectParam(q1p0, u, a), 1.0);
    }

    if (std::isfinite(bestSq) == false)
    {
        return MathStatus::Degenerate; // should not happen for finite input
    }

    const float32 cp1x = (float32)(static_cast<float64>(p0.X) + bestS * u.x);
    const float32 cp1y = (float32)(static_cast<float64>(p0.Y) + bestS * u.y);
    const float32 cp1z = (float32)(static_cast<float64>(p0.Z) + bestS * u.z);
    const float32 cp2x = (float32)(static_cast<float64>(q0.X) + bestT * v.x);
    const float32 cp2y = (float32)(static_cast<float64>(q0.Y) + bestT * v.y);
    const float32 cp2z = (float32)(static_cast<float64>(q0.Z) + bestT * v.z);
    const Vector3 c1{cp1x, cp1y, cp1z};
    const Vector3 c2{cp2x, cp2y, cp2z};
    if (IsFinite(c1) == false || IsFinite(c2) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = SegmentClosest{bestS, bestT, c1, c2, bestSq};
    return MathStatus::Success;
}

// ===========================================================================
// Frustum
// ===========================================================================
namespace
{
// Normalize a candidate plane (nx,ny,nz,d) by the xyz length; false if the xyz
// length is zero (a degenerate plane).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (nx, ny, nz, d) are the plane's raw coefficients.
[[nodiscard]] bool NormalizePlaneD(float64 nx, float64 ny, float64 nz, float64 d, Plane& out) noexcept
{
    const float64 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len == 0.0 || std::isfinite(len) == false)
    {
        return false;
    }
    const float32 fx = (float32)(nx / len), fy = (float32)(ny / len), fz = (float32)(nz / len), fd = (float32)(d / len);
    if (IsFinite(fx) == false || IsFinite(fy) == false || IsFinite(fz) == false || IsFinite(fd) == false)
    {
        return false;
    }
    out = Plane{Vector3{fx, fy, fz}, fd};
    return true;
}
} // namespace

// Thanks to Waldemar Celes, "Extracting Frustum and Camera Information", Game
// Programming Gems 4 (2004), section 2.2, pp. 147-156, for pulling clip planes
// back through the point transform. Ludus uses 0<=z<=w, reverse-Z near/far labels
// and an inactive far plane for infinite perspective, rather than OpenGL depth.
// See docs/architecture/math-research.md for source scope and the derivation.
MathStatus TryExtractFrustum(const Matrix4& worldToClip, FrustumMode mode, Frustum& out) noexcept
{
    if (IsFinite(worldToClip) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    // Rows of the clip matrix (column-major storage: row r = (c0[r],c1[r],c2[r],c3[r])).
    auto row = [&](usize r) {
        return Vector4{worldToClip.At(r, 0), worldToClip.At(r, 1), worldToClip.At(r, 2), worldToClip.At(r, 3)};
    };
    const Vector4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    // Inward coefficients (design §9): components as doubles.
    struct Cand
    {
        float64 nx, ny, nz, d;
        bool required;
    };
    Cand cands[6] = {
        {(float64)r3.X + static_cast<float64>(r0.X),
         (float64)r3.Y + static_cast<float64>(r0.Y),
         (float64)r3.Z + static_cast<float64>(r0.Z),
         (float64)r3.W + static_cast<float64>(r0.W),
         true}, // left
        {(float64)r3.X - static_cast<float64>(r0.X),
         (float64)r3.Y - static_cast<float64>(r0.Y),
         (float64)r3.Z - static_cast<float64>(r0.Z),
         (float64)r3.W - static_cast<float64>(r0.W),
         true}, // right
        {(float64)r3.X + static_cast<float64>(r1.X),
         (float64)r3.Y + static_cast<float64>(r1.Y),
         (float64)r3.Z + static_cast<float64>(r1.Z),
         (float64)r3.W + static_cast<float64>(r1.W),
         true}, // bottom
        {(float64)r3.X - static_cast<float64>(r1.X),
         (float64)r3.Y - static_cast<float64>(r1.Y),
         (float64)r3.Z - static_cast<float64>(r1.Z),
         (float64)r3.W - static_cast<float64>(r1.W),
         true}, // top
        {(float64)r3.X - static_cast<float64>(r2.X),
         (float64)r3.Y - static_cast<float64>(r2.Y),
         (float64)r3.Z - static_cast<float64>(r2.Z),
         (float64)r3.W - static_cast<float64>(r2.W),
         true},                                                             // near (depthUpper)
        {(float64)r2.X, (float64)r2.Y, (float64)r2.Z, (float64)r2.W, true}, // far (depthLower)
    };
    // Plane slot order: Left, Right, Bottom, Top, Near, Far.
    Frustum result{};
    result.Mode = mode;
    uint8 mask = 0;
    for (int i = 0; i < 6; ++i)
    {
        bool active = true;
        if (i == 5 && mode == FrustumMode::InfiniteReverseZPerspective)
        {
            // depthLower must have zero xyz and a positive constant; mark inactive.
            const float64 xyzLen =
                std::sqrt(cands[i].nx * cands[i].nx + cands[i].ny * cands[i].ny + cands[i].nz * cands[i].nz);
            if (xyzLen > INFINITE_FAR_PLANE_NORMAL_TOLERANCE || (cands[i].d > 0.0) == false)
            {
                return MathStatus::Degenerate;
            }
            active = false;
            result.Planes[i] = Plane{Vector3{0, 0, 0}, (float32)cands[i].d};
        }
        else
        {
            Plane p{};
            if (NormalizePlaneD(cands[i].nx, cands[i].ny, cands[i].nz, cands[i].d, p) == false)
            {
                return MathStatus::Degenerate;
            }
            result.Planes[i] = p;
            active = true;
        }
        if (active)
        {
            mask |= (uint8)(1u << i);
        }
    }
    result.ActiveMask = mask;
    out = result;
    return MathStatus::Success;
}

namespace
{
// Roundoff allowance for conservative classification: derived from the sum of
// absolute products that form the signed distance, scaled by a few ULPs, plus
// the coefficient-narrowing slack. This is added to the geometric margin so a
// retained object is never falsely culled.
[[nodiscard]] float64 DistanceAllowance(float64 absSum) noexcept
{
    // ~ a handful of float32 ULPs over the magnitude of the dot plus a small
    // absolute floor for the normalized coefficients and constant narrowing.
    constexpr float64 kRelative = 8.0 * 1.1920929e-07; // 8 * float32 epsilon
    constexpr float64 kFloor = 1e-5;
    return kRelative * absSum + kFloor;
}
} // namespace

MathStatus
TryClassifySphere(const Frustum& frustum, const Sphere& sphere, float32 margin, FrustumRelation& out) noexcept
{
    if (IsFinite(sphere.Center) == false || std::isfinite(sphere.Radius) == false || std::isfinite(margin) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (sphere.Radius < 0.0f || margin < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }
    bool anyIntersecting = false;
    for (int i = 0; i < 6; ++i)
    {
        if ((frustum.ActiveMask & (1u << i)) == 0)
        {
            continue;
        }
        const Plane& p = frustum.Planes[i];
        const float64 absSum = std::fabs((float64)p.Normal.X * static_cast<float64>(sphere.Center.X)) +
                               std::fabs((float64)p.Normal.Y * static_cast<float64>(sphere.Center.Y)) +
                               std::fabs((float64)p.Normal.Z * static_cast<float64>(sphere.Center.Z)) +
                               std::fabs((float64)p.D);
        const float64 dist = (float64)p.Normal.X * static_cast<float64>(sphere.Center.X) +
                             (float64)p.Normal.Y * static_cast<float64>(sphere.Center.Y) +
                             (float64)p.Normal.Z * static_cast<float64>(sphere.Center.Z) + static_cast<float64>(p.D);
        const float64 allowance = DistanceAllowance(absSum);
        const float64 r = (float64)sphere.Radius + static_cast<float64>(margin);
        if (dist < -r - allowance)
        {
            out = FrustumRelation::Outside;
            return MathStatus::Success;
        }
        if ((dist > r + allowance) == false)
        {
            anyIntersecting = true;
        }
    }
    out = anyIntersecting ? FrustumRelation::Intersecting : FrustumRelation::Inside;
    return MathStatus::Success;
}

MathStatus TryClassifyAabb(const Frustum& frustum, const Aabb3& box, float32 margin, FrustumRelation& out) noexcept
{
    if (std::isfinite(margin) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (margin < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }
    if (IsEmpty(box))
    {
        out = FrustumRelation::Outside;
        return MathStatus::Success;
    }
    if (IsFinite(box) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const float64 cx = ((float64)box.Min.X + static_cast<float64>(box.Max.X)) * 0.5;
    const float64 cy = ((float64)box.Min.Y + static_cast<float64>(box.Max.Y)) * 0.5;
    const float64 cz = ((float64)box.Min.Z + static_cast<float64>(box.Max.Z)) * 0.5;
    const float64 ex = ((float64)box.Max.X - static_cast<float64>(box.Min.X)) * 0.5;
    const float64 ey = ((float64)box.Max.Y - static_cast<float64>(box.Min.Y)) * 0.5;
    const float64 ez = ((float64)box.Max.Z - static_cast<float64>(box.Min.Z)) * 0.5;
    bool anyIntersecting = false;
    for (int i = 0; i < 6; ++i)
    {
        if ((frustum.ActiveMask & (1u << i)) == 0)
        {
            continue;
        }
        const Plane& p = frustum.Planes[i];
        const float64 nx = static_cast<float64>(p.Normal.X), ny = static_cast<float64>(p.Normal.Y),
                      nz = static_cast<float64>(p.Normal.Z), d = static_cast<float64>(p.D);
        const float64 s = nx * cx + ny * cy + nz * cz + d;                                  // center distance
        const float64 rproj = std::fabs(nx) * ex + std::fabs(ny) * ey + std::fabs(nz) * ez; // projected radius
        const float64 absSum = std::fabs(nx * cx) + std::fabs(ny * cy) + std::fabs(nz * cz) + std::fabs(d) +
                               std::fabs(nx) * ex + std::fabs(ny) * ey + std::fabs(nz) * ez;
        const float64 allowance = DistanceAllowance(absSum);
        if (s < -rproj - static_cast<float64>(margin) - allowance)
        {
            out = FrustumRelation::Outside;
            return MathStatus::Success;
        }
        if ((s > rproj + static_cast<float64>(margin) + allowance) == false)
        {
            anyIntersecting = true;
        }
    }
    out = anyIntersecting ? FrustumRelation::Intersecting : FrustumRelation::Inside;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
