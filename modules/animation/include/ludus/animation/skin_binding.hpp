#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/pose.hpp>
#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::animation
{
/// Default admission count for one mesh skin binding's palette.
inline constexpr foundation::usize DEFAULT_SKIN_BINDING_MAX_PALETTE_ENTRIES = 1024;

/// @brief One palette entry of a mesh skin binding.
///
/// InverseBindComponent maps mesh-space positions at bind time into the bound joint's space,
/// including any mesh-to-skeleton conversion. It belongs to this mesh binding, not the skeleton:
/// the bind pose is independent of the skeleton's rest pose, and modular meshes sharing a
/// skeleton may carry different inverse bind data.
struct SkinBindingJointRecord final
{
    foundation::uint64 JointId = 0;                   ///< Skeleton joint identity driving this palette entry.
    foundation::math::Affine3 InverseBindComponent{}; ///< Finite, invertible inverse bind transform.
};

/// @brief Borrowed cooked mesh skin binding, validated by TryPrepareSkinBinding.
struct SkinBindingPackage final
{
    /// Skeleton::GetCompatibility of the skeleton this mesh was bound against.
    foundation::Fingerprint128 SkeletonCompatibility{};
    /// Palette entries in mesh palette order; each joint appears at most once.
    foundation::BorrowedView<const SkinBindingJointRecord> Palette{nullptr, 0};
};

/// Per-binding preparation admission policy.
struct SkinBindingLimits final
{
    foundation::usize MaxPaletteEntries = DEFAULT_SKIN_BINDING_MAX_PALETTE_ENTRIES; ///< Positive palette limit.
};

namespace detail
{
struct SkinBindingStorage;
}

/// @brief Immutable, move-only validated mesh skin binding.
///
/// Owns the palette-to-skeleton joint index map, resolved once at preparation, and a copy of the
/// inverse bind transforms. Const reads are safe concurrently while the owner stays alive and unmoved.
class SkinBinding final
{
public:
    SkinBinding() noexcept;                               ///< Construct unprepared storage without allocation.
    ~SkinBinding() noexcept;                              ///< Release the binding; no readers may remain.
    SkinBinding(SkinBinding&& other) noexcept;            ///< Transfer ownership; other becomes unprepared.
    SkinBinding& operator=(SkinBinding&& other) noexcept; ///< Release this binding and take other's.
    SkinBinding(const SkinBinding&) = delete;             ///< Bindings are not implicitly copied.
    SkinBinding& operator=(const SkinBinding&) = delete;  ///< Bindings are not implicitly copied.

    [[nodiscard]] bool IsPrepared() const noexcept;                   ///< Whether a validated binding is owned.
    [[nodiscard]] foundation::uint32 GetPaletteSize() const noexcept; ///< Palette entries, or zero when unprepared.
    /// Compatibility identity of the bound skeleton; zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetCompatibility() const noexcept;
    /// Skeleton joint index for each palette entry; borrowed from this owner, empty when unprepared.
    [[nodiscard]] foundation::BorrowedView<const foundation::uint32> GetJointIndices() const noexcept;
    /// Inverse bind transform for each palette entry; borrowed from this owner, empty when unprepared.
    [[nodiscard]] foundation::BorrowedView<const foundation::math::Affine3> GetInverseBindTransforms() const noexcept;

private:
    friend AnimationStatus TryPrepareSkinBinding(const Skeleton& skeleton,
                                                 const SkinBindingPackage& package,
                                                 const SkinBindingLimits& limits,
                                                 SkinBinding& outBinding) noexcept;
    foundation::UniquePtr<detail::SkinBindingStorage> mStorage;
};

/// @brief Validate a mesh skin binding against a skeleton and publish it.
/// @param skeleton Prepared skeleton whose compatibility identity the package must name.
/// @param package Borrowed package; its views need not survive a successful return.
/// @param limits Palette admission policy.
/// @param outBinding Replaced only on Success.
/// @return Success, InvalidArgument (unprepared skeleton, empty/invalid palette or zero limit),
///         IncompatibleSkeleton, InvalidJointId (absent or repeated joint), InvalidTransform
///         (nonfinite or singular inverse bind), BudgetExceeded or OutOfMemory.
[[nodiscard]] AnimationStatus TryPrepareSkinBinding(const Skeleton& skeleton,
                                                    const SkinBindingPackage& package,
                                                    const SkinBindingLimits& limits,
                                                    SkinBinding& outBinding) noexcept;

/// @brief CPU skinning-palette oracle: palette[i] = Compose(component[joint[i]], inverseBind[i]).
///
/// At the mesh bind pose every entry is identity (within rounding); at the skeleton's rest pose
/// it generally is not. All entries are computed and checked before any is written.
/// @param binding Prepared skin binding.
/// @param pose Evaluated component pose for the binding's compatibility identity.
/// @param outPalette Caller-owned storage with exactly GetPaletteSize entries; unchanged on failure.
/// @return Success, InvalidArgument, IncompatibleSkeleton, NotEvaluated or OutOfRange. Allocation-free.
[[nodiscard]] AnimationStatus
TryComputeSkinningPalette(const SkinBinding& binding,
                          const ComponentPose& pose,
                          foundation::BorrowedView<foundation::math::Affine3> outPalette) noexcept;
} // namespace ludus::animation
