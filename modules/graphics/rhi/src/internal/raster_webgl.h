#pragma once

#include "profile.h"
// Included only by rhi_webgl.cpp. Thanks to Khronos, OpenGL ES 3.0.6,
// sections 2.8 (vertex arrays), 2.12.6 (uniform blocks) and 5.2 (sync objects):
// https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf
// GL syncs are polled without blocking; shader metadata comes from the offline
// GLSL ES artifact, independently of WGSL/Metal/SPIR-V packing.
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
{
namespace
{
struct RasterGlShader final
{
    GLuint Object = 0;
    internal::RasterShaderInfo Info;
};
struct RasterGlPipeline final
{
    GLuint Object = 0;
    GLuint Array = 0;
    internal::RasterPipelineInfo Info;
    foundation::StaticArray<GLint, internal::RASTER_BINDINGS> Textures{};
    foundation::StaticArray<uint32, internal::RASTER_BINDINGS> Samplers{};
    RasterGlPipeline() noexcept
    {
        for (GLint& location : Textures)
        {
            location = -1;
        }
    }
};
struct RasterGlCompletion final
{
    GLsync Sync = nullptr;
    uint64 Ordinal = 0;
    bool Reserved = false;
};
internal::StorageTable<GLuint> gRasterBuffers;
internal::StorageTable<GLuint> gRasterTextures;
internal::StorageTable<TextureDescription> gRasterTextureDescriptions;
internal::StorageTable<GLuint> gRasterFramebuffers;
internal::StorageTable<GLuint> gRasterRenderTextures;
internal::StorageTable<GLuint> gRasterSampleFramebuffers;
internal::RasterPassInfo gRasterGlPass;
bool gRasterGlPassOpen = false;
bool gRasterHdrProbed = false, gRasterHdr = false;
internal::StorageTable<GLuint> gRasterDepths;
internal::StorageTable<GLuint> gRasterMsaaColors;
GLuint gRasterMrtFramebuffer = 0, gRasterResolveFramebuffer = 0;
struct RasterGlView final
{
    GLuint Object = 0;
    GLenum Target = GL_TEXTURE_2D;
    bool Owned = false;
};
internal::StorageTable<RasterGlView> gRasterViews;
internal::StorageTable<GLuint> gRasterSamplers;
internal::StorageTable<RasterGlShader> gRasterShaders;
internal::StorageTable<internal::RasterLayout> gRasterLayouts;
internal::StorageTable<internal::RasterSet> gRasterSets;
internal::StorageTable<RasterGlPipeline> gRasterPipelines;
internal::StorageTable<RasterGlCompletion> gRasterCompletions;
RasterGlCompletion* gRasterReserved = nullptr;
uint64 gRasterCompleted = 0;
GLenum RasterGlTextureTarget(TextureDimension dimension) noexcept
{
    return dimension == TextureDimension::D2Array ? GL_TEXTURE_2D_ARRAY
           : dimension == TextureDimension::Cube  ? GL_TEXTURE_CUBE_MAP
           : dimension == TextureDimension::D3    ? GL_TEXTURE_3D
                                                  : GL_TEXTURE_2D;
}
GLenum RasterGlTextureFormat(RasterFormat format) noexcept
{
    return format == RasterFormat::Rgba16Float ? GL_RGBA16F
           : format == RasterFormat::R8Unorm   ? GL_R8
           : format == RasterFormat::Rg8Unorm  ? GL_RG8
           : format == RasterFormat::Rgba8Srgb ? GL_SRGB8_ALPHA8
                                               : GL_RGBA8;
}
RasterStatus RasterGlResult() noexcept
{
    const auto error = glGetError();
    return error == GL_NO_ERROR        ? RasterStatus::Ready
           : error == GL_OUT_OF_MEMORY ? RasterStatus::OutOfMemory
                                       : RasterStatus::Failed;
}
bool RasterGlInterface(GLuint program, const internal::RasterShaderInfo& shader, RasterGlPipeline& pipeline) noexcept
{
    for (usize i = 0; i < shader.Layout.Count; ++i)
    {
        const auto& entry = shader.Layout.Entries[i];
        if (entry.Kind == RasterBindingKind::UniformBuffer)
        {
            const GLuint block = glGetUniformBlockIndex(program, shader.UniformBlocks[entry.Binding]);
            if (block == GL_INVALID_INDEX)
            {
                return false;
            }
            GLint size = 0;
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
            if (size <= 0 || static_cast<usize>(size) > entry.MinSize)
            {
                return false;
            }
            glUniformBlockBinding(program, block, entry.Binding);
        }
        else if (internal::TextureBinding(entry.Kind))
        {
            const GLint location = glGetUniformLocation(program, shader.TextureNames[entry.Binding]);
            if (location < 0)
            {
                return false;
            }
            if (pipeline.Textures[entry.Binding] >= 0 &&
                (pipeline.Textures[entry.Binding] != location ||
                 pipeline.Samplers[entry.Binding] != shader.TextureSamplers[entry.Binding]))
            {
                return false;
            }
            pipeline.Textures[entry.Binding] = location;
            pipeline.Samplers[entry.Binding] = shader.TextureSamplers[entry.Binding];
        }
    }
    return true;
}
bool RasterGlProgram(GLuint program, RasterGlPipeline& pipeline) noexcept
{
    const auto& vertex = gRasterShaders[pipeline.Info.Vertex].Info;
    const auto& fragment = gRasterShaders[pipeline.Info.Fragment].Info;
    if (RasterGlInterface(program, vertex, pipeline) == false ||
        RasterGlInterface(program, fragment, pipeline) == false)
    {
        return false;
    }
    GLint count = 0;
    // Thanks to Khronos, OpenGL ES 3.0.6, section 2.12.5 "Vertex Attributes":
    // linked programs expose only active attributes. Shadow fragments can eliminate
    // unused lit vertex inputs; verify every active location/type against the cook.
    // https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    if (count < 0 || static_cast<usize>(count) > vertex.InputCount)
    {
        return false;
    }
    for (GLint i = 0; i < count; ++i)
    {
        char name[128]{};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(program, static_cast<GLuint>(i), sizeof(name), &length, &size, &type, name);
        if (size != 1 || length <= 0 || length >= static_cast<GLsizei>(sizeof(name) - 1))
        {
            return false;
        }
        const auto location = glGetAttribLocation(program, name);
        bool found = false;
        for (usize j = 0; j < vertex.InputCount; ++j)
        {
            const auto& input = vertex.Inputs[j];
            const GLenum expected = input.Format == RasterVertexFormat::Float2   ? GL_FLOAT_VEC2
                                    : input.Format == RasterVertexFormat::Float3 ? GL_FLOAT_VEC3
                                                                                 : GL_FLOAT_VEC4;
            if (location == static_cast<GLint>(input.Location) && type == expected)
            {
                found = true;
            }
        }
        if (found == false)
        {
            return false;
        }
    }
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &count);
    for (GLint i = 0; i < count; ++i)
    {
        char name[128]{};
        GLsizei length = 0;
        glGetActiveUniformBlockName(program, static_cast<GLuint>(i), sizeof(name), &length, name);
        bool found = false;
        for (usize binding = 0; binding < internal::RASTER_BINDINGS; ++binding)
        {
            if (std::strcmp(name, vertex.UniformBlocks[binding]) == 0 ||
                std::strcmp(name, fragment.UniformBlocks[binding]) == 0)
            {
                found = true;
            }
        }
        if (found == false || length <= 0 || length >= static_cast<GLsizei>(sizeof(name) - 1))
        {
            return false;
        }
    }
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i)
    {
        const auto index = static_cast<GLuint>(i);
        GLint block = -1;
        glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &block);
        if (block >= 0)
        {
            continue;
        }
        char name[128]{};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, index, sizeof(name), &length, &size, &type, name);
        if (size != 1 ||
            (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_ARRAY && type != GL_SAMPLER_CUBE &&
             type != GL_SAMPLER_3D) ||
            length <= 0 || length >= static_cast<GLsizei>(sizeof(name) - 1))
        {
            return false;
        }
        bool found = false;
        for (usize binding = 0; binding < internal::RASTER_BINDINGS; ++binding)
        {
            if (std::strcmp(name, vertex.TextureNames[binding]) == 0 ||
                std::strcmp(name, fragment.TextureNames[binding]) == 0)
            {
                const internal::RasterShaderInfo* stages[2]{&vertex, &fragment};
                for (const auto* shader : stages)
                {
                    for (usize j = 0; j < shader->Layout.Count; ++j)
                    {
                        const auto& entry = shader->Layout.Entries[j];
                        if (entry.Binding != binding || internal::TextureBinding(entry.Kind) == false)
                        {
                            continue;
                        }
                        const GLenum expected = entry.Kind == RasterBindingKind::Texture2DArray ? GL_SAMPLER_2D_ARRAY
                                                : entry.Kind == RasterBindingKind::TextureCube  ? GL_SAMPLER_CUBE
                                                : entry.Kind == RasterBindingKind::Texture3D    ? GL_SAMPLER_3D
                                                                                                : GL_SAMPLER_2D;
                        found |= type == expected;
                    }
                }
            }
        }
        if (found == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace
ComputeCapabilities ComputeLimits() noexcept
{
    return {};
}
RasterStatus ComputeCreatePipeline(usize, const internal::ComputePipelineInfo&, uint32) noexcept
{
    return RasterStatus::Unsupported;
}
RasterStatus ComputeEncode(const internal::ComputePacket&) noexcept
{
    return RasterStatus::Unsupported;
}
void ComputeBufferBarrier(usize, GraphAccessMode) noexcept {}
// Thanks to Khronos, "WebGL 2.0 Specification", vertexAttribIPointer and
// WebGL 1.0 section "Vertex Attribute Data Stride": the API caps stride at 255.
// Ludus uses four-byte-aligned float attributes, so its largest legal stride is 252.
// https://registry.khronos.org/webgl/specs/1.0/#VERTEX_ATTRIB_POINTER
uint32 RasterColorAttachmentLimit() noexcept
{
    GLint colors = 0, buffers = 0;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &colors);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &buffers);
    const auto count = colors < buffers ? colors : buffers;
    return count > 4 ? 4U : count > 0 ? static_cast<uint32>(count) : 0U;
}
uint32 RasterVertexStrideLimit() noexcept
{
    return 252;
}
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage&) noexcept
{
    return false;
}
BindingCapabilities BindingLimits() noexcept
{
    GLint vertexBlocks = 0;
    GLint fragmentBlocks = 0;
    GLint vertexTextures = 0;
    GLint fragmentTextures = 0;
    GLint uniformBindings = 0;
    GLint textureUnits = 0;
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &vertexBlocks);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &fragmentBlocks);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextures);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &fragmentTextures);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &uniformBindings);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &textureUnits);
    const uint32 uniforms = static_cast<uint32>(vertexBlocks < fragmentBlocks ? vertexBlocks : fragmentBlocks);
    const uint32 textures = static_cast<uint32>(vertexTextures < fragmentTextures ? vertexTextures : fragmentTextures);
    const uint32 nativeBindings = static_cast<uint32>(uniformBindings < textureUnits ? uniformBindings : textureUnits);
    const uint32 bindings =
        nativeBindings < internal::RASTER_BINDINGS ? nativeBindings : static_cast<uint32>(internal::RASTER_BINDINGS);
    return
    {
        .MaxDynamicUniformBuffers = uniforms < bindings ? uniforms : bindings,
        .MaxTextureArrayElements = 1,
        .MaxDescriptorsPerSet = internal::CurrentStorageLimits().DescriptorsPerSet < bindings
                                    ? internal::CurrentStorageLimits().DescriptorsPerSet
                                    : bindings,
        .MaxBindings = bindings,
        .MaxUniformBuffersPerStage = uniforms < bindings ? uniforms : bindings,
        .MaxSampledTexturesPerStage = textures < bindings ? textures : bindings,
        .MaxSamplersPerStage = textures < bindings ? textures : bindings,
        .MaxUniformBuffersPerSet = bindings,
        .MaxSamplersPerSet = bindings,
    };
}
RasterCapabilities RasterLimits() noexcept
{
    if (gContext <= 0)
    {
        return {};
    }
    GLint textureDimension = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &textureDimension);
    GLint alignment = 0;
    GLint uniform = 0;
    GLint attributes = 0;
    GLint textures = 0;
    GLint vertexTextures = 0;
    GLint vertexBlocks = 0;
    GLint fragmentBlocks = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &uniform);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textures);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextures);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &vertexBlocks);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &fragmentBlocks);
    if (textureDimension <= 0 || alignment <= 0 || uniform < 16384 || attributes < 8 || textures < 8 ||
        vertexTextures < 8 || vertexBlocks < 8 || fragmentBlocks < 8)
    {
        return {};
    }
    if (gRasterHdrProbed == false)
    {
        gRasterHdr = emscripten_webgl_enable_extension(gContext, "EXT_color_buffer_float") == EM_TRUE;
        gRasterHdrProbed = true;
    }
    // WebGL has no queryable buffer-size maximum; retain a signed-32-bit API bound,
    // independently intersected with the caller-configured logical resource budget.
    const usize bufferBytes = internal::GetResourceSizeLimit(0x7fffffffU);
    return {bufferBytes,
            static_cast<uint32>(textureDimension),
            static_cast<usize>(alignment > 256 ? alignment : 256),
            16384,
            internal::CurrentStorageLimits().ResourcesPerKind,
            internal::CurrentStorageLimits().DrawsPerFrame,
            static_cast<uint32>(attributes < 16 ? attributes : 16),
            gRasterHdr,
            bufferBytes};
}
// Thanks to Khronos Group, "WebGL 2.0 Specification", texture formats and GLES 3.0 constraints.
// https://registry.khronos.org/webgl/specs/2.0.0/
// Intersect guaranteed normalized formats and enabled HDR with implemented uses;
// no storage/copy/MSAA or R8/RG8 attachment path is advertised.
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    if (gContext <= 0 || internal::CompressedTexture(format))
    {
        return {};
    }
    FormatCapabilities result = internal::PortableFormatLimits(format, RasterLimits());
    if (result.Uses.ColorAttachment)
    {
        GLint renderbufferDimension = 0;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbufferDimension);
        // The private depth/stencil renderbuffer and raster viewport can be smaller than sampled textures.
        if (renderbufferDimension <= 0)
        {
            result.MaxAttachmentDimension2D = 0;
            result.Uses.ColorAttachment = false;
            result.Uses.Blendable = false;
            result.SampleCounts = 0;
        }
        else
        {
            const uint32 depthDimension = static_cast<uint32>(renderbufferDimension);
            if (depthDimension < result.MaxAttachmentDimension2D)
            {
                result.MaxAttachmentDimension2D = depthDimension;
            }
            if (gMaxDimension < result.MaxAttachmentDimension2D)
            {
                result.MaxAttachmentDimension2D = gMaxDimension;
            }
        }
        result.MaxAttachmentMipLevels = 1;
        result.MaxAttachmentLayers = 1;
        result.AttachmentViews = true;
        result.StencilAttachment = true;
        // WebGL exposes only SAMPLES (a bounded Int32Array), not NUM_SAMPLE_COUNTS.
        // The bridge copies the returned elements; zeroed trailing entries are ignored.
        GLint colorSamples[32]{}, depthSamples[32]{};
        glGetInternalformativ(GL_RENDERBUFFER, RasterGlTextureFormat(format), GL_SAMPLES, 32, colorSamples);
        glGetInternalformativ(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, GL_SAMPLES, 32, depthSamples);
        bool color4 = false, depth4 = false;
        for (usize i = 0; i < 32; ++i)
        {
            color4 |= colorSamples[i] == 4;
            depth4 |= depthSamples[i] == 4;
        }
        if (color4 && depth4)
        {
            result.SampleCounts |= 4;
        }
    }
    if (result.Uses.Sampled)
    {
        GLint layers = 0, volume = 0, cube = 0;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);
        glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &volume);
        glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &cube);
        result.MaxArrayLayers = layers > 0 ? static_cast<uint32>(layers) : 0;
        result.MaxDimension3D = volume > 0 ? static_cast<uint32>(volume) : 0;
        result.MaxCubeDimension = cube > 0 ? static_cast<uint32>(cube) : 0;
        if (result.MaxCubeDimension > result.MaxSampledDimension2D)
        {
            result.MaxCubeDimension = result.MaxSampledDimension2D;
        }
        // Snapshot copies use framebuffer color reads. Normalized formats are core-renderable;
        // HDR partial copies require the separately probed float color-buffer extension.
        result.PartialViews = format != RasterFormat::Rgba16Float || result.Uses.ColorAttachment;
    }
    return result;
}

