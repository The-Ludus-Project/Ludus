#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::foundation
{
/// @brief Explicit result of FoundationStrings operations.
///
/// Each operation documents which values it returns and what it leaves unchanged on failure.
/// Values are not stable serialized codes.
enum class StringStatus : uint8
{
    /// The operation succeeded and wrote its documented outputs.
    Ok,
    /// The allocation domain could not supply the requested storage.
    OutOfMemory,
    /// A length or size computation exceeds a representable or configured maximum, such as a
    /// table's spelling-length limit.
    TooLarge,
    /// A fixed capacity or configured budget (entries, allocated bytes or bounded storage) has no
    /// room for the request.
    CapacityExceeded,
    /// The arguments or configuration violate a documented requirement, such as a nonempty output
    /// owner.
    InvalidArgument,
    /// The bytes are not well-formed UTF-8.
    InvalidUtf8,
    /// The bytes contain a zero byte where the operation forbids one.
    EmbeddedZero,
    /// The owner is empty, or the ID is invalid or outside its table's entries.
    InvalidHandle,
    /// The ID belongs to a different table than the one asked to resolve it.
    WrongTable,
    /// A lookup did not find the requested spelling. Nothing was inserted.
    NotFound,
    /// The process has issued every table token; no further table can be created.
    TokenExhausted,
    /// A table lookup visited every lookup slot without reaching the requested
    /// spelling or an empty slot. The load-factor invariant makes this unreachable
    /// for intact metadata, so it reports corrupted table state; the lookup leaves
    /// the table and the caller's output unchanged.
    ProbeExhausted,
};
} // namespace ludus::foundation
