// Scalar batch operations (design §12). Precise FP flags; <cmath>/<limits>
// permitted (ADR 0003/0010). No heap scratch; the preflight recomputes.
// Thanks to Nicolas Guillemot and Marc Fauconneau Dufresne, "Portable SIMD
// Programs Using ISPC", Game Engine Gems 3 (2016), chapter 16, PDF pp. 204-213,
// for the bulk-across-objects design direction. These are scalar reference
// loops with caller-owned output and preflight validation, not an ISPC port or
// a claim of vectorization. Reviewed scope: docs/architecture/math-research.md.

#include <ludus/foundation/math/batch.hpp>
#include <ludus/foundation/math/queries.hpp>

#include <cmath>
#include <cstdint>

namespace ludus::foundation::math
{
namespace
{
// Address-range overlap classification for two byte ranges [aBegin,aEnd) and
// [bBegin,bEnd), computed on uintptr_t with overflow checks. Returns:
//   0 = disjoint, 1 = exactly identical range, 2 = partial (any other) overlap.
// Using uintptr_t avoids UB from comparing pointers into different objects.
enum class Overlap : uint8
{
    Disjoint,
    Exact,
    Partial,
};

[[nodiscard]] Overlap ClassifyOverlap(const void* a, usize aBytes, const void* b, usize bBytes) noexcept
{
    const std::uintptr_t aBegin = reinterpret_cast<std::uintptr_t>(a);
    const std::uintptr_t bBegin = reinterpret_cast<std::uintptr_t>(b);
    // Overflow-checked ends. If begin+bytes overflows uintptr_t, treat the range
    // as covering to the top; this is conservative and never under-reports.
    const std::uintptr_t maxPtr = static_cast<std::uintptr_t>(-1);
    const std::uintptr_t aEnd = (aBytes == 0) ? aBegin : (aBegin > maxPtr - aBytes ? maxPtr : aBegin + aBytes);
    const std::uintptr_t bEnd = (bBytes == 0) ? bBegin : (bBegin > maxPtr - bBytes ? maxPtr : bBegin + bBytes);
    if (aBytes == 0 || bBytes == 0)
    {
        return Overlap::Disjoint;
    }
    if (aBegin == bBegin && aEnd == bEnd && aBytes == bBytes)
    {
        return Overlap::Exact;
    }
    // Disjoint if one ends at or before the other begins.
    if (aEnd <= bBegin || bEnd <= aBegin)
    {
        return Overlap::Disjoint;
    }
    return Overlap::Partial;
}
} // namespace

MathStatus
TryTransformPoints(const Affine3& transform, std::span<const Vector3> input, std::span<Vector3> output) noexcept
{
    if (input.size() != output.size())
    {
        return MathStatus::SizeMismatch;
    }
    if (IsFinite(transform) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (input.empty())
    {
        return MathStatus::Success;
    }

    const Overlap overlap = ClassifyOverlap(input.data(), input.size_bytes(), output.data(), output.size_bytes());
    if (overlap == Overlap::Partial)
    {
        return MathStatus::InvalidArgument; // only exact in-place or disjoint allowed
    }

    // Prevalidate: every input finite AND every transformed point finite, before
    // writing anything (transactional, all-or-nothing). Recomputed in the write
    // loop; no heap scratch.
    for (usize i = 0; i < input.size(); ++i)
    {
        const Vector3 p = input[i];
        if (IsFinite(p) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        const Vector3 r = TransformPoint(transform, p);
        if (IsFinite(r) == false)
        {
            return MathStatus::OutOfRange;
        }
    }

    // Write. For exact in-place, read each element into a local before writing.
    for (usize i = 0; i < input.size(); ++i)
    {
        const Vector3 p = input[i];
        output[i] = TransformPoint(transform, p);
    }
    return MathStatus::Success;
}

MathStatus TryClassifySpheres(const Frustum& frustum,
                              std::span<const Sphere> input,
                              float32 margin,
                              std::span<FrustumRelation> output) noexcept
{
    if (input.size() != output.size())
    {
        return MathStatus::SizeMismatch;
    }
    if (std::isfinite(margin) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (margin < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }
    if (input.empty())
    {
        return MathStatus::Success;
    }
    // Input and output must be disjoint (different element types, but a buffer
    // can still be reinterpreted; reject any overlap).
    const Overlap overlap = ClassifyOverlap(input.data(), input.size_bytes(), output.data(), output.size_bytes());
    if (overlap != Overlap::Disjoint)
    {
        return MathStatus::InvalidArgument;
    }

    // Prevalidate every sphere (and that classification succeeds) before writing.
    for (usize i = 0; i < input.size(); ++i)
    {
        FrustumRelation relation = FrustumRelation::Outside;
        const MathStatus status = TryClassifySphere(frustum, input[i], margin, relation);
        if (status != MathStatus::Success)
        {
            return status;
        }
    }
    for (usize i = 0; i < input.size(); ++i)
    {
        FrustumRelation relation = FrustumRelation::Outside;
        (void)TryClassifySphere(frustum, input[i], margin, relation);
        output[i] = relation;
    }
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
