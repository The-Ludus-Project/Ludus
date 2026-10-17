#pragma once

// Compile-time seam for a separate, unexported startup-fault test backend.
// Production builds call the OS directly and contain no hook dispatch or fault
// state. Mach-O and PE linkers lack GNU --wrap, so macOS and Windows inject the
// same failures as the Linux wrapper test through these functions, which the
// test executable defines. Each hook runs before the real OS call it guards.
namespace ludus::foundation::threading::detail::test
{
/// Returns true to report thread creation failure without creating a thread.
bool ShouldFailThreadStart() noexcept;
/// Records one join of a started native thread, before the OS join.
void RecordThreadJoin() noexcept;
/// Returns true to report gate mutex initialization failure.
bool ShouldFailMutexInitialization() noexcept;
/// Returns true to report gate condition initialization failure.
bool ShouldFailConditionInitialization() noexcept;
} // namespace ludus::foundation::threading::detail::test
