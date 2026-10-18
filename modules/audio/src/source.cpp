#include <ludus/audio/audio_source.h>

#include <ludus/foundation/math/scalar.hpp>

#include <cstring>

#include "internal/input.hpp"

namespace ludus::audio
{
namespace
{
Status Analyze(ma_decoder* decoder, SourceInfo& output, std::span<float32> peaks) noexcept
{
    SourceInfo next;
    next.Channels = decoder->outputChannels;
    next.SampleRate = decoder->outputSampleRate;
    ma_uint64 length = 0;
    auto status = Status::Ok;
    if (next.Channels < 1 || next.Channels > 2 || ma_decoder_get_length_in_pcm_frames(decoder, &length) != MA_SUCCESS ||
        length == 0)
    {
        status = Status::Unsupported;
    }
    next.Frames = length;
    if (status == Status::Ok && peaks.empty() == false)
    {
        for (auto& value : peaks)
        {
            value = 0;
        }
        float32 scratch[4096 * 2];
        uint64 cursor = 0;
        const uint64 bucket = length / peaks.size() + (length % peaks.size() != 0 ? 1 : 0);
        while (cursor < length)
        {
            ma_uint64 got = 0;
            const auto result = ma_decoder_read_pcm_frames(decoder, scratch, 4096, &got);
            if ((result != MA_SUCCESS && result != MA_AT_END) || got == 0)
            {
                status = Status::DecodeError;
                break;
            }
            for (uint64 f = 0; f < got; ++f)
            {
                float32 peak = 0;
                for (uint32 c = 0; c < next.Channels; ++c)
                {
                    auto value = scratch[f * next.Channels + c];
                    if (ludus::foundation::math::IsFinite(value) == false)
                    {
                        status = Status::DecodeError;
                        break;
                    }
                    if (value < 0)
                    {
                        value = -value;
                    }
                    if (value > peak)
                    {
                        peak = value;
                    }
                }
                const auto index = static_cast<usize>((cursor + f) / bucket);
                if (index < peaks.size() && peak > peaks[index])
                {
                    peaks[index] = peak;
                }
                if (peak > next.Peak)
                {
                    next.Peak = peak;
                }
            }
            cursor += got;
        }
    }
    if (status == Status::Ok)
    {
        output = next;
    }
    return status;
}
} // namespace
Status InspectSource(std::span<const uint8> encoded,
                     SourceFormat format,
                     SourceInfo& output,
                     std::span<float32> peaks) noexcept
{
    if (format != SourceFormat::Wav && format != SourceFormat::Flac)
    {
        return Status::Unsupported;
    }
    if (encoded.empty() || peaks.size() > 4096)
    {
        return Status::InvalidArgument;
    }
    const bool wav = encoded.size() >= 12 && std::memcmp(encoded.data(), "RIFF", 4) == 0 &&
                     std::memcmp(encoded.data() + 8, "WAVE", 4) == 0;
    const bool flac = encoded.size() >= 4 && std::memcmp(encoded.data(), "fLaC", 4) == 0;
    if ((format == SourceFormat::Wav && wav == false) || (format == SourceFormat::Flac && flac == false))
    {
        return Status::DecodeError;
    }
    ma_decoder decoder{};
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder) != MA_SUCCESS)
    {
        return Status::DecodeError;
    }
    const auto status = Analyze(&decoder, output, peaks);
    ma_decoder_uninit(&decoder);
    return status;
}
Status InspectSource(StreamInput& input, SourceFormat format, SourceInfo& output, std::span<float32> peaks) noexcept
{
    if (format != SourceFormat::Wav && format != SourceFormat::Flac)
    {
        return Status::Unsupported;
    }
    if (peaks.size() > 4096 || input.Seek(0, false) != Status::Ok)
    {
        return Status::InvalidArgument;
    }
    ma_decoder decoder{};
    const auto config = internal::InputConfig(format, 0, 0);
    if (ma_decoder_init(internal::ReadInput, internal::SeekInput, &input, &config, &decoder) != MA_SUCCESS)
    {
        return Status::DecodeError;
    }
    const auto status = Analyze(&decoder, output, peaks);
    ma_decoder_uninit(&decoder);
    return status;
}
} // namespace ludus::audio
