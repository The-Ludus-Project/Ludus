#include "internal/surface.h"

// R14b single-surface backend (the browser dispatcher): the session hosts only its
// primary surface, so CreateSurface reports Unsupported before any backend work.
// Primary-surface completion is not tracked per surface on this backend.
namespace ludus::graphics::rhi::backend
{
foundation::usize AdditionalSurfaceLimit() noexcept
{
    return 0;
}
StartupError CreateSurface(foundation::usize, const WindowInfo&) noexcept
{
    return StartupError::BackendUnavailable;
}
void DestroySurface(foundation::usize) noexcept {}
void SelectSurface(foundation::usize) noexcept {}
SurfaceCompletion GetSurfaceCompletion(foundation::usize) noexcept
{
    return {};
}
} // namespace ludus::graphics::rhi::backend
