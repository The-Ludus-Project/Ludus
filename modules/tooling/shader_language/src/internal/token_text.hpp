#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/token.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Result of appending synthesized text.
enum class TextOutcome : foundation::uint8
{
    Ok,          ///< The text was appended.
    OutOfMemory, ///< Allocation failed.
    Limit,       ///< MAX_SYNTHESIZED_BYTES would be exceeded.
};

/// @brief Resolves token spellings against the snapshot files and the synthesized text.
///
/// Views returned by Spell stay valid only until the next append, because appending may move the
/// synthesized text.
class TokenText final
{
public:
    /// Borrow the snapshot files and the result's synthesized text; both must outlive this object.
    TokenText(foundation::BorrowedView<const shader_compiler::SourceFile> files,
              foundation::Array<char>& inoutSynthesized) noexcept
        : mFiles(files), mSynthesized(inoutSynthesized)
    {
    }

    /// Spelling of `token`, or an empty view if it is out of range.
    [[nodiscard]] std::string_view Spell(const Token& token) const noexcept;

    /// Append `text` and receive its offset in the synthesized text.
    [[nodiscard]] TextOutcome TryAppend(std::string_view text, foundation::uint32& outOffset) noexcept;

    /// The synthesized text appended so far.
    [[nodiscard]] std::string_view GetSynthesized() const noexcept
    {
        return std::string_view(mSynthesized.GetData(), mSynthesized.GetSize());
    }

private:
    foundation::BorrowedView<const shader_compiler::SourceFile> mFiles;
    foundation::Array<char>& mSynthesized;
};

/// Spelling of a token in a file or synthesized buffer, checked against both extents.
[[nodiscard]] std::string_view SpellToken(foundation::BorrowedView<const shader_compiler::SourceFile> files,
                                          std::string_view synthesized,
                                          const Token& token) noexcept;
} // namespace ludus::tooling::shader_language::internal
