#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/config/config.hpp>
#include <ludus/foundation/parsing/json.hpp>

namespace ludus::foundation::config
{
/// @brief V1 cooked layer: exact fields {version,schema,layer,values}; each record has
/// {key,type,value,source,line}. 64-bit integers use canonical decimal strings to survive tools with binary64 JSON
/// numbers.
///
/// Unknown fields/types fail closed.
/// The owner bumps schema identity when descriptor contracts change. Input is
/// bounded to 256 KiB. This adapter owns no filesystem or schema registration.
inline constexpr usize MAX_BUNDLE_BYTES = 262144;
/// Decode an exact value kind from a borrowed JSON node. Failure preserves the output.
/// Applies wire-format checks. Context validation applies per-setting constraints.
[[nodiscard]] Status DecodeValue(parsing::JsonValue input, Type type, Value& output) noexcept;
/// Append the selected payload to the writer using the configuration wire encoding.
/// The caller finishes the writer to observe capacity or encoding failures.
void EncodeValue(parsing::JsonWriter& writer, const Value& value) noexcept;
/// @brief On success leaves ONE replacement prepared; caller commits at its boundary.
///
/// Failure never publishes a partial layer. Caller-supplied domain outlives call.
[[nodiscard]] Status PrepareJson(Context& context,
                                 std::string_view input,
                                 Layer expectedLayer,
                                 std::string_view schemaIdentity,
                                 uint64 expectedRevision,
                                 const AllocationDomain& domain,
                                 Diagnostic& error) noexcept;
/// @brief Prepares one cooked layer embedded in an already parsed JSON document.
/// The caller bounds the enclosing document and keeps it alive for the call. The same schema, rank,
/// record and revision checks apply as in PrepareJson. Success leaves a prepared replacement for
/// Commit. Failure publishes nothing. @p domain supplies temporary storage and @p outDiagnostic reports
/// layer or record errors without document byte offsets.
[[nodiscard]] Status PrepareJsonLayer(Context& inoutContext,
                                      parsing::JsonValue root,
                                      Layer expectedLayer,
                                      std::string_view schemaIdentity,
                                      uint64 expectedRevision,
                                      const AllocationDomain& domain,
                                      Diagnostic& outDiagnostic) noexcept;
/// @brief Sparse explicit assignments only (including explicit values equal to defaults).
///
/// Output view is preserved on failure; caller buffer may contain a partial prefix.
[[nodiscard]] Status WriteLayer(const Context& context,
                                Layer layer,
                                std::string_view schemaIdentity,
                                std::span<uint8> buffer,
                                std::span<const uint8>& output) noexcept;
/// @brief Export version 1 schema metadata from the context in declaration order.
///
/// Each record includes the allowed source bitmask and schema-local application group.
/// Numeric kinds include inclusive descriptor bounds. Integer bounds are decimal strings
/// and real bounds are JSON numbers. Enum and String include a UTF-8 byte limit.
/// The kind's representable range and native validation still apply.
///
/// Requires an initialized context and a nonempty UTF-8 identity of at most 128 bytes with no NUL.
/// On success the output borrows the caller buffer and includes a trailing newline.
/// On failure the output view is preserved. The buffer may contain a partial prefix.
[[nodiscard]] Status WriteSchema(const Context& context,
                                 std::string_view schemaIdentity,
                                 std::span<uint8> buffer,
                                 std::span<const uint8>& outOutput) noexcept;
} // namespace ludus::foundation::config
