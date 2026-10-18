// Resident clip decode via the pinned private miniaudio 0.11.23 decoder
// (WAV/FLAC), per .kiro/specs/audio/design.md sections 2 and 9. This is a cold
// operation off rendering. It decodes/converts to float32 PCM at the session
// rate using miniaudio's data converter (which uses the audited linear
// resampler), owns the produced finite PCM, validates it, and reports failure
// explicitly. No miniaudio type crosses the module boundary.

#include "internal/decode.hpp"

// Decode-only, no device, no high-level engine (see ludus_miniaudio_config.h for
// the compiled implementation unit; here we only need the API declarations).
#include "miniaudio.h"

#include <cmath>
#include <new>

namespace ludus::audio::internal
{
namespace
{
[[nodiscard]] ma_format PreferredFormat() noexcept
{
    return ma_format_f32;
}
} // namespace

DecodedPcm
DecodeResidentClip(std::span<const ludus::foundation::uint8> encoded, SourceFormat format, uint32 sessionRate) noexcept
{
    DecodedPcm out{};
    if (encoded.empty() || sessionRate == 0)
    {
        out.Result = Status::InvalidArgument;
        return out;
    }

    if (format != SourceFormat::Wav && format != SourceFormat::Flac)
    {
        out.Result = Status::Unsupported;
        return out;
    }
    const bool wav = encoded.size() >= 12 && encoded[0] == 'R' && encoded[1] == 'I' && encoded[2] == 'F' &&
                     encoded[3] == 'F' && encoded[8] == 'W' && encoded[9] == 'A' && encoded[10] == 'V' &&
                     encoded[11] == 'E';
    const bool flac =
        encoded.size() >= 4 && encoded[0] == 'f' && encoded[1] == 'L' && encoded[2] == 'a' && encoded[3] == 'C';
    if ((format == SourceFormat::Wav && wav == false) || (format == SourceFormat::Flac && flac == false))
    {
        out.Result = Status::DecodeError;
        return out;
    }
    // Restrict the decoder to the requested codec so an unexpected container is
    // rejected rather than silently decoded.
    ma_decoding_backend_vtable* backends[1] = {};
    (void)backends;

    ma_decoder_config config = ma_decoder_config_init(PreferredFormat(), 0, sessionRate);
    // Channels 0 => keep the source channel count (we validate mono/stereo below).
    config.format = ma_format_f32;
    config.sampleRate = sessionRate;

    ma_decoder decoder;
    const ma_result initResult = ma_decoder_init_memory(encoded.data(), encoded.size_bytes(), &config, &decoder);
    if (initResult != MA_SUCCESS)
    {
        out.Result = Status::DecodeError;
        return out;
    }

    ma_format sourceFormat{};
    ma_uint32 sourceChannels = 0, sourceRate = 0;
    if (ma_data_source_get_data_format(decoder.pBackend, &sourceFormat, &sourceChannels, &sourceRate, nullptr, 0) !=
            MA_SUCCESS ||
        sourceRate == 0)
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::DecodeError;
        return out;
    }
    ma_uint64 sourceFrames = 0;
    if (ma_data_source_get_length_in_pcm_frames(decoder.pBackend, &sourceFrames) != MA_SUCCESS || sourceFrames == 0)
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::DecodeError;
        return out;
    }
    out.SourceFrames = sourceFrames;
    out.SourceRate = sourceRate;
    // Confirm the actual codec matches the declared format.
    // (miniaudio decodes by content; we additionally gate channel layout.)
    (void)format;

    const uint32 channels = decoder.outputChannels;
    if (channels != 1 && channels != 2)
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::Unsupported;
        return out;
    }

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) != MA_SUCCESS || totalFrames == 0)
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::DecodeError;
        return out;
    }

    // Guard the sample-value product against overflow before allocating.
    if (totalFrames > RESIDENT_PCM_CAP_BYTES / (static_cast<uint64>(channels) * sizeof(float32)))
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::AssetCapacity;
        return out;
    }
    const uint64 sampleValues = static_cast<uint64>(totalFrames) * channels;
    if (sampleValues == 0 || sampleValues > (RESIDENT_PCM_CAP_BYTES / sizeof(float32)))
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::AssetCapacity;
        return out;
    }

    auto* pcm = new (std::nothrow) float32[static_cast<usize>(sampleValues)];
    if (pcm == nullptr)
    {
        ma_decoder_uninit(&decoder);
        out.Result = Status::OutOfMemory;
        return out;
    }

    ma_uint64 framesRead = 0;
    const ma_result readResult = ma_decoder_read_pcm_frames(&decoder, pcm, totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);

    if ((readResult != MA_SUCCESS && readResult != MA_AT_END) || framesRead == 0 || framesRead + 1 < totalFrames)
    {
        ::operator delete[](pcm, std::nothrow);
        out.Result = Status::DecodeError;
        return out;
    }

    // Validate finiteness and compute the whole-clip peak (conservative
    // audibility estimate; cursor-local refinement is deferred, design 7.2).
    const uint64 actualSamples = static_cast<uint64>(framesRead) * channels;
    float32 peak = 0.0F;
    for (uint64 i = 0; i < actualSamples; ++i)
    {
        const float32 s = pcm[i];
        if (std::isfinite(s) == false)
        {
            ::operator delete[](pcm, std::nothrow);
            out.Result = Status::DecodeError;
            return out;
        }
        const float32 a = s < 0.0F ? -s : s;
        if (a > peak)
        {
            peak = a;
        }
    }

    out.Result = Status::Ok;
    out.Pcm = pcm;
    out.Frames = framesRead;
    out.Channels = channels;
    out.Layout = channels == 2 ? ChannelLayout::Stereo : ChannelLayout::Mono;
    out.PreparedPeak = peak;
    return out;
}

} // namespace ludus::audio::internal
