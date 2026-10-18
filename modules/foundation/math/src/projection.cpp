// Camera / projection helpers (design §8). Precise FP flags; <cmath>/<limits>
// permitted (ADR 0003/0010). Coefficients computed in double then narrow-checked.

#include <ludus/foundation/math/projection.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
namespace
{
// Build a Matrix4 from rows given as double arrays; narrow-check each element.
[[nodiscard]] MathStatus FromRows(const float64 rows[4][4], Matrix4& out) noexcept
{
    Matrix4 m{};
    for (int c = 0; c < 4; ++c)
    {
        const float32 x = static_cast<float32>(rows[0][c]);
        const float32 y = static_cast<float32>(rows[1][c]);
        const float32 z = static_cast<float32>(rows[2][c]);
        const float32 w = static_cast<float32>(rows[3][c]);
        if (IsFinite(x) == false || IsFinite(y) == false || IsFinite(z) == false || IsFinite(w) == false)
        {
            return MathStatus::OutOfRange;
        }
        m.Columns[c] = Vector4{x, y, z, w};
    }
    out = m;
    return MathStatus::Success;
}

[[nodiscard]] bool NormalizeDouble3(float64 x, float64 y, float64 z, float64& ox, float64& oy, float64& oz) noexcept
{
    const float64 s = std::fmax(std::fmax(std::fabs(x), std::fabs(y)), std::fabs(z));
    if (s == 0.0 || std::isfinite(s) == false)
    {
        return false;
    }
    const float64 ux = x / s, uy = y / s, uz = z / s;
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

MathStatus TryLookAt(Vector3 eye, Vector3 target, Vector3 upHint, Matrix4& outView) noexcept
{
    if (IsFinite(eye) == false || IsFinite(target) == false || IsFinite(upHint) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    // z = normalize(eye - target) (points backward, toward the camera).
    float64 zx, zy, zz;
    if (NormalizeDouble3((float64)eye.X - static_cast<float64>(target.X),
                         (float64)eye.Y - static_cast<float64>(target.Y),
                         (float64)eye.Z - static_cast<float64>(target.Z),
                         zx,
                         zy,
                         zz) == false)
    {
        return MathStatus::Degenerate;
    }
    // x = normalize(cross(upHint, z)).
    const float64 cx = (float64)upHint.Y * zz - (float64)upHint.Z * zy;
    const float64 cy = (float64)upHint.Z * zx - (float64)upHint.X * zz;
    const float64 cz = (float64)upHint.X * zy - (float64)upHint.Y * zx;
    float64 xx, xy, xz;
    if (NormalizeDouble3(cx, cy, cz, xx, xy, xz) == false)
    {
        return MathStatus::Degenerate; // parallel/zero upHint
    }
    // y = cross(z, x) (already unit for orthonormal z,x).
    const float64 yx = zy * xz - zz * xy;
    const float64 yy = zz * xx - zx * xz;
    const float64 yz = zx * xy - zy * xx;
    // View rows are x/y/z with translation -(row . eye). Column-major storage.
    const float64 tx =
        -(xx * static_cast<float64>(eye.X) + xy * static_cast<float64>(eye.Y) + xz * static_cast<float64>(eye.Z));
    const float64 ty =
        -(yx * static_cast<float64>(eye.X) + yy * static_cast<float64>(eye.Y) + yz * static_cast<float64>(eye.Z));
    const float64 tz =
        -(zx * static_cast<float64>(eye.X) + zy * static_cast<float64>(eye.Y) + zz * static_cast<float64>(eye.Z));
    const float64 rows[4][4] = {
        {xx, xy, xz, tx},
        {yx, yy, yz, ty},
        {zx, zy, zz, tz},
        {0.0, 0.0, 0.0, 1.0},
    };
    return FromRows(rows, outView);
}

// Thanks to Nathan Reed, "Visualizing Depth Precision", NVIDIA Technical Blog
// (2015), https://developer.nvidia.com/blog/visualizing-depth-precision/, for the
// floating-depth reverse-Z design rationale. The perspective and orthographic
// coefficients below are derived for Ludus's RH, 0<=z<=w convention with checked
// double intermediates; this credits design inspiration, not copied formulas.
// Reviewed source and depth contract: docs/architecture/math-research.md.
MathStatus TryPerspectiveReverseZ(float32 verticalFovRadians,
                                  float32 aspect,
                                  float32 nearPlane,
                                  float32 farPlane,
                                  Matrix4& out) noexcept
{
    if (std::isfinite(verticalFovRadians) == false || std::isfinite(aspect) == false ||
        std::isfinite(nearPlane) == false || std::isfinite(farPlane) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((aspect > 0.0f) == false || (nearPlane > 0.0f) == false || (farPlane > nearPlane) == false ||
        (verticalFovRadians > 0.0f) == false || (verticalFovRadians < kPiF) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 k = 1.0 / std::tan(static_cast<float64>(verticalFovRadians) * 0.5);
    const float64 n = static_cast<float64>(nearPlane), f = static_cast<float64>(farPlane);
    const float64 rows[4][4] = {
        {k / static_cast<float64>(aspect), 0.0, 0.0, 0.0},
        {0.0, k, 0.0, 0.0},
        {0.0, 0.0, n / (f - n), n * f / (f - n)},
        {0.0, 0.0, -1.0, 0.0},
    };
    return FromRows(rows, out);
}

MathStatus
TryPerspectiveReverseZInfinite(float32 verticalFovRadians, float32 aspect, float32 nearPlane, Matrix4& out) noexcept
{
    if (std::isfinite(verticalFovRadians) == false || std::isfinite(aspect) == false ||
        std::isfinite(nearPlane) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((aspect > 0.0f) == false || (nearPlane > 0.0f) == false || (verticalFovRadians > 0.0f) == false ||
        (verticalFovRadians < kPiF) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 k = 1.0 / std::tan(static_cast<float64>(verticalFovRadians) * 0.5);
    const float64 n = static_cast<float64>(nearPlane);
    const float64 rows[4][4] = {
        {k / static_cast<float64>(aspect), 0.0, 0.0, 0.0},
        {0.0, k, 0.0, 0.0},
        {0.0, 0.0, 0.0, n},
        {0.0, 0.0, -1.0, 0.0},
    };
    return FromRows(rows, out);
}

MathStatus TryOrthographicReverseZ(float32 left,
                                   float32 right,
                                   float32 bottom,
                                   float32 top,
                                   float32 nearPlane,
                                   float32 farPlane,
                                   Matrix4& out) noexcept
{
    if (std::isfinite(left) == false || std::isfinite(right) == false || std::isfinite(bottom) == false ||
        std::isfinite(top) == false || std::isfinite(nearPlane) == false || std::isfinite(farPlane) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((left < right) == false || (bottom < top) == false || (nearPlane >= 0.0f) == false ||
        (nearPlane < farPlane) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 l = static_cast<float64>(left), r = static_cast<float64>(right), b = static_cast<float64>(bottom),
                  t = static_cast<float64>(top), n = static_cast<float64>(nearPlane),
                  f = static_cast<float64>(farPlane);
    const float64 rows[4][4] = {
        {2.0 / (r - l), 0.0, 0.0, -(r + l) / (r - l)},
        {0.0, 2.0 / (t - b), 0.0, -(t + b) / (t - b)},
        {0.0, 0.0, 1.0 / (f - n), f / (f - n)},
        {0.0, 0.0, 0.0, 1.0},
    };
    return FromRows(rows, out);
}

MathStatus TryNdcToFramebuffer(Vector3 ndc, Viewport viewport, Vector2& out) noexcept
{
    if (IsFinite(ndc) == false || IsFinite(viewport.X) == false || IsFinite(viewport.Y) == false ||
        IsFinite(viewport.Width) == false || IsFinite(viewport.Height) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((viewport.Width > 0.0f) == false || (viewport.Height > 0.0f) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 px = (float64)viewport.X + ((float64)ndc.X + 1.0) * (float64)viewport.Width * 0.5;
    const float64 py = (float64)viewport.Y + (1.0 - (float64)ndc.Y) * (float64)viewport.Height * 0.5;
    const float32 fx = static_cast<float32>(px);
    const float32 fy = static_cast<float32>(py);
    if (IsFinite(fx) == false || IsFinite(fy) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector2{fx, fy};
    return MathStatus::Success;
}

MathStatus TryFramebufferToNdc(Vector2 pixel, Viewport viewport, Vector2& out) noexcept
{
    if (IsFinite(pixel) == false || IsFinite(viewport.X) == false || IsFinite(viewport.Y) == false ||
        IsFinite(viewport.Width) == false || IsFinite(viewport.Height) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((viewport.Width > 0.0f) == false || (viewport.Height > 0.0f) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 nx = ((float64)pixel.X - static_cast<float64>(viewport.X)) * 2.0 / (float64)viewport.Width - 1.0;
    const float64 ny = 1.0 - ((float64)pixel.Y - static_cast<float64>(viewport.Y)) * 2.0 / (float64)viewport.Height;
    const float32 fx = static_cast<float32>(nx);
    const float32 fy = static_cast<float32>(ny);
    if (IsFinite(fx) == false || IsFinite(fy) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector2{fx, fy};
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
