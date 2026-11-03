#pragma once
// One owner for the CPU/GPU interaction region capacity. Overflow rejects a query; no triangle is dropped.
#define LUDUS_WAVE_REGION_TRIANGLES 256

// WaveQuery outcomes written by wave scene queries and connections; host parity tests read them back.
#define LUDUS_WAVE_QUERY_READY 0                // Finished; a region query may report a miss.
#define LUDUS_WAVE_QUERY_INVALID 1              // Invalid geometry, cone or hierarchy input.
#define LUDUS_WAVE_QUERY_CAPACITY 2             // An interaction region exceeded LUDUS_WAVE_REGION_TRIANGLES.
#define LUDUS_WAVE_QUERY_PROPAGATION_CAPACITY 3 // Too many successive optical interaction regions.
#define LUDUS_WAVE_QUERY_ESCAPE_CAPACITY 4      // Escaping a connection endpoint needed too many attempts.

// Wave-profile diagnostics, mirrored by the public RenderGraphFailureReason enum.
#define LUDUS_SCENE_FAILURE_WAVE_DIRECTION 22            // The aperture plane needs a nonzero propagation direction.
#define LUDUS_SCENE_FAILURE_WAVE_RESOLUTION 23           // Fresnel quadrature cannot resolve the phase gradient.
#define LUDUS_SCENE_FAILURE_WAVE_CAPACITY 24             // An optical region exceeds its triangle workspace.
#define LUDUS_SCENE_FAILURE_WAVE_CONNECTION 25           // Invalid scalar paraxial geometry or wavelength.
#define LUDUS_SCENE_FAILURE_WAVE_PROPAGATION_CAPACITY 26 // Too many successive optical interaction regions.