RasterStatus
RasterCreateBuffer(usize slot, const BufferDescription& info, std::span<const uint8> bytes, uint32) noexcept
{
    const bool indices = info.Role == BufferRole::Index16 || info.Role == BufferRole::Index32;
    const GLenum target = indices                            ? GL_ELEMENT_ARRAY_BUFFER
                          : info.Role == BufferRole::Uniform ? GL_UNIFORM_BUFFER
                                                             : GL_ARRAY_BUFFER;
    // WebGL classifies index buffers at their first binding. Use the default
    // VAO during setup so initialization cannot replace a pipeline's index binding.
    if (indices)
    {
        glBindVertexArray(0);
    }
    glGenBuffers(1, &gRasterBuffers[slot]);
    glBindBuffer(target, gRasterBuffers[slot]);
    glBufferData(target, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STATIC_DRAW);
    glBindBuffer(target, 0);
    return RasterGlResult();
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32) noexcept
{
    gRasterTextureDescriptions[slot] = info;
    glGenTextures(1, &gRasterTextures[slot]);
    const auto target = RasterGlTextureTarget(info.Dimension);
    glBindTexture(target, gRasterTextures[slot]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    uint32 width = info.Width, height = info.Height;
    for (uint32 level = 0; level < info.MipLevels; ++level)
    {
        const auto mip = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
        const auto channels = internal::TextureBlockBytes(info.Format);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(mip.RowPitch / channels));
        const auto format = info.Format == RasterFormat::R8Unorm    ? GL_RED
                            : info.Format == RasterFormat::Rg8Unorm ? GL_RG
                                                                    : GL_RGBA;
        const auto type = info.Format == RasterFormat::Rgba16Float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
        const auto* bytes = info.Attachment || info.DeferredInitialization ? nullptr : upload.Bytes.data() + mip.Offset;
        if (info.Dimension == TextureDimension::D3 || info.Dimension == TextureDimension::D2Array)
        {
            glTexImage3D(target,
                         static_cast<GLint>(level),
                         static_cast<GLint>(RasterGlTextureFormat(info.Format)),
                         static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height),
                         static_cast<GLsizei>(internal::TextureSlices(info, level)),
                         0,
                         format,
                         type,
                         bytes);
        }
        else
        {
            for (uint32 slice = 0; slice < info.DepthOrLayers; ++slice)
            {
                const auto face =
                    info.Dimension == TextureDimension::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : target;
                const auto* source = bytes == nullptr ? nullptr : bytes + slice * mip.RowPitch * height;
                glTexImage2D(face,
                             static_cast<GLint>(level),
                             static_cast<GLint>(RasterGlTextureFormat(info.Format)),
                             static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height),
                             0,
                             format,
                             type,
                             source);
            }
        }
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(info.MipLevels - 1));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(target, 0);
    if (info.Attachment)
    {
        if (info.SampleCount == 1)
        {
            glGenTextures(1, &gRasterRenderTextures[slot]);
            glBindTexture(GL_TEXTURE_2D, gRasterRenderTextures[slot]);
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           info.Format == RasterFormat::Rgba16Float  ? GL_RGBA16F
                           : info.Format == RasterFormat::Rgba8Unorm ? GL_RGBA8
                                                                     : GL_SRGB8_ALPHA8,
                           static_cast<GLsizei>(info.Width),
                           static_cast<GLsizei>(info.Height));
        }
        glGenRenderbuffers(1, &gRasterDepths[slot]);
        glBindRenderbuffer(GL_RENDERBUFFER, gRasterDepths[slot]);
        if (info.SampleCount == 4)
        {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,
                                             4,
                                             info.Stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                                             static_cast<GLsizei>(info.Width),
                                             static_cast<GLsizei>(info.Height));
        }
        else
        {
            glRenderbufferStorage(GL_RENDERBUFFER,
                                  info.Stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                                  static_cast<GLsizei>(info.Width),
                                  static_cast<GLsizei>(info.Height));
        }
        glGenFramebuffers(1, &gRasterFramebuffers[slot]);
        glBindFramebuffer(GL_FRAMEBUFFER, gRasterFramebuffers[slot]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gRasterRenderTextures[slot], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  info.Stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER,
                                  gRasterDepths[slot]);
        if (info.SampleCount == 4)
        {
            glGenRenderbuffers(1, &gRasterMsaaColors[slot]);
            glBindRenderbuffer(GL_RENDERBUFFER, gRasterMsaaColors[slot]);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,
                                             4,
                                             RasterGlTextureFormat(info.Format),
                                             static_cast<GLsizei>(info.Width),
                                             static_cast<GLsizei>(info.Height));
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gRasterMsaaColors[slot]);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            return RasterStatus::Unsupported;
        }
        glGenFramebuffers(1, &gRasterSampleFramebuffers[slot]);
        glBindFramebuffer(GL_FRAMEBUFFER, gRasterSampleFramebuffers[slot]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gRasterTextures[slot], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            return RasterStatus::Unsupported;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return RasterGlResult();
}
// WebGL 2 has no texture-view API. Immutable sampled subranges are copied using
// framebuffer reads; source retention prevents any mutable transfer until retirement.
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32) noexcept
{
    const auto& info = gRasterTextureDescriptions[source.Texture];
    const auto& range = source.View;
    if (range.Attachment)
    {
        gRasterViews[slot].Object = gRasterRenderTextures[source.Texture];
        return RasterStatus::Ready;
    }
    auto& view = gRasterViews[slot];
    view.Target = RasterGlTextureTarget(range.Dimension);
    const uint32 layers = info.Dimension == TextureDimension::D3 ? 1U : info.DepthOrLayers;
    if (range.BaseMip == 0 && range.MipCount == info.MipLevels && range.BaseLayer == 0 && range.LayerCount == layers &&
        range.Dimension == info.Dimension)
    {
        view.Object = gRasterTextures[source.Texture];
        return RasterStatus::Ready;
    }
    view.Owned = true;
    glGenTextures(1, &view.Object);
    glBindTexture(view.Target, view.Object);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    RasterStatus result = RasterStatus::Ready;
    for (uint32 level = 0; level < range.MipCount && result == RasterStatus::Ready; ++level)
    {
        const uint32 mip = range.BaseMip + level;
        const auto width = internal::TextureMipExtent(info.Width, mip);
        const auto height = internal::TextureMipExtent(info.Height, mip);
        const auto slices =
            info.Dimension == TextureDimension::D3 ? internal::TextureSlices(info, mip) : range.LayerCount;
        const auto format = info.Format == RasterFormat::R8Unorm    ? GL_RED
                            : info.Format == RasterFormat::Rg8Unorm ? GL_RG
                                                                    : GL_RGBA;
        const auto type = info.Format == RasterFormat::Rgba16Float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
        if (range.Dimension == TextureDimension::D3 || range.Dimension == TextureDimension::D2Array)
        {
            glTexImage3D(view.Target,
                         static_cast<GLint>(level),
                         static_cast<GLint>(RasterGlTextureFormat(info.Format)),
                         static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height),
                         static_cast<GLsizei>(slices),
                         0,
                         format,
                         type,
                         nullptr);
        }
        else
        {
            for (uint32 slice = 0; slice < slices; ++slice)
            {
                const auto target =
                    range.Dimension == TextureDimension::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : view.Target;
                glTexImage2D(target,
                             static_cast<GLint>(level),
                             static_cast<GLint>(RasterGlTextureFormat(info.Format)),
                             static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height),
                             0,
                             format,
                             type,
                             nullptr);
            }
        }
        for (uint32 slice = 0; slice < slices; ++slice)
        {
            if (info.Dimension == TextureDimension::D3 || info.Dimension == TextureDimension::D2Array)
            {
                glFramebufferTextureLayer(GL_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          gRasterTextures[source.Texture],
                                          static_cast<GLint>(mip),
                                          static_cast<GLint>(range.BaseLayer + slice));
            }
            else
            {
                const auto target = info.Dimension == TextureDimension::Cube
                                        ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + range.BaseLayer + slice
                                        : GL_TEXTURE_2D;
                glFramebufferTexture2D(GL_FRAMEBUFFER,
                                       GL_COLOR_ATTACHMENT0,
                                       target,
                                       gRasterTextures[source.Texture],
                                       static_cast<GLint>(mip));
            }
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                result = RasterStatus::Unsupported;
                break;
            }
            if (range.Dimension == TextureDimension::D3 || range.Dimension == TextureDimension::D2Array)
            {
                glCopyTexSubImage3D(view.Target,
                                    static_cast<GLint>(level),
                                    0,
                                    0,
                                    static_cast<GLint>(slice),
                                    0,
                                    0,
                                    static_cast<GLsizei>(width),
                                    static_cast<GLsizei>(height));
            }
            else
            {
                const auto target =
                    range.Dimension == TextureDimension::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : view.Target;
                glCopyTexSubImage2D(target,
                                    static_cast<GLint>(level),
                                    0,
                                    0,
                                    0,
                                    0,
                                    static_cast<GLsizei>(width),
                                    static_cast<GLsizei>(height));
            }
        }
    }
    glTexParameteri(view.Target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(view.Target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(range.MipCount - 1));
    glBindTexture(view.Target, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    const auto error = RasterGlResult();
    return error != RasterStatus::Ready ? error : result;
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32) noexcept
{
    glGenSamplers(1, &gRasterSamplers[slot]);
    const GLuint sampler = gRasterSamplers[slot];
    const auto filter = info.Filter == RasterFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    const auto minFilter =
        info.MipFilter == RasterMipFilter::None ? filter
        : info.MipFilter == RasterMipFilter::Nearest
            ? (info.Filter == RasterFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST)
            : (info.Filter == RasterFilter::Nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
    glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, info.MinLod);
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, info.MipFilter == RasterMipFilter::None ? 0 : info.MaxLod);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, info.U == RasterAddress::Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, info.V == RasterAddress::Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, info.W == RasterAddress::Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    return RasterGlResult();
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& info,
                                const internal::RasterShaderInfo& metadata,
                                uint32) noexcept
{
    auto& shader = gRasterShaders[slot];
    shader.Info = metadata;
    shader.Object = Compile(info.Stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, info.GlslEs);
    return shader.Object != 0 ? RasterGlResult() : RasterStatus::Failed;
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32) noexcept
{
    gRasterLayouts[slot] = info;
    return RasterStatus::Ready;
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout&, const internal::RasterSet& info, uint32) noexcept
{
    return gRasterSets[slot].TryCopyFrom(info) == true ? RasterStatus::Ready : internal::StorageFailure();
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32) noexcept
{
    auto& pipeline = gRasterPipelines[slot];
    pipeline.Info = info;
    pipeline.Object = glCreateProgram();
    if (pipeline.Object == 0)
    {
        return RasterStatus::OutOfMemory;
    }
    glAttachShader(pipeline.Object, gRasterShaders[info.Vertex].Object);
    glAttachShader(pipeline.Object, gRasterShaders[info.Fragment].Object);
    glLinkProgram(pipeline.Object);
    GLint linked = 0;
    glGetProgramiv(pipeline.Object, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE || RasterGlProgram(pipeline.Object, pipeline) == false)
    {
        char log[512]{};
        GLsizei length = 0;
        glGetProgramInfoLog(pipeline.Object, sizeof(log), &length, log);
        LUDUS_LOG_WARN(LOG_RHI,
                       "Raster GLSL ES link/interface failure: {}",
                       std::string_view(log, static_cast<usize>(length)));
        return RasterStatus::Failed;
    }
    glGenVertexArrays(1, &pipeline.Array);
    return RasterGlResult();
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            glDeleteBuffers(1, &gRasterBuffers[slot]);
            gRasterBuffers[slot] = 0;
            break;
        case internal::RasterKind::Texture:
            glDeleteFramebuffers(1, &gRasterSampleFramebuffers[slot]);
            glDeleteTextures(1, &gRasterRenderTextures[slot]);
            gRasterSampleFramebuffers[slot] = gRasterRenderTextures[slot] = 0;
            glDeleteFramebuffers(1, &gRasterFramebuffers[slot]);
            glDeleteRenderbuffers(1, &gRasterDepths[slot]);
            glDeleteRenderbuffers(1, &gRasterMsaaColors[slot]);
            gRasterMsaaColors[slot] = 0;
            gRasterFramebuffers[slot] = gRasterDepths[slot] = 0;
            glDeleteTextures(1, &gRasterTextures[slot]);
            gRasterTextures[slot] = 0;
            break;
        case internal::RasterKind::View:
            if (gRasterViews[slot].Owned)
            {
                glDeleteTextures(1, &gRasterViews[slot].Object);
            }
            gRasterViews[slot] = {};
            break;
        case internal::RasterKind::Sampler:
            glDeleteSamplers(1, &gRasterSamplers[slot]);
            gRasterSamplers[slot] = 0;
            break;
        case internal::RasterKind::Shader:
            glDeleteShader(gRasterShaders[slot].Object);
            gRasterShaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            gRasterLayouts[slot] = {};
            break;
        case internal::RasterKind::Set:
            gRasterSets[slot] = {};
            break;
        case internal::RasterKind::Pipeline:
            glDeleteProgram(gRasterPipelines[slot].Object);
            glDeleteVertexArrays(1, &gRasterPipelines[slot].Array);
            gRasterPipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
        case internal::RasterKind::RayGeometry:
        case internal::RasterKind::RayScene:
        case internal::RasterKind::RayProgram:
        case internal::RasterKind::RayBatch:
        case internal::RasterKind::SpatialUpscaler:
        case internal::RasterKind::Count:
            break;
    }
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    const auto& pipeline = gRasterPipelines[packet.Pipeline];
    const auto& info = pipeline.Info;
    const auto& set = gRasterSets[packet.Set];
    const auto& layout = gRasterLayouts[set.Layout];
    glUseProgram(pipeline.Object);
    glBindVertexArray(pipeline.Array);
    for (usize i = 0; i < info.AttributeCount; ++i)
    {
        const auto& attribute = info.Attributes[i];
        const auto& stream = info.Streams[attribute.Stream];
        glBindBuffer(GL_ARRAY_BUFFER, gRasterBuffers[packet.Vertices[attribute.Stream]]);
        const GLint components = attribute.Format == RasterVertexFormat::Float2   ? 2
                                 : attribute.Format == RasterVertexFormat::Float3 ? 3
                                                                                  : 4;
        // GL interprets this pointer as a byte offset into the bound buffer.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const void* offset = reinterpret_cast<const void*>(packet.Offsets[attribute.Stream] + attribute.Offset);
        glVertexAttribPointer(attribute.Location,
                              components,
                              GL_FLOAT,
                              GL_FALSE,
                              static_cast<GLsizei>(stream.Stride),
                              offset);
        glEnableVertexAttribArray(attribute.Location);
        glVertexAttribDivisor(attribute.Location, stream.PerInstance ? 1 : 0);
    }
    for (usize i = 0; i < layout.Count; ++i)
    {
        const auto& entry = layout.Entries[i];
        if (entry.Kind == RasterBindingKind::UniformBuffer)
        {
            glBindBufferRange(GL_UNIFORM_BUFFER,
                              entry.Binding,
                              gRasterBuffers[set.Slots[i]],
                              static_cast<GLintptr>(set.Offsets[i] + packet.BindingOffsets.Bytes[entry.Binding]),
                              static_cast<GLsizeiptr>(set.Sizes[i]));
        }
        else if (internal::TextureBinding(entry.Kind))
        {
            const uint32 sampler = pipeline.Samplers[entry.Binding];
            GLuint object = 0;
            for (usize j = 0; j < layout.Count; ++j)
            {
                if (layout.Entries[j].Kind == RasterBindingKind::Sampler && layout.Entries[j].Binding == sampler)
                {
                    object = gRasterSamplers[set.Slots[j]];
                }
            }
            glActiveTexture(GL_TEXTURE0 + entry.Binding);
            const auto& view = gRasterViews[set.Slots[i]];
            glBindTexture(view.Target, view.Object);
            glBindSampler(entry.Binding, object);
            glUniform1i(pipeline.Textures[entry.Binding], static_cast<GLint>(entry.Binding));
        }
    }
    if (info.Depth)
    {
        glEnable(GL_DEPTH_TEST);
    }
    else
    {
        glDisable(GL_DEPTH_TEST);
    }
    const GLenum comparisons[]{GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL, GL_ALWAYS};
    internal::RasterArea area;
    const auto& pass = gRasterGlPass;
    const bool isSurface = gRasterGlPassOpen == false || pass.Texture == internal::RASTER_INVALID;
    const uint32 width = isSurface ? gTarget.Width : gRasterTextureDescriptions[pass.Texture].Width;
    const uint32 height = isSurface ? gTarget.Height : gRasterTextureDescriptions[pass.Texture].Height;
    if (internal::RasterResolveArea(gRasterGlPassOpen ? pass.Description : RasterPassDescription{},
                                    width,
                                    height,
                                    area) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (area.Empty)
    {
        return RasterStatus::Ready;
    }
    const auto& v = area.Viewport;
    const auto& c = area.Scissor;
    glViewport(static_cast<GLint>(v.X),
               static_cast<GLint>(height - v.Y - v.Height),
               static_cast<GLsizei>(v.Width),
               static_cast<GLsizei>(v.Height));
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(c.X),
              static_cast<GLint>(height - c.Y - c.Height),
              static_cast<GLsizei>(c.Width),
              static_cast<GLsizei>(c.Height));
    glDepthFunc(comparisons[static_cast<usize>(info.DepthCompare)]);
    glDepthMask(info.Depth && info.DepthWrite ? GL_TRUE : GL_FALSE);
    if (info.Blend)
    {
        glEnable(GL_BLEND);
    }
    else
    {
        glDisable(GL_BLEND);
    }
    if (info.Stencil)
    {
        glEnable(GL_STENCIL_TEST);
        const GLenum
            stencilComparisons[]{GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        const GLenum ops[]{GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
        const RasterStencilFace faces[]{info.StencilFront, info.StencilBack};
        for (usize i = 0; i < 2; ++i)
        {
            const GLenum face = i == 0 ? GL_FRONT : GL_BACK;
            const auto& input = faces[i];
            glStencilFuncSeparate(face,
                                  stencilComparisons[static_cast<usize>(input.Compare)],
                                  static_cast<GLint>(info.StencilReference),
                                  info.StencilReadMask);
            glStencilOpSeparate(face,
                                ops[static_cast<usize>(input.Fail)],
                                ops[static_cast<usize>(input.DepthFail)],
                                ops[static_cast<usize>(input.Pass)]);
            glStencilMaskSeparate(face, info.StencilWriteMask);
        }
    }
    else
    {
        glDisable(GL_STENCIL_TEST);
    }
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gRasterBuffers[packet.Indices]);
    // GL interprets this pointer-shaped argument as an element-buffer byte offset.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const void* indexOffset = reinterpret_cast<const void*>(packet.IndexOffset);
    glDrawElementsInstanced(GL_TRIANGLES,
                            static_cast<GLsizei>(packet.IndexCount),
                            packet.Index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                            indexOffset,
                            static_cast<GLsizei>(packet.InstanceCount));
    glBindVertexArray(0);
    return RasterGlResult();
}
uint64 RasterCompleted() noexcept
{
    for (auto& completion : gRasterCompletions)
    {
        if (completion.Sync == nullptr)
        {
            continue;
        }
        const auto status = glClientWaitSync(completion.Sync, 0, 0);
        if (status == GL_WAIT_FAILED)
        {
            internal::Fail(gSession,
                           emscripten_is_webgl_context_lost(gContext) ? StartupError::DeviceLost
                                                                      : StartupError::RenderingUnavailable);
            return gRasterCompleted;
        }
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
        {
            if (completion.Ordinal > gRasterCompleted)
            {
                gRasterCompleted = completion.Ordinal;
            }
            glDeleteSync(completion.Sync);
            completion = {};
        }
    }
    return gRasterCompleted;
}
void RasterEndPass() noexcept
{
    glDisable(GL_SCISSOR_TEST);
    if (gRasterGlPassOpen == false)
    {
        return;
    }
    const auto& pass = gRasterGlPass;
    if (pass.ColorCount != 0)
    {
        const auto width = static_cast<GLint>(pass.Width), height = static_cast<GLint>(pass.Height);
        for (usize i = 0; i < pass.ColorCount; ++i)
        {
            const auto& c = pass.Colors[i];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, gRasterMrtFramebuffer);
            glReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
            usize destination = internal::RASTER_INVALID;
            if (c.ResolveTexture != internal::RASTER_INVALID)
            {
                destination = c.ResolveTexture;
                // First resolve without flipping, then normalize orientation in a single-sample blit.
                const auto resolve = gRasterResolveFramebuffer;
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                       GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D,
                                       gRasterRenderTextures[destination],
                                       0);
                glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gRasterSampleFramebuffers[destination]);
                glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            if (c.Store == RasterStore::Store && pass.SampleCount == 1)
            {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, gRasterMrtFramebuffer);
                glReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gRasterSampleFramebuffers[c.Texture]);
                glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, gRasterMrtFramebuffer);
        gRasterGlPassOpen = false;
        return;
    }
    if (pass.Texture != internal::RASTER_INVALID && pass.Description.ColorStore == RasterStore::Store)
    {
        // GL framebuffer rows run bottom-up; uploaded/sampled texture rows in
        // the portable API run top-down. A private flipped blit preserves the
        // same UV convention as Metal, Vulkan and WebGPU, including Load passes.
        const auto& texture = gRasterTextureDescriptions[pass.Texture];
        const auto width = static_cast<GLint>(texture.Width), height = static_cast<GLint>(texture.Height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gRasterFramebuffers[pass.Texture]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gRasterSampleFramebuffers[pass.Texture]);
        glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, gRasterFramebuffers[pass.Texture]);
    }
    GLenum discard[2]{};
    GLsizei count = 0;
    const bool surface = pass.Texture == internal::RASTER_INVALID;
    if (pass.Description.ColorStore == RasterStore::Discard)
    {
        discard[count++] = surface ? GL_COLOR : GL_COLOR_ATTACHMENT0;
    }
    if (pass.Description.DepthStore == RasterStore::Discard)
    {
        discard[count++] = surface ? GL_DEPTH : GL_DEPTH_ATTACHMENT;
    }
    if (count != 0)
    {
        glInvalidateFramebuffer(GL_FRAMEBUFFER, count, discard);
    }
    gRasterGlPassOpen = false;
}
void RasterTextureBarrier(usize, RasterTextureUse) noexcept {}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    if (info.Texture == internal::RASTER_INVALID || gRasterFramebuffers[info.Texture] != 0)
    {
        return RasterStatus::Ready;
    }
    return RasterStatus::InvalidState;
}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    gRasterGlPass = info;
    gRasterGlPassOpen = true;
    const auto& state = info.Description;
    const bool surface = info.Texture == internal::RASTER_INVALID;
    glBindFramebuffer(GL_FRAMEBUFFER, surface ? 0 : gRasterFramebuffers[info.Texture]);
    if (info.ColorCount != 0)
    {
        if (gRasterMrtFramebuffer == 0)
        {
            glGenFramebuffers(1, &gRasterMrtFramebuffer);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, gRasterMrtFramebuffer);
        GLenum outputs[4]{};
        for (usize i = 0; i < 4; ++i)
        {
            const GLenum attachment = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, 0);
            if (i < info.ColorCount)
            {
                const auto slot = info.Colors[i].Texture;
                outputs[i] = attachment;
                if (info.SampleCount == 4)
                {
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, gRasterMsaaColors[slot]);
                }
                else
                {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, gRasterRenderTextures[slot], 0);
                }
            }
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  info.Stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER,
                                  gRasterDepths[info.Texture]);
        glDrawBuffers(static_cast<GLsizei>(info.ColorCount), outputs);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            return RasterStatus::Failed;
        }
    }
    const auto width = surface ? gTarget.Width : info.Width;
    const auto height = surface ? gTarget.Height : info.Height;
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(state.DepthReadOnly ? GL_FALSE : GL_TRUE);
    if (info.ColorCount != 0)
    {
        for (usize i = 0; i < info.ColorCount; ++i)
        {
            if (info.Colors[i].Load != RasterLoad::Load)
            {
                glClearBufferfv(GL_COLOR, static_cast<GLint>(i), info.Colors[i].Clear);
            }
        }
    }
    glClearColor(state.Clear[0], state.Clear[1], state.Clear[2], state.Clear[3]);
    glClearDepthf(state.ClearDepth);
    GLbitfield clear = 0;
    if (info.ColorCount == 0 && state.ColorLoad != RasterLoad::Load)
    {
        clear |= GL_COLOR_BUFFER_BIT;
    }
    if (state.DepthLoad != RasterLoad::Load)
    {
        clear |= GL_DEPTH_BUFFER_BIT;
    }
    glStencilMask(255);
    glClearStencil(static_cast<GLint>(state.ClearStencil));
    if (info.Stencil && state.StencilReadOnly == false && state.StencilLoad != RasterLoad::Load)
    {
        clear |= GL_STENCIL_BUFFER_BIT;
    }
    if (clear != 0)
    {
        glClear(clear);
    }
    return RasterGlResult();
}
RasterStatus RasterReserveSubmission() noexcept
{
    (void)RasterCompleted();
    if (gRasterReserved != nullptr)
    {
        return RasterStatus::Ready;
    }
    for (usize i = 0; i < internal::CurrentStorageLimits().Submissions; ++i)
    {
        auto& completion = gRasterCompletions[i];
        if (completion.Sync == nullptr && completion.Reserved == false)
        {
            completion.Reserved = true;
            gRasterReserved = &completion;
            return RasterStatus::Ready;
        }
    }
    return RasterStatus::CapacityExceeded;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    if (gRasterReserved == nullptr)
    {
        return;
    }
    auto& completion = *gRasterReserved;
    gRasterReserved = nullptr;
    completion.Reserved = false;
    completion.Ordinal = ordinal;
    completion.Sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (completion.Sync == nullptr)
    {
        internal::Fail(gSession, StartupError::RenderingUnavailable);
    }
}
void RasterReset() noexcept
{
    glDeleteFramebuffers(1, &gRasterMrtFramebuffer);
    gRasterMrtFramebuffer = 0;
    glDeleteFramebuffers(1, &gRasterResolveFramebuffer);
    gRasterResolveFramebuffer = 0;
    gRasterGlPassOpen = false;
    gRasterGlPass = {};
    gRasterHdrProbed = gRasterHdr = false;
    if (gContext > 0 && emscripten_is_webgl_context_lost(gContext) == false)
    {
        glFinish();
    }
    for (auto& completion : gRasterCompletions)
    {
        if (completion.Sync != nullptr)
        {
            glDeleteSync(completion.Sync);
        }
        completion = {};
    }
    gRasterReserved = nullptr;
    gRasterCompleted = 0;
}
void RasterShutdown() noexcept {}

RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            return gRasterBuffers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Texture:
            return gRasterTextures.EnsureSlots(count) && gRasterTextureDescriptions.EnsureSlots(count) &&
                           gRasterFramebuffers.EnsureSlots(count) && gRasterRenderTextures.EnsureSlots(count) &&
                           gRasterSampleFramebuffers.EnsureSlots(count) && gRasterDepths.EnsureSlots(count) &&
                           gRasterMsaaColors.EnsureSlots(count)
                       ? RasterStatus::Ready
                       : internal::StorageFailure();
        case internal::RasterKind::View:
            return gRasterViews.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Sampler:
            return gRasterSamplers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Shader:
            return gRasterShaders.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Layout:
            return gRasterLayouts.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Set:
            return gRasterSets.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Pipeline:
            return gRasterPipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::ComputePipeline:
            return RasterStatus::Unsupported;
        default:
            return RasterStatus::InvalidDescription;
    }
}
RasterStatus RasterPrepareStorage() noexcept
{
    // Frame storage admission precedes Begin(), so preserve synchronous loss
    // before querying GL errors or allocating the R10 framebuffer objects.
    if (gContext <= 0 || emscripten_is_webgl_context_lost(gContext))
    {
        internal::Fail(gSession, StartupError::DeviceLost);
        return RasterStatus::DeviceLost;
    }
    if (gRasterCompletions.EnsureSlots(internal::CurrentStorageLimits().Submissions) == false)
    {
        return internal::StorageFailure();
    }
    if (gRasterMrtFramebuffer == 0)
    {
        glGenFramebuffers(1, &gRasterMrtFramebuffer);
    }
    if (gRasterResolveFramebuffer == 0)
    {
        glGenFramebuffers(1, &gRasterResolveFramebuffer);
    }
    return RasterGlResult();
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
