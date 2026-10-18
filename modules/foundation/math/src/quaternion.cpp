// Quaternion robust algorithms (design §6). Precise FP flags; <cmath>/<limits>
// permitted (ADR 0003/0010).

#include <ludus/foundation/math/quaternion.hpp>

#include <cmath>

namespace ludus::foundation::math
{
namespace
{
struct Quat4d
{
    float64 x, y, z, w;
};

[[nodiscard]] float64 Len4d(const Quat4d& q) noexcept
{
    const float64 s = std::fmax(std::fmax(std::fabs(q.x), std::fabs(q.y)), std::fmax(std::fabs(q.z), std::fabs(q.w)));
    if (s == 0.0)
    {
        return 0.0;
    }
    const float64 ux = q.x / s, uy = q.y / s, uz = q.z / s, uw = q.w / s;
    return s * std::sqrt(ux * ux + uy * uy + uz * uz + uw * uw);
}

// Normalise a double quaternion into a float32 Quaternion with a finite check.
[[nodiscard]] MathStatus NarrowUnit(const Quat4d& q, Quaternion& out) noexcept
{
    const float64 len = Len4d(q);
    if (len == 0.0 || std::isfinite(len) == false)
    {
        return MathStatus::Degenerate;
    }
    const float32 x = static_cast<float32>(q.x / len);
    const float32 y = static_cast<float32>(q.y / len);
    const float32 z = static_cast<float32>(q.z / len);
    const float32 w = static_cast<float32>(q.w / len);
    if (IsFinite(x) == false || IsFinite(y) == false || IsFinite(z) == false || IsFinite(w) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Quaternion{x, y, z, w};
    return MathStatus::Success;
}

// Robustly normalise a float32 direction in double; returns false for zero /
// non-finite.
[[nodiscard]] bool NormalizeDir(Vector3 v, float64& ox, float64& oy, float64& oz) noexcept
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
    const float64 len = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (len == 0.0 || std::isfinite(len) == false)
    {
        return false;
    }
    ox = ux / len;
    oy = uy / len;
    oz = uz / len;
    return true;
}
} // namespace

MathStatus TryNormalize(Quaternion q, Quaternion& out) noexcept
{
    if (IsFinite(q) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    return NarrowUnit(Quat4d{static_cast<float64>(q.X),
                             static_cast<float64>(q.Y),
                             static_cast<float64>(q.Z),
                             static_cast<float64>(q.W)},
                      out);
}

MathStatus TryFromAxisAngle(Vector3 axis, float32 angleRadians, Quaternion& out) noexcept
{
    if (IsFinite(axis) == false || std::isfinite(angleRadians) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    float64 ax, ay, az;
    if (NormalizeDir(axis, ax, ay, az) == false)
    {
        return MathStatus::Degenerate;
    }
    const float64 half = static_cast<float64>(angleRadians) * 0.5;
    const float64 s = std::sin(half);
    const float64 c = std::cos(half);
    return NarrowUnit(Quat4d{ax * s, ay * s, az * s, c}, out);
}

Vector3 Rotate(Quaternion q, Vector3 v) noexcept
{
    LUDUS_ASSERT(IsFinite(q) && IsFinite(v));
    // t = 2 * cross(q.xyz, v); result = v + q.w * t + cross(q.xyz, t). Standard
    // expansion; matches ToMatrix3(q) * v. Done in float64 then narrowed.
    const float64 qx = static_cast<float64>(q.X), qy = static_cast<float64>(q.Y), qz = static_cast<float64>(q.Z),
                  qw = static_cast<float64>(q.W);
    const float64 vx = static_cast<float64>(v.X), vy = static_cast<float64>(v.Y), vz = static_cast<float64>(v.Z);
    const float64 tx = 2.0 * (qy * vz - qz * vy);
    const float64 ty = 2.0 * (qz * vx - qx * vz);
    const float64 tz = 2.0 * (qx * vy - qy * vx);
    const float64 rx = vx + qw * tx + (qy * tz - qz * ty);
    const float64 ry = vy + qw * ty + (qz * tx - qx * tz);
    const float64 rz = vz + qw * tz + (qx * ty - qy * tx);
    return Vector3{static_cast<float32>(rx), static_cast<float32>(ry), static_cast<float32>(rz)};
}

// Thanks to Stan Melax, "The Shortest Arc Quaternion", Game Programming Gems 1
// (2000), section 2.10, pp. 214-217, for the dot/cross half-angle construction.
// Ludus adds checked double intermediates, a stable near-opposite branch and a
// deterministic exact-opposite axis; the chapter's sample omits opposite inputs.
// See docs/architecture/math-research.md for the reviewed source and adaptations.
MathStatus TryRotationBetween(Vector3 from, Vector3 to, Quaternion& out) noexcept
{
    if (IsFinite(from) == false || IsFinite(to) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    float64 fx, fy, fz, tx, ty, tz;
    if (NormalizeDir(from, fx, fy, fz) == false || NormalizeDir(to, tx, ty, tz) == false)
    {
        return MathStatus::Degenerate;
    }
    float64 d = fx * tx + fy * ty + fz * tz;
    d = d < -1.0 ? -1.0 : (d > 1.0 ? 1.0 : d);
    // cross(from,to)
    const float64 cx = fy * tz - fz * ty;
    const float64 cy = fz * tx - fx * tz;
    const float64 cz = fx * ty - fy * tx;
    const float64 s = std::sqrt(cx * cx + cy * cy + cz * cz);

    if (d >= 0.0)
    {
        // Regular case. w = sqrt((1+d)/2); vector = cross / sqrt(2(1+d)).
        const float64 denom = std::sqrt(2.0 * (1.0 + d));
        if (denom == 0.0)
        {
            out = QuaternionIdentity();
            return MathStatus::Success;
        }
        return NarrowUnit(Quat4d{cx / denom, cy / denom, cz / denom, std::sqrt((1.0 + d) / 2.0)}, out);
    }

    // d < 0.
    if (s > 0.0)
    {
        // Use the unit cross axis; avoid cancellation in (1+d) near opposite.
        const float64 vmag = std::sqrt((1.0 - d) / 2.0);
        const float64 w = s / std::sqrt(2.0 * (1.0 - d));
        return NarrowUnit(Quat4d{(cx / s) * vmag, (cy / s) * vmag, (cz / s) * vmag, w}, out);
    }

    // s == 0 and d < 0 -> exact opposite. Pick the coordinate axis least aligned
    // with `from` (ties X then Y then Z), rotate 180 degrees about cross(from,axis).
    const float64 ax = std::fabs(fx), ay = std::fabs(fy), az = std::fabs(fz);
    float64 axisx = 0.0, axisy = 0.0, axisz = 0.0;
    if (ax <= ay && ax <= az)
    {
        axisx = 1.0;
    }
    else if (ay <= az)
    {
        axisy = 1.0;
    }
    else
    {
        axisz = 1.0;
    }
    // perpendicular = normalize(cross(from, axis))
    const float64 px = fy * axisz - fz * axisy;
    const float64 py = fz * axisx - fx * axisz;
    const float64 pz = fx * axisy - fy * axisx;
    const float64 plen = std::sqrt(px * px + py * py + pz * pz);
    if (plen == 0.0)
    {
        return MathStatus::Degenerate;
    }
    return NarrowUnit(Quat4d{px / plen, py / plen, pz / plen, 0.0}, out);
}

namespace
{
[[nodiscard]] Quaternion NegateIfNeeded(Quaternion a, Quaternion b) noexcept
{
    const float32 d = Dot(a, b);
    if (d < 0.0f)
    {
        return Quaternion{-b.X, -b.Y, -b.Z, -b.W};
    }
    return b;
}
} // namespace

// Thanks to Jason Shankel, "Interpolating Quaternions", Game Programming Gems 1
// (2000), section 2.9, pp. 205-213, for hemisphere correction, spherical
// interpolation and the normalized close-angle fallback. Ludus exposes separate
// nlerp/slerp operations and uses double intermediates; squad/spin curves are
// outside this implementation. Source scope: docs/architecture/math-research.md.
Quaternion NlerpShortest(Quaternion a, Quaternion b, float32 t) noexcept
{
    LUDUS_ASSERT(std::isfinite(t));
    const Quaternion bc = NegateIfNeeded(a, b);
    const float64 ta = 1.0 - static_cast<float64>(t);
    const float64 tb = static_cast<float64>(t);
    Quat4d blend{ta * static_cast<float64>(a.X) + tb * static_cast<float64>(bc.X),
                 ta * static_cast<float64>(a.Y) + tb * static_cast<float64>(bc.Y),
                 ta * static_cast<float64>(a.Z) + tb * static_cast<float64>(bc.Z),
                 ta * static_cast<float64>(a.W) + tb * static_cast<float64>(bc.W)};
    const float64 len = Len4d(blend);
    if (len == 0.0)
    {
        return a; // degenerate antipodal pair after correction; return a safely
    }
    return Quaternion{static_cast<float32>(blend.x / len),
                      static_cast<float32>(blend.y / len),
                      static_cast<float32>(blend.z / len),
                      static_cast<float32>(blend.w / len)};
}

Quaternion SlerpShortest(Quaternion a, Quaternion b, float32 t) noexcept
{
    LUDUS_ASSERT(std::isfinite(t));
    const Quaternion bc = NegateIfNeeded(a, b);
    float64 d = static_cast<float64>(Dot(a, bc));
    d = d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
    // Close-angle: normalised linear blend (nlerp) avoids a tiny sine denom.
    if (d > 0.9995)
    {
        return NlerpShortest(a, bc, t);
    }
    const float64 theta = std::acos(d);
    const float64 sinTheta = std::sin(theta);
    const float64 wa = std::sin((1.0 - static_cast<float64>(t)) * theta) / sinTheta;
    const float64 wb = std::sin(static_cast<float64>(t) * theta) / sinTheta;
    Quat4d blend{wa * static_cast<float64>(a.X) + wb * static_cast<float64>(bc.X),
                 wa * static_cast<float64>(a.Y) + wb * static_cast<float64>(bc.Y),
                 wa * static_cast<float64>(a.Z) + wb * static_cast<float64>(bc.Z),
                 wa * static_cast<float64>(a.W) + wb * static_cast<float64>(bc.W)};
    const float64 len = Len4d(blend);
    if (len == 0.0)
    {
        return a;
    }
    return Quaternion{static_cast<float32>(blend.x / len),
                      static_cast<float32>(blend.y / len),
                      static_cast<float32>(blend.z / len),
                      static_cast<float32>(blend.w / len)};
}

bool SameRotation(Quaternion a, Quaternion b, float32 toleranceRadians) noexcept
{
    if (IsFinite(a) == false || IsFinite(b) == false || std::isfinite(toleranceRadians) == false)
    {
        return false;
    }
    if (toleranceRadians < 0.0f || toleranceRadians > kPiF)
    {
        return false;
    }
    // Validate unit inputs within a small rounding slack.
    const float64 la = Len4d(Quat4d{static_cast<float64>(a.X),
                                    static_cast<float64>(a.Y),
                                    static_cast<float64>(a.Z),
                                    static_cast<float64>(a.W)});
    const float64 lb = Len4d(Quat4d{static_cast<float64>(b.X),
                                    static_cast<float64>(b.Y),
                                    static_cast<float64>(b.Z),
                                    static_cast<float64>(b.W)});
    if (std::fabs(la - 1.0) > 1e-4 || std::fabs(lb - 1.0) > 1e-4)
    {
        return false;
    }
    float64 ad = std::fabs(static_cast<float64>(Dot(a, b)));
    ad = ad > 1.0 ? 1.0 : ad;
    const float64 threshold = std::cos(static_cast<float64>(toleranceRadians) / 2.0);
    // Allow a small unit-input rounding slack below the threshold.
    return ad >= threshold - 1e-6;
}
} // namespace ludus::foundation::math
