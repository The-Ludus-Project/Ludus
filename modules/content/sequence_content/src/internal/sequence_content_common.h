#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/sequence_content/sequence_content.h>

#include <string_view>

namespace ludus::content::sequence_content::internal
{
/// Builds a bounded `name[index].member` field path for diagnostics.
class FieldPath final
{
public:
    /// Start a path with a member name.
    explicit FieldPath(std::string_view name) noexcept;
    /// Append `.name`.
    [[nodiscard]] FieldPath Member(std::string_view name) const noexcept;
    /// Append `[index]`.
    [[nodiscard]] FieldPath Element(usize index) const noexcept;
    /// The path text; truncated, never failing, when it exceeds MessageText.
    [[nodiscard]] LUDUS_INLINE std::string_view GetView() const noexcept
    {
        return mText.View();
    }

private:
    FieldPath() noexcept = default;
    void append(std::string_view text) noexcept;
    MessageText mText{};
};

/// Record a failure with its field and return its status.
SequenceContentStatus
Fail(SequenceContentError& outError, SequenceContentStatus status, std::string_view field) noexcept;

/// Validate a document after reading or decoding it, filling outError on failure.
[[nodiscard]] SequenceContentStatus ValidateDocument(const SequenceDocument& document,
                                                     SequenceContentError& outError) noexcept;
} // namespace ludus::content::sequence_content::internal
