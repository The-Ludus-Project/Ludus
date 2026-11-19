// Compare the LUSL program with the Slang oracle on the same Metal textures and samplers.
// Comparison samplers are exercised directly because RasterSamplerDescription exposes color
// samplers only. Both shaders follow the native binding-array cooker's SPIR-V-to-MSL path.
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Metal/Metal.h>

namespace
{
using namespace ludus::foundation;

constexpr NSUInteger COLOR_WIDTH = 2;
constexpr NSUInteger RGBA_CHANNELS = 4;
constexpr NSUInteger CUBE_FACES = 6;
constexpr NSUInteger VOLUME_SLOT = 2;
constexpr NSUInteger CUBE_SLOT = VOLUME_SLOT + 1;
constexpr NSUInteger LAYERS_SLOT = CUBE_SLOT + 1;
constexpr NSUInteger DEPTH_SLOT = LAYERS_SLOT + 1;
constexpr NSUInteger COLOR_SAMPLER_SLOT = DEPTH_SLOT + 1;
constexpr NSUInteger DEPTH_SAMPLER_SLOT = COLOR_SAMPLER_SLOT + 1;
constexpr NSUInteger SECOND_COLOR_ROW = 1;
constexpr NSUInteger VERTEX_COUNT = 3;
constexpr float64 CLEAR_DEPTH = 0.75;

using MetalTexture = id<MTLTexture>;
using MetalLibrary = id<MTLLibrary>;
using Pixel = StaticArray<float32, RGBA_CHANNELS>;

struct Readback final
{
    Pixel Samples;
    Pixel Comparison;
};

MetalTexture MakeColorTexture(id<MTLDevice> device, MTLTextureType kind, NSUInteger layers)
{
    MTLTextureDescriptor* description = [MTLTextureDescriptor new];
    description.textureType = kind;
    description.pixelFormat = MTLPixelFormatRGBA8Unorm;
    description.width = COLOR_WIDTH;
    description.height = COLOR_WIDTH;
    description.depth = kind == MTLTextureType3D ? layers : 1;
    description.arrayLength = kind == MTLTextureType2DArray ? layers : 1;
    description.storageMode = MTLStorageModeShared;
    description.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:description];
    REQUIRE(texture != nil);
    return texture;
}

// Upload two rows. Only the red channel matters, and each row can vary independently.
void UploadSlice(id<MTLTexture> texture, NSUInteger slice, uint8 redFirstRow, uint8 redSecondRow)
{
    constexpr NSUInteger ROW_BYTES = COLOR_WIDTH * RGBA_CHANNELS;
    constexpr NSUInteger IMAGE_BYTES = COLOR_WIDTH * ROW_BYTES;
    StaticArray<uint8, IMAGE_BYTES> bytes{};
    bytes[0] = redFirstRow;
    bytes[RGBA_CHANNELS] = redFirstRow;
    bytes[SECOND_COLOR_ROW * ROW_BYTES] = redSecondRow;
    bytes[SECOND_COLOR_ROW * ROW_BYTES + RGBA_CHANNELS] = redSecondRow;
    const bool isVolume = texture.textureType == MTLTextureType3D;
    [texture replaceRegion:MTLRegionMake3D(0, 0, isVolume == true ? slice : 0, COLOR_WIDTH, COLOR_WIDTH, 1)
               mipmapLevel:0
                     slice:isVolume == true ? 0 : slice
                 withBytes:bytes.GetData()
               bytesPerRow:ROW_BYTES
             bytesPerImage:IMAGE_BYTES];
}

MetalTexture MakeAttachment(id<MTLDevice> device, MTLPixelFormat format, MTLStorageMode storage)
{
    MTLTextureDescriptor* description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                           width:1
                                                                                          height:1
                                                                                       mipmapped:NO];
    description.storageMode = storage;
    description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:description];
    REQUIRE(texture != nil);
    return texture;
}

MetalLibrary CompileSource(id<MTLDevice> device, NSString* source)
{
    NSError* errorOrNull = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&errorOrNull];
    INFO((errorOrNull == nil ? "" : errorOrNull.localizedDescription.UTF8String));
    REQUIRE(library != nil);
    return library;
}

