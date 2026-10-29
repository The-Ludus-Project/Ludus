#include "renderer_deferred_fullscreen.slang"

// Preserve an authored prepass's exact depth in the G-buffer's private depth
// attachment. RHI attachment ownership forbids sampling a bound color owner.
LUDUS_SHADER_BINDING(0) Texture2D<float> sourceDepthTexture LUDUS_SHADER_TEXTURE_REGISTER(0);
struct DeferredDepthOutput
{
    float Depth : SV_Depth;
};
// Selected by the recipe's explicit entry and stage, like most renderer recipes. Graph builds
// call it from their generated rgEntry; with a [shader] attribute here as well, the shader
// worker's Slang 2026.1.2 never finished its entry-point parameter transform for that stage.
DeferredDepthOutput DeferredDepthMain(in DeferredVarying value)
{
    DeferredDepthOutput output;
    output.Depth = sourceDepthTexture.Load(int3(int2(value.PositionClip.xy), 0));
    return output;
}
