#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::animation
{
/// @brief Result of every fallible Animation operation.
///
/// A failing status never replaces a published output: prepared skeletons, poses, bindings and
/// inspection records keep their previous contents. Multi-failure precedence is not an ABI promise;
/// tests should assert a relevant failure rather than one ordering when several inputs are invalid.
/// See "Implementation and acceptance" in docs/architecture/skeletal-animation.md.
enum class AnimationStatus : foundation::uint8
{
    Success,              ///< A complete validated result was published.
    InvalidArgument,      ///< An invalid view, limit, identity, unit, index or unprepared input was supplied.
    InvalidHierarchy,     ///< Joints are not one depth-first, parent-before-child tree with matching subtree ends.
    InvalidJointId,       ///< A joint identity is zero, duplicated or absent from the skeleton.
    InvalidTransform,     ///< A translation/rotation/matrix is nonfinite, nonunit or not invertible.
    UnsupportedScale,     ///< A joint scale is outside the strictly positive supported domain.
    IncompatibleSkeleton, ///< A pose or binding was prepared for a different skeleton compatibility identity.
    BudgetExceeded,       ///< The requested joint or palette count exceeds the supplied limits.
    OutOfMemory,          ///< A fallible preparation allocation failed.
    OutOfRange,           ///< Composition produced a nonfinite (unrepresentable) component transform.
    NotEvaluated,         ///< A component pose has no published evaluation to read.
};
} // namespace ludus::animation