Readback Render(id<MTLDevice> device, NSString* path)
{
    NSError* errorOrNull = nil;
    NSString* source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&errorOrNull];
    REQUIRE(source != nil);
    id<MTLLibrary> fragment = CompileSource(device, source);
    id<MTLLibrary> vertex = CompileSource(device, @R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 Fullscreen(uint index [[vertex_id]])
{
    float2 corner = float2((index << 1) & 2, index & 2);
    return float4(corner * 2 - 1, 0, 1);
}
)");
    MTLRenderPipelineDescriptor* description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [vertex newFunctionWithName:@"Fullscreen"];
    description.fragmentFunction = [fragment newFunctionWithName:@"main0"];
    REQUIRE(description.fragmentFunction != nil);
    description.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    description.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:description error:&errorOrNull];
    INFO((errorOrNull == nil ? "" : errorOrNull.localizedDescription.UTF8String));
    REQUIRE(pipeline != nil);

    constexpr uint8 FULL_RED = 255;
    constexpr uint8 VOLUME_RED = 127;
    constexpr uint8 CUBE_RED = 63;
    constexpr uint8 LAYER_RED = 191;
    id<MTLTexture> first = MakeColorTexture(device, MTLTextureType2D, 1);
    id<MTLTexture> second = MakeColorTexture(device, MTLTextureType2D, 1);
    id<MTLTexture> volume = MakeColorTexture(device, MTLTextureType3D, 2);
    id<MTLTexture> cube = MakeColorTexture(device, MTLTextureTypeCube, 1);
    id<MTLTexture> layers = MakeColorTexture(device, MTLTextureType2DArray, 2);
    UploadSlice(first, 0, FULL_RED, FULL_RED);
    UploadSlice(second, 0, FULL_RED, 0);
    UploadSlice(volume, 0, 0, 0);
    UploadSlice(volume, 1, VOLUME_RED, VOLUME_RED);
    for (NSUInteger face = 0; face < CUBE_FACES; ++face)
    {
        const uint8 red = face == 0 ? CUBE_RED : 0;
        UploadSlice(cube, face, red, red);
    }
    UploadSlice(layers, 0, 0, 0);
    UploadSlice(layers, 1, LAYER_RED, LAYER_RED);

    id<MTLTexture> depth = MakeAttachment(device, MTLPixelFormatDepth32Float, MTLStorageModePrivate);
    id<MTLTexture> samples = MakeAttachment(device, MTLPixelFormatRGBA32Float, MTLStorageModeShared);
    id<MTLTexture> comparison = MakeAttachment(device, MTLPixelFormatRGBA32Float, MTLStorageModeShared);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    REQUIRE(commands != nil);
    MTLRenderPassDescriptor* clear = [MTLRenderPassDescriptor renderPassDescriptor];
    clear.depthAttachment.texture = depth;
    clear.depthAttachment.loadAction = MTLLoadActionClear;
    clear.depthAttachment.storeAction = MTLStoreActionStore;
    clear.depthAttachment.clearDepth = CLEAR_DEPTH;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:clear];
    REQUIRE(encoder != nil);
    [encoder endEncoding];

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = samples;
    pass.colorAttachments[1].texture = comparison;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    REQUIRE(encoder != nil);
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:first atIndex:0];
    [encoder setFragmentTexture:second atIndex:1];
    [encoder setFragmentTexture:volume atIndex:VOLUME_SLOT];
    [encoder setFragmentTexture:cube atIndex:CUBE_SLOT];
    [encoder setFragmentTexture:layers atIndex:LAYERS_SLOT];
    [encoder setFragmentTexture:depth atIndex:DEPTH_SLOT];
    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    id<MTLSamplerState> colorSampler = [device newSamplerStateWithDescriptor:sampler];
    sampler.compareFunction = MTLCompareFunctionLess;
    id<MTLSamplerState> depthSampler = [device newSamplerStateWithDescriptor:sampler];
    REQUIRE(colorSampler != nil);
    REQUIRE(depthSampler != nil);
    [encoder setFragmentSamplerState:colorSampler atIndex:COLOR_SAMPLER_SLOT];
    [encoder setFragmentSamplerState:depthSampler atIndex:DEPTH_SAMPLER_SLOT];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:VERTEX_COUNT];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    INFO((commands.error == nil ? "" : commands.error.localizedDescription.UTF8String));
    REQUIRE(commands.status == MTLCommandBufferStatusCompleted);

    Readback result{};
    constexpr NSUInteger PIXEL_BYTES = RGBA_CHANNELS * sizeof(float32);
    [samples getBytes:result.Samples.GetData()
          bytesPerRow:PIXEL_BYTES
           fromRegion:MTLRegionMake2D(0, 0, 1, 1)
          mipmapLevel:0];
    [comparison getBytes:result.Comparison.GetData()
             bytesPerRow:PIXEL_BYTES
              fromRegion:MTLRegionMake2D(0, 0, 1, 1)
             mipmapLevel:0];
    return result;
}
} // namespace

TEST_CASE("LUSL texture sampling and binding arrays match Slang on Metal", "[gpu][lusl]")
{
    @autoreleasepool
    {
        id<MTLDevice> deviceOrNull = MTLCreateSystemDefaultDevice();
        if (deviceOrNull == nil)
        {
            SKIP("No Metal adapter");
        }
        const Readback lusl = Render(deviceOrNull, @LUDUS_TEXTURE_PARITY_LUSL);
        const Readback slang = Render(deviceOrNull, @LUDUS_TEXTURE_PARITY_SLANG);
        for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
        {
            CAPTURE(channel);
            CHECK(lusl.Samples[channel] == slang.Samples[channel]);
            CHECK(lusl.Comparison[channel] == slang.Comparison[channel]);
        }
        constexpr Pixel EXPECTED_SAMPLES = {{1, 127.0F / 255.0F, 63.0F / 255.0F, 191.0F / 255.0F}};
        constexpr Pixel EXPECTED_COMPARISON = {{0.5F, 1, 0, 1}};
        constexpr float32 UNORM_ROUNDING_TOLERANCE = 0.000001F;
        for (usize channel = 0; channel < RGBA_CHANNELS; ++channel)
        {
            CAPTURE(channel);
            const float32 difference = lusl.Samples[channel] - EXPECTED_SAMPLES[channel];
            CHECK(difference <= UNORM_ROUNDING_TOLERANCE);
            CHECK(difference >= -UNORM_ROUNDING_TOLERANCE);
            CHECK(lusl.Comparison[channel] == EXPECTED_COMPARISON[channel]);
        }
    }
}
