#include "internal/stream.hpp"
#include "internal/frame_conversion.hpp"
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

#include <cstring>
#include <new>

#include "internal/input.hpp"

namespace ludus::audio::internal
{
namespace
{
/// Chunks decoded before a stream starts playing: half of its ring.
constexpr uint32 PRIMED_CHUNK_COUNT = static_cast<uint32>(STREAM_CHUNK_COUNT / 2);
} // namespace
StreamData::~StreamData() noexcept
{
    auto* decoder = static_cast<ma_decoder*>(Decoder);
    if (decoder != nullptr)
    {
        ma_decoder_uninit(decoder);
        delete decoder;
    }
    delete[] Encoded;
    delete Input;
}
Status StreamData::Open(std::span<const uint8> bytes, const StreamDescriptor& descriptor, uint32 rate) noexcept
{
    if (descriptor.Format != SourceFormat::Wav && descriptor.Format != SourceFormat::Flac)
    {
        return Status::Unsupported;
    }
    if (bytes.empty() || bytes.size() > BROWSER_ENCODED_CAP_BYTES)
    {
        return Status::AssetCapacity;
    }
    const bool wav = bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
                     std::memcmp(bytes.data() + 8, "WAVE", 4) == 0;
    const bool flac = bytes.size() >= 4 && std::memcmp(bytes.data(), "fLaC", 4) == 0;
    if ((descriptor.Format == SourceFormat::Wav && wav == false) ||
        (descriptor.Format == SourceFormat::Flac && flac == false))
    {
        return Status::DecodeError;
    }
    Encoded = new (std::nothrow) uint8[bytes.size()];
    auto* decoder = new (std::nothrow) ma_decoder();
    if (Encoded == nullptr || decoder == nullptr)
    {
        delete decoder;
        return Status::OutOfMemory;
    }
    EncodedBytes = bytes.size();
    std::memcpy(Encoded, bytes.data(), bytes.size());
    auto config = ma_decoder_config_init(ma_format_f32, 2, rate);
    if (ma_decoder_init_memory(Encoded, bytes.size(), &config, decoder) != MA_SUCCESS)
    {
        delete decoder;
        return Status::DecodeError;
    }
    Decoder = decoder;
    return Configure(descriptor, rate);
}
Status StreamData::Open(StreamInput* input, const StreamDescriptor& descriptor, uint32 rate) noexcept
{
    Input = input;
    if (descriptor.Format != SourceFormat::Wav && descriptor.Format != SourceFormat::Flac)
    {
        return Status::Unsupported;
    }
    if (input == nullptr || input->Seek(0, false) != Status::Ok)
    {
        return Status::InvalidArgument;
    }
    auto* decoder = new (std::nothrow) ma_decoder();
    if (decoder == nullptr)
    {
        return Status::OutOfMemory;
    }
    const auto config = InputConfig(descriptor.Format, 2, rate);
    if (ma_decoder_init(ReadInput, SeekInput, input, &config, decoder) != MA_SUCCESS)
    {
        delete decoder;
        return Status::DecodeError;
    }
    Decoder = decoder;
    return Configure(descriptor, rate);
}
Status StreamData::Configure(const StreamDescriptor& descriptor, uint32 rate) noexcept
{
    auto* decoder = static_cast<ma_decoder*>(Decoder);
    Rate = rate;
    ma_uint64 length = 0, sourceLength = 0;
    ma_uint32 sourceRate = 0, channels = 0;
    ma_format format{};
    if (ma_decoder_get_length_in_pcm_frames(decoder, &length) != MA_SUCCESS || length == 0 ||
        ma_data_source_get_data_format(decoder->pBackend, &format, &channels, &sourceRate, nullptr, 0) != MA_SUCCESS ||
        ma_data_source_get_length_in_pcm_frames(decoder->pBackend, &sourceLength) != MA_SUCCESS || sourceRate == 0 ||
        channels < 1 || channels > 2)
    {
        return Status::DecodeError;
    }
    if (descriptor.Looping == false && (descriptor.SourceLoopBegin != 0 || descriptor.SourceLoopEnd != 0))
    {
        return Status::InvalidArgument;
    }
    Looping = descriptor.Looping;
    End = length;
    if (descriptor.SourceLoopEnd != 0 || descriptor.SourceLoopBegin != 0)
    {
        if (descriptor.SourceLoopBegin >= descriptor.SourceLoopEnd || descriptor.SourceLoopEnd > sourceLength)
        {
            return Status::InvalidArgument;
        }
        if (TryResampleFrame(descriptor.SourceLoopBegin, sourceRate, rate, Begin) == false ||
            TryResampleFrame(descriptor.SourceLoopEnd, sourceRate, rate, End) == false || Begin >= End || End > length)
        {
            return Status::InvalidArgument;
        }
    }
    for (uint32 i = 0; i < PRIMED_CHUNK_COUNT && WorkerEof == false; ++i)
    {
        Refill();
    }
    for (uint32 i = 0; i < Written.load(std::memory_order_relaxed); ++i)
    {
        if (Chunks[i].Error)
        {
            return Status::DecodeError;
        }
    }
    return Status::Ok;
}
void StreamData::Refill() noexcept
{
    if (Cancel.load(std::memory_order_acquire))
    {
        Done.store(true, std::memory_order_release);
        return;
    }
    if (WorkerEof)
    {
        return;
    }
    const auto written = Written.load(std::memory_order_relaxed), read = Read.load(std::memory_order_acquire);
    if (written - read >= 6)
    {
        return;
    }
    auto& chunk = Chunks[written & (STREAM_CHUNK_COUNT - 1)];
    chunk.Frames = 0;
    chunk.Eof = false;
    chunk.Error = false;
    auto* decoder = static_cast<ma_decoder*>(Decoder);
    while (chunk.Frames < STREAM_CHUNK_FRAMES)
    {
        if (Cancel.load(std::memory_order_acquire))
        {
            Done.store(true, std::memory_order_release);
            return;
        }
        if (DecodeCursor >= End)
        {
            if (Looping == false)
            {
                chunk.Eof = true;
                WorkerEof = true;
                break;
            }
            if (ma_decoder_seek_to_pcm_frame(decoder, Begin) != MA_SUCCESS)
            {
                chunk.Error = true;
                WorkerEof = true;
                break;
            }
            DecodeCursor = Begin;
        }
        const uint64 left = End - DecodeCursor;
        const uint32 need =
            static_cast<uint32>(left < STREAM_CHUNK_FRAMES - chunk.Frames ? left : STREAM_CHUNK_FRAMES - chunk.Frames);
        ma_uint64 got = 0;
        const auto result = ma_decoder_read_pcm_frames(decoder, chunk.Samples + usize{chunk.Frames} * 2, need, &got);
        chunk.Frames += static_cast<uint32>(got);
        DecodeCursor += got;
        if (result != MA_SUCCESS && result != MA_AT_END)
        {
            chunk.Error = true;
            WorkerEof = true;
            break;
        }
        if (got == 0)
        {
            chunk.Error = true;
            WorkerEof = true;
            break;
        }
    }
    if (Looping == false && DecodeCursor == End)
    {
        chunk.Eof = true;
        WorkerEof = true;
    }
    for (uint32 i = 0; i < chunk.Frames * 2; ++i)
    {
        const auto sample = chunk.Samples[i];
        if (ludus::foundation::math::IsFinite(sample) == false)
        {
            chunk.Error = true;
            WorkerEof = true;
            break;
        }
    }
    Written.store(written + 1, std::memory_order_release);
}
StreamResult StreamData::Render(float32* output, const StreamRequest& request) noexcept
{
    StreamResult result;
    const auto frames = request.Frames;
    const auto gain = request.Gain;
    const bool stop = request.Stop;
    for (uint32 f = 0; f < frames; ++f)
    {
        auto read = Read.load(std::memory_order_relaxed);
        const auto written = Written.load(std::memory_order_acquire);
        const bool available = read != written;
        const float32 target = available && stop == false ? gain : 0.0F;
        const float32 step = 1.0F / (static_cast<float32>(Rate) / 200.0F + 1.0F);
        if (Fade < target)
        {
            Fade = Fade + step < target ? Fade + step : target;
        }
        else
        {
            Fade = Fade - step > target ? Fade - step : target;
        }
        if (available && stop == false)
        {
            auto& chunk = Chunks[read & (STREAM_CHUNK_COUNT - 1)];
            if (Offset < chunk.Frames)
            {
                LastL = chunk.Samples[usize{Offset} * 2];
                LastR = chunk.Samples[usize{Offset} * 2 + 1];
                ++Offset;
                ++result.Frames;
            }
            if (Offset == chunk.Frames)
            {
                const bool eof = chunk.Eof, failed = chunk.Error;
                Offset = 0;
                Read.store(read + 1, std::memory_order_release);
                if (eof || failed)
                {
                    result.Terminal = true;
                    result.Error = failed;
                }
            }
        }
        output[usize{f} * 2] = LastL * Fade;
        output[usize{f} * 2 + 1] = LastR * Fade;
        if (result.Terminal || (stop && Fade == 0.0F))
        {
            result.Terminal = true;
            break;
        }
    }
    return result;
}
} // namespace ludus::audio::internal
