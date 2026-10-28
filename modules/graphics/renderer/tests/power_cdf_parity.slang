// Test-only kernel for power_cdf_parity_tests.cpp: runs the public power-CDF scene accessors over
// a host-packed scene so the host can compare them with the CPU reference. No ray is traced, so the
// unused preview bindings leave the reflected layout while keeping graph wrappers' slot order.
#include "power_cdf_parity_layout.h"
#include "renderer_gpu_preview.slang"
#include "scene_shader.slang"

LUDUS_SHADER_BINDING(LUDUS_POWER_CDF_PARITY_RESULT_BINDING) RWStructuredBuffer<uint4> Results;

[numthreads(LUDUS_POWER_CDF_PARITY_GROUP_SIZE, 1, 1)] void
PowerCdfParityMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    if (invocation >= Dimensions.x * Dimensions.y)
        return;
    SceneContext scene = (SceneContext)0;
    scene.Version = SCENE_INTERFACE_VERSION;
    RayLaunch launch = (RayLaunch)0;
    launch.ExtentPixel = Dimensions.xy;
    launch.PositionPixel = uint2(invocation % Dimensions.x, invocation / Dimensions.x);
    launch.SampleIndex = Dimensions.z;
    launch.Seed = Sampling.xy;
    uint dimension =
        invocation % LUDUS_POWER_CDF_PARITY_INVALID_PERIOD == 0
            ? RANDOM_MAX_DIMENSION + 1
            : LUDUS_POWER_CDF_PARITY_FIRST_TICKET_DIMENSION + 2 * (invocation % LUDUS_POWER_CDF_PARITY_TICKET_COUNT);
    EmissiveTriangleSelection selection = SelectEmissiveTriangle(scene, launch, dimension);
    Results[invocation] = uint4(selection.TriangleIndex,
                                asuint(selection.SelectionPmf),
                                selection.HasSample == true ? 1 : 0,
                                asuint(EvaluateEmissiveTriangleSelectionPmf(scene, invocation)));
}
