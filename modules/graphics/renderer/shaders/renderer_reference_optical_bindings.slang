// The optical connections of renderer_reference_optical.lusl (LUSL L5d wave 2): the wave scene queries, after the
// reference kernels' bindings and libraries (renderer_reference_bindings.slang). The kernel reaches this file through
// `extern slang` (renderer_reference_optical_bindings.luslh); wave_reference_scene.slang calls the kernel's
// SceneValue and IsBvhCandidateAccepted.
#pragma once

#include "renderer_reference_bindings.slang"
#include "wave_reference_scene.slang"
