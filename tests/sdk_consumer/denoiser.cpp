#include <ludus/graphics/renderer/render_graph_activation.hpp>

// An SDK built with the denoiser reports it and, by linking Renderer, loads the
// OIDN runtime bundled under its prefix when this consumer starts.
bool ExerciseInstalledDenoiser() noexcept
{
    return ludus::graphics::renderer::IsDenoisingAvailable() == (EXPECTED_DENOISER != 0);
}
