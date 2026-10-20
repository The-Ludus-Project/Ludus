#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/rhi/gpu_scope.h>

namespace ludus::graphics::rhi::internal
{
/// Native-independent bounded lifecycle. Native storage is owned by the backend until completion.
class GpuPassTimingBuffer final
{
public:
    [[nodiscard]] bool CanArm() const noexcept;
    void Arm() noexcept;
    void BeginFrame() noexcept;
    void Submit() noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool BeginScope(const GpuScopeDescription& scope) noexcept;
    [[nodiscard]] bool EndScope() noexcept;
    [[nodiscard]] foundation::usize AddPass(GpuPassKind kind) noexcept;
    [[nodiscard]] LUDUS_INLINE const GpuScopeDescription* GetScopeOrNull() const noexcept
    {
        return mHasScope == true ? &mScope : nullptr;
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
    GpuScopeDescription mScope;
    foundation::uint32 mScopePassIndex = 0;
    bool mHasScope = false;
    bool mIsCapturingFrame = false;
};
} // namespace ludus::graphics::rhi::internal
