#include "internal/prepared.hpp"
#include "internal/frame_conversion.hpp"
#include <new>
namespace ludus::audio
{
PreparedClip::Impl::~Impl() noexcept
{
    ::operator delete[](Decoded.Pcm, std::nothrow);
}
PreparedClip::~PreparedClip() noexcept
{
    delete mImpl;
}
uint64 PreparedClip::Bytes() const noexcept
{
    return mImpl != nullptr ? mImpl->Decoded.Frames * mImpl->Decoded.Channels * sizeof(float32) : 0;
}
Status PreparedClip::Decode(std::span<const uint8> encoded, const ClipDescriptor& descriptor, uint32 rate) noexcept
{
    if (encoded.empty() || rate == 0 || (descriptor.SourceLoopEnd == 0 && descriptor.SourceLoopBegin != 0) ||
        (descriptor.SourceLoopEnd != 0 && descriptor.SourceLoopBegin >= descriptor.SourceLoopEnd))
    {
        return Status::InvalidArgument;
    }
    auto* next = new (std::nothrow) Impl();
    if (next == nullptr)
    {
        return Status::OutOfMemory;
    }
    next->Decoded = internal::DecodeResidentClip(encoded, descriptor.Format, rate);
    next->Rate = rate;
    next->AssetHash = descriptor.AssetHash;
    auto& decoded = next->Decoded;
    if (decoded.Result != Status::Ok)
    {
        const auto status = decoded.Result;
        delete next;
        return status;
    }
    if (descriptor.SourceLoopEnd != 0)
    {
        if (descriptor.SourceLoopEnd > decoded.SourceFrames ||
            internal::TryResampleFrame(descriptor.SourceLoopBegin, decoded.SourceRate, rate, next->LoopBegin) ==
                false ||
            internal::TryResampleFrame(descriptor.SourceLoopEnd, decoded.SourceRate, rate, next->LoopEnd) == false ||
            next->LoopBegin >= next->LoopEnd || next->LoopEnd > decoded.Frames)
        {
            delete next;
            return Status::InvalidArgument;
        }
    }
    delete mImpl;
    mImpl = next;
    return Status::Ok;
}
} // namespace ludus::audio
