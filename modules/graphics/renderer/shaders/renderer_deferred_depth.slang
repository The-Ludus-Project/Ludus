#include "renderer_deferred_fullscreen.slang"

// Preserve an authored prepass's exact depth in the G-buffer's private depth
// attachment. RHI attachment ownership forbids sampling a bound color owner.
LUDUS_SHADER_BINDING(0) Texture2D<float> sourceDepthTexture LUDUS_SHADER_TEXTURE_REGISTER(0);
struct DeferredDepthOutput
{
    float Depth : SV_Depth;
};
[shader("fragment")] DeferredDepthOutput DeferredDepthMain(in DeferredVarying value) {
    DeferredDepthOutput output;
    output.Depth = sourceDepthTexture.Load(int3(int2(value.PositionClip.xy), 0));
    return output;
}
