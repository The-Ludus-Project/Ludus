#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>

namespace ludus::editor::tests
{
/// Source that fails every request, as when the operating system cannot supply entropy.
inline foundation::identity::EntropySource MakeUnavailableEntropySource() noexcept
{
    foundation::identity::EntropySource source;
    source.FillBytesOrNull = [](void*, foundation::uint8*, foundation::usize) noexcept { return false; };
    return source;
}

/// Byte written by MakeFixedEntropySource(); its UUIDv4 text is FIXED_UUID_TEXT.
inline constexpr foundation::uint8 FIXED_ENTROPY_BYTE = 0xAB;
/// Canonical text of the UUIDv4 built from sixteen FIXED_ENTROPY_BYTE bytes.
inline constexpr const char* FIXED_UUID_TEXT = "abababab-abab-4bab-abab-abababababab";

/// Deterministic source that fills every byte with FIXED_ENTROPY_BYTE.
inline foundation::identity::EntropySource MakeFixedEntropySource() noexcept
{
    foundation::identity::EntropySource source;
    source.FillBytesOrNull = [](void*, foundation::uint8* outBytes, foundation::usize size) noexcept {
        for (foundation::usize i = 0; i < size; ++i)
        {
            outBytes[i] = FIXED_ENTROPY_BYTE;
        }
        return true;
    };
    return source;
}

/// State of MakeSequentialEntropySource(): the byte value of its next request.
struct SequentialEntropy final
{
    /// Value written to every byte of the next request; incremented after each request.
    foundation::uint8 NextByte = 1;
};

/// Deterministic source that fills each request with one byte value, then increments it,
/// so successive UUIDs differ. `state` must outlive every use of the returned source.
inline foundation::identity::EntropySource MakeSequentialEntropySource(SequentialEntropy& state) noexcept
{
    foundation::identity::EntropySource source;
    source.ContextOrNull = &state;
    source.FillBytesOrNull = [](void* contextOrNull, foundation::uint8* outBytes, foundation::usize size) noexcept {
        if (contextOrNull == nullptr)
        {
            return false;
        }
        SequentialEntropy& sequence = *static_cast<SequentialEntropy*>(contextOrNull);
        for (foundation::usize i = 0; i < size; ++i)
        {
            outBytes[i] = sequence.NextByte;
        }
        ++sequence.NextByte;
        return true;
    };
    return source;
}
} // namespace ludus::editor::tests
