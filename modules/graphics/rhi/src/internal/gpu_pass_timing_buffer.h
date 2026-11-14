#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::rhi::internal
{
/// Position of the innermost open scope. Epoch changes whenever the stack is discarded, at
/// submission or reset, so a lexical scope from an earlier frame cannot close a newer one.
struct GpuScopePosition final
{
    foundation::uint64 Epoch = 0;
    foundation::uint32 Depth = 0;
};
/// Native-independent bounded lifecycle. Native storage is owned by the backend until completion.
/// Scopes nest up to MAX_GPU_SCOPE_DEPTH; timed physical operations belong to the outermost one.
class GpuPassTimingBuffer final
{
public:
    [[nodiscard]] bool CanArm() const noexcept;
    void Arm() noexcept;
    void BeginFrame() noexcept;
    void Submit() noexcept;
    void Reset() noexcept;
    /// Push scope. Returns false, without change, when the stack is full.
    [[nodiscard]] bool BeginScope(const GpuScopeDescription& scope) noexcept;
    /// Pop the innermost scope. Returns false when no scope is open.
    [[nodiscard]] bool EndScope() noexcept;
    /// Discard every open scope and advance the epoch; returns how many native regions to close.
    [[nodiscard]] foundation::uint32 CloseScopes() noexcept;
    [[nodiscard]] foundation::usize AddPass(GpuPassKind kind) noexcept;
    /// Innermost open scope, which labels native encoders.
    [[nodiscard]] LUDUS_INLINE const GpuScopeDescription* GetScopeOrNull() const noexcept
    {
        return mDepth == 0 ? nullptr : &mScopes[mDepth - 1];
    }
    /// Open scope at depth index, outermost first; index must be below GetDepth().
    [[nodiscard]] LUDUS_INLINE const GpuScopeDescription& GetScope(foundation::uint32 index) const noexcept
    {
        return mScopes[index];
    }
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetDepth() const noexcept
    {
        return mDepth;
    }
    [[nodiscard]] LUDUS_INLINE GpuScopePosition GetPosition() const noexcept
    {
        return { .Epoch = mEpoch, .Depth = mDepth };
    }
    [[nodiscard]] LUDUS_INLINE GpuPassTimings& GetReport() noexcept
    {
        return mReport;
    }
    [[nodiscard]] LUDUS_INLINE const GpuPassTimings& GetReport() const noexcept
    {
        return mReport;
    }

private:
    GpuPassTimings mReport;
    foundation::StaticArray<GpuScopeDescription, MAX_GPU_SCOPE_DEPTH> mScopes{};
    foundation::uint64 mEpoch = 1;
    foundation::uint32 mDepth = 0;
    foundation::uint32 mScopePassIndex = 0;
    bool mIsCapturingFrame = false;
};
} // namespace ludus::graphics::rhi::internal
