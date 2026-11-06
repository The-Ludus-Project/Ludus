#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/preprocessor.hpp>
#include <ludus/tooling/shader_language/syntax.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// @brief Builds a DiagnosticFix from edits anchored on preprocessed tokens.
///
/// An edit succeeds only for a token spelled in a request file, outside any macro expansion. If any
/// edit fails, or more than MAX_FIX_EDITS are added, the whole fix is withdrawn: a partial repair
/// would be wrong.
class FixBuilder final
{
public:
    FixBuilder(const PreprocessRequest& request, const PreprocessResult& preprocessed) noexcept
        : mRequest(request), mInput(preprocessed)
    {
    }

    /// Insert `text` just before token `token`.
    void InsertBefore(foundation::uint32 token, std::string_view text) noexcept;
    /// Insert `text` just after token `token`.
    void InsertAfter(foundation::uint32 token, std::string_view text) noexcept;
    /// Remove token `token` and one following space, if any.
    void Remove(foundation::uint32 token) noexcept;
    /// Replace the spelling of token `token` with `text`.
    void Replace(foundation::uint32 token, std::string_view text) noexcept;

    /// The fix, or an empty one when an edit could not be made.
    [[nodiscard]] DiagnosticFix GetFix() const noexcept
    {
        return mIsValid == true ? mFix : DiagnosticFix{};
    }

private:
    [[nodiscard]] bool isEditable(foundation::uint32 token) const noexcept;
    // NOLINTBEGIN(bugprone-easily-swappable-parameters): a token, a byte offset and a byte count.
    void addEdit(foundation::uint32 token,
                 foundation::uint32 offset,
                 foundation::uint32 removed,
                 std::string_view text) noexcept;
    // NOLINTEND(bugprone-easily-swappable-parameters)

    const PreprocessRequest& mRequest;
    const PreprocessResult& mInput;
    DiagnosticFix mFix;
    bool mIsValid = true;
};
} // namespace ludus::tooling::shader_language::internal
