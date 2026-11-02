#pragma once
// One owner for the CPU/GPU interaction region capacity. Overflow rejects a query; no triangle is dropped.
#define LUDUS_WAVE_REGION_TRIANGLES 256

// Wave-profile diagnostics, mirrored by the public RenderGraphFailureReason enum.
#define LUDUS_SCENE_FAILURE_WAVE_DIRECTION 22            // The aperture plane needs a nonzero propagation direction.
#define LUDUS_SCENE_FAILURE_WAVE_RESOLUTION 23           // Fresnel quadrature cannot resolve the phase gradient.
#define LUDUS_SCENE_FAILURE_WAVE_CAPACITY 24             // An optical region exceeds its triangle workspace.
#define LUDUS_SCENE_FAILURE_WAVE_CONNECTION 25           // Invalid scalar paraxial geometry or wavelength.
#define LUDUS_SCENE_FAILURE_WAVE_PROPAGATION_CAPACITY 26 // Too many successive optical interaction regions.
