#pragma once
// Thanks to Khronos, "WebGL 2.0 Specification", Buffer objects, Copying Buffers,
// and getBufferSubData: https://registry.khronos.org/webgl/specs/latest/2.0/
// Index/non-index staging stays type-compatible. A later zero-timeout sync poll
// precedes getBufferSubData; no MapBufferRange, glFinish, or long client wait is
// introduced on the transfer path. The final browser read can still incur IPC.
EM_JS(void, LifetimeGlCopyBytes, (void* output, ludus::foundation::uint32 size), {
    GLctx.getBufferSubData(GLctx.COPY_READ_BUFFER, 0, HEAPU8, output, size);
});
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
{
namespace
{
struct LifetimeGlTransfer final
{
    GLuint Staging = 0;
    GLsync Sync = nullptr;
    bool Index = false;
    bool Busy = false;
};
LifetimeGlTransfer gLifetimeGl[2][internal::LIFETIME_TRANSFERS];
RasterStatus LifetimeGlStage(LifetimeGlTransfer& transfer, bool readback, bool index) noexcept
{
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    glBindVertexArray(0);
    if (transfer.Staging != 0 && transfer.Index != index)
    {
        glDeleteBuffers(1, &transfer.Staging);
        transfer.Staging = 0;
    }
    if (transfer.Staging == 0)
    {
        glGenBuffers(1, &transfer.Staging);
        const auto target = index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        glBindBuffer(target, transfer.Staging);
        glBufferData(target,
                     static_cast<GLsizeiptr>(internal::CurrentStorageLimits().TransferBytes),
                     nullptr,
                     readback ? GL_STREAM_READ : GL_STREAM_DRAW);
        glBindBuffer(target, 0);
        transfer.Index = index;
    }
    return transfer.Staging != 0 ? RasterGlResult() : RasterStatus::OutOfMemory;
}
RasterStatus LifetimeGlFence(LifetimeGlTransfer& transfer) noexcept
{
    const auto status = RasterGlResult();
    if (status != RasterStatus::Ready)
    {
        // Commands may already have changed a destination. A GL error does not
        // prove completion: close this session before any slice/resource reuse.
        internal::Fail(gSession, StartupError::RenderingUnavailable);
        return status;
    }
    transfer.Sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (transfer.Sync == nullptr)
    {
        internal::Fail(gSession, StartupError::RenderingUnavailable);
        return RasterStatus::Failed;
    }
    transfer.Busy = true;
    return RasterStatus::Ready;
}
} // namespace
bool LifetimeTransferAvailable(bool readback, usize slot) noexcept
{
    return gLifetimeGl[readback ? 1 : 0][slot].Busy == false;
}
RasterStatus LifetimeUpload(usize transferSlot,
                            const BufferDescription& info,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{
    const auto bufferSlot = uploadTarget.Buffer;

    auto& transfer = gLifetimeGl[0][transferSlot];
    const bool index = info.Role == BufferRole::Index16 || info.Role == BufferRole::Index32;
    const auto ready = LifetimeGlStage(transfer, false, index);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    const auto target = index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    glGenBuffers(1, &gRasterBuffers[bufferSlot]);
    glBindBuffer(target, gRasterBuffers[bufferSlot]);
    glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, GL_STATIC_DRAW);
    glBindBuffer(target, 0);
    glBindBuffer(GL_COPY_READ_BUFFER, transfer.Staging);
    glBufferSubData(GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(size), bytes);
    glBindBuffer(GL_COPY_WRITE_BUFFER, gRasterBuffers[bufferSlot]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(size));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return LifetimeGlFence(transfer);
}
RasterStatus LifetimeReadback(usize transferSlot, BufferRole role, const internal::LifetimeCopyRange& range) noexcept
{
    const auto bufferSlot = range.Buffer;
    const auto offset = range.Offset;
    const auto size = range.Size;
    auto& transfer = gLifetimeGl[1][transferSlot];
    const auto ready = LifetimeGlStage(transfer, true, role == BufferRole::Index16 || role == BufferRole::Index32);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, gRasterBuffers[bufferSlot]);
    glBindBuffer(GL_COPY_WRITE_BUFFER, transfer.Staging);
    glCopyBufferSubData(GL_COPY_READ_BUFFER,
                        GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(offset),
                        0,
                        static_cast<GLsizeiptr>(size));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return LifetimeGlFence(transfer);
}
// Thanks to Khronos, "WebGL 2.0 Specification", 3.7.6 and 3.7.9,
// https://registry.khronos.org/webgl/specs/latest/2.0/ ; PBO transfers retain fences.
RasterStatus
LifetimeBufferUpdate(usize slot, BufferRole role, const internal::LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    auto& transfer = gLifetimeGl[0][slot];
    const auto ready = LifetimeGlStage(transfer, false, role == BufferRole::Index16 || role == BufferRole::Index32);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, transfer.Staging);
    glBufferSubData(GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(range.Size), bytes);
    glBindBuffer(GL_COPY_WRITE_BUFFER, gRasterBuffers[range.Buffer]);
    glCopyBufferSubData(GL_COPY_READ_BUFFER,
                        GL_COPY_WRITE_BUFFER,
                        0,
                        static_cast<GLintptr>(range.Offset),
                        static_cast<GLsizeiptr>(range.Size));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return LifetimeGlFence(transfer);
}
RasterStatus LifetimeTextureTransfer(usize slot, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    const bool readback = info.Operation == internal::TextureTransferOperation::Readback;
    auto& transfer = gLifetimeGl[readback ? 1 : 0][slot];
    const auto ready = LifetimeGlStage(transfer, readback, false);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    const auto& source = info.SourceRegion;
    const auto& destination = info.DestinationRegion;
    GLuint framebuffer = 0;
    if (info.Operation == internal::TextureTransferOperation::Update)
    {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, transfer.Staging);
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(info.Size), bytes);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(info.RowPitch / 4));
        glBindTexture(GL_TEXTURE_2D, gRasterTextures[info.Destination]);
        glTexSubImage2D(GL_TEXTURE_2D,
                        static_cast<GLint>(destination.Mip),
                        static_cast<GLint>(destination.X),
                        static_cast<GLint>(destination.Y),
                        static_cast<GLsizei>(destination.Width),
                        static_cast<GLsizei>(destination.Height),
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    else
    {
        // Sampled backing already contains top-oriented logical rows. Reading its
        // physical GL row zero directly preserves that orientation (no second flip).
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               gRasterTextures[info.Source],
                               static_cast<GLint>(source.Mip));
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            return RasterStatus::Failed;
        }
        if (readback)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, transfer.Staging);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(info.RowPitch / 4));
            glReadPixels(static_cast<GLint>(source.X),
                         static_cast<GLint>(source.Y),
                         static_cast<GLsizei>(source.Width),
                         static_cast<GLsizei>(source.Height),
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         nullptr);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        else
        {
            glBindTexture(GL_TEXTURE_2D, gRasterTextures[info.Destination]);
            glCopyTexSubImage2D(GL_TEXTURE_2D,
                                static_cast<GLint>(destination.Mip),
                                static_cast<GLint>(destination.X),
                                static_cast<GLint>(destination.Y),
                                static_cast<GLint>(source.X),
                                static_cast<GLint>(source.Y),
                                static_cast<GLsizei>(source.Width),
                                static_cast<GLsizei>(source.Height));
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (readback == false && gRasterTextureDescriptions[info.Destination].Attachment)
    {
        const auto& description = gRasterTextureDescriptions[info.Destination];
        const auto width = static_cast<GLint>(description.Width), height = static_cast<GLint>(description.Height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gRasterSampleFramebuffers[info.Destination]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gRasterFramebuffers[info.Destination]);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    return LifetimeGlFence(transfer);
}
RasterStatus LifetimePollTransfer(bool readback, usize slot) noexcept
{
    auto& transfer = gLifetimeGl[readback ? 1 : 0][slot];
    if (transfer.Busy == false || transfer.Sync == nullptr)
    {
        return RasterStatus::Failed;
    }
    const auto status = glClientWaitSync(transfer.Sync, 0, 0);
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
    {
        return RasterStatus::Ready;
    }
    if (status == GL_TIMEOUT_EXPIRED)
    {
        return RasterStatus::Pending;
    }
    internal::Fail(gSession,
                   emscripten_is_webgl_context_lost(gContext) ? StartupError::DeviceLost
                                                              : StartupError::RenderingUnavailable);
    return RasterStatus::Failed;
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* output, usize size) noexcept
{
    const auto ready = LifetimePollTransfer(true, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, gLifetimeGl[1][slot].Staging);
    LifetimeGlCopyBytes(output, static_cast<uint32>(size));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    return RasterGlResult();
}
void LifetimeReleaseTransfer(bool readback, usize slot) noexcept
{
    auto& transfer = gLifetimeGl[readback ? 1 : 0][slot];
    if (transfer.Sync != nullptr)
    {
        glDeleteSync(transfer.Sync);
    }
    transfer.Sync = nullptr;
    transfer.Busy = false;
}
void LifetimeReset() noexcept
{
    // RasterReset already established idle/loss for context-owned teardown.
    for (auto& direction : gLifetimeGl)
    {
        for (auto& transfer : direction)
        {
            if (transfer.Sync != nullptr)
            {
                glDeleteSync(transfer.Sync);
            }
            if (transfer.Staging != 0)
            {
                glDeleteBuffers(1, &transfer.Staging);
            }
            transfer = {};
        }
    }
}
void RasterDiscardSubmission() noexcept
{
    if (gRasterReserved != nullptr)
    {
        *gRasterReserved = {};
        gRasterReserved = nullptr;
    }
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGL_NAMESPACE
