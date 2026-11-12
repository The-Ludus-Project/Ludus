#pragma once

// Minimal in-memory WAV fixture builder for audio tests. Produces a canonical
// 16-bit PCM WAV (mono or stereo) that the pinned miniaudio decoder accepts.
// Test-only; not part of the engine.

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <cmath>
#include <cstring>

namespace ludus::audio::test
{
using ludus::foundation::Array;
using ludus::foundation::uint16;
using ludus::foundation::uint32;
using ludus::foundation::uint8;

inline void PutU32(Array<uint8>& b, uint32 v)
{
    b.Add(static_cast<uint8>(v & 0xFF));
    b.Add(static_cast<uint8>((v >> 8) & 0xFF));
    b.Add(static_cast<uint8>((v >> 16) & 0xFF));
    b.Add(static_cast<uint8>((v >> 24) & 0xFF));
}

inline void PutU16(Array<uint8>& b, uint16 v)
{
    b.Add(static_cast<uint8>(v & 0xFF));
    b.Add(static_cast<uint8>((v >> 8) & 0xFF));
}

// Build a sine-tone 16-bit PCM WAV. channels 1 or 2; frames samples per channel.
// test-only fixture builder; convertible params are intentional here.
inline Array<uint8> MakeSineWav(uint32 sampleRate, // NOLINT(bugprone-easily-swappable-parameters)
                                uint16 channels,
                                uint32 frames,
                                double freqHz = 440.0,
                                double amplitude = 0.5)
{
    const uint16 bitsPerSample = 16;
    const uint16 blockAlign = static_cast<uint16>(channels * (bitsPerSample / 8));
    const uint32 byteRate = sampleRate * blockAlign;
    const uint32 dataBytes = frames * blockAlign;

    Array<uint8> b;
    b.EnsureCapacity(44 + dataBytes);
    // RIFF header
    const uint8 riff[4] = {'R', 'I', 'F', 'F'};
    b.AddRange(foundation::BorrowedView<const uint8>(riff, 4));
    PutU32(b, 36 + dataBytes);
    const uint8 wave[4] = {'W', 'A', 'V', 'E'};
    b.AddRange(foundation::BorrowedView<const uint8>(wave, 4));
    // fmt chunk
    const uint8 fmt[4] = {'f', 'm', 't', ' '};
    b.AddRange(foundation::BorrowedView<const uint8>(fmt, 4));
    PutU32(b, 16);
    PutU16(b, 1); // PCM
    PutU16(b, channels);
    PutU32(b, sampleRate);
    PutU32(b, byteRate);
    PutU16(b, blockAlign);
    PutU16(b, bitsPerSample);
    // data chunk
    const uint8 data[4] = {'d', 'a', 't', 'a'};
    b.AddRange(foundation::BorrowedView<const uint8>(data, 4));
    PutU32(b, dataBytes);
    for (uint32 f = 0; f < frames; ++f)
    {
        const double t = static_cast<double>(f) / static_cast<double>(sampleRate);
        const double s = amplitude * std::sin(2.0 * 3.14159265358979323846 * freqHz * t);
        const auto sample = static_cast<int16_t>(s * 32767.0);
        for (uint16 c = 0; c < channels; ++c)
        {
            b.Add(static_cast<uint8>(static_cast<uint16>(sample) & 0xFF));
            b.Add(static_cast<uint8>((static_cast<uint16>(sample) >> 8) & 0xFF));
        }
    }
    return b;
}

// Distinct left/right constant WAV (for L/R adapter parity tests, A2).
// test-only fixture builder; convertible params are intentional here.
inline Array<uint8> MakeLrWav(uint32 sampleRate, // NOLINT(bugprone-easily-swappable-parameters)
                              uint32 frames,
                              int16_t left,
                              int16_t right)
{
    Array<uint8> b;
    const uint16 channels = 2;
    const uint16 bitsPerSample = 16;
    const uint16 blockAlign = static_cast<uint16>(channels * (bitsPerSample / 8));
    const uint32 byteRate = sampleRate * blockAlign;
    const uint32 dataBytes = frames * blockAlign;
    const uint8 riff[4] = {'R', 'I', 'F', 'F'};
    b.AddRange(foundation::BorrowedView<const uint8>(riff, 4));
    PutU32(b, 36 + dataBytes);
    const uint8 wave[4] = {'W', 'A', 'V', 'E'};
    b.AddRange(foundation::BorrowedView<const uint8>(wave, 4));
    const uint8 fmt[4] = {'f', 'm', 't', ' '};
    b.AddRange(foundation::BorrowedView<const uint8>(fmt, 4));
    PutU32(b, 16);
    PutU16(b, 1);
    PutU16(b, channels);
    PutU32(b, sampleRate);
    PutU32(b, byteRate);
    PutU16(b, blockAlign);
    PutU16(b, bitsPerSample);
    const uint8 data[4] = {'d', 'a', 't', 'a'};
    b.AddRange(foundation::BorrowedView<const uint8>(data, 4));
    PutU32(b, dataBytes);
    for (uint32 f = 0; f < frames; ++f)
    {
        PutU16(b, static_cast<uint16>(left));
        PutU16(b, static_cast<uint16>(right));
    }
    return b;
}

} // namespace ludus::audio::test
