#include "internal/fix_builder.hpp"

namespace ludus::tooling::shader_language::internal
{
using foundation::uint32;

bool FixBuilder::isEditable(uint32 token) const noexcept
{
    if (token == NO_TOKEN || token >= mInput.Tokens.GetSize())
    {
        return false;
    }
    const Token& spelled = mInput.Tokens[token];
    return spelled.IsSynthesized == false && spelled.ExpansionIndex == NO_EXPANSION &&
           spelled.Location.FileIndex < mRequest.Files.GetSize();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a token, a byte offset and a byte count.
void FixBuilder::addEdit(uint32 token, uint32 offset, uint32 removed, std::string_view text) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (mFix.EditCount >= MAX_FIX_EDITS || text.size() > FixText::GetCapacity())
    {
        mIsValid = false;
        return;
    }
    TextEdit& edit = mFix.Edits[mFix.EditCount];
    edit.FileIndex = mInput.Tokens[token].Location.FileIndex;
    edit.ByteOffset = offset;
    edit.RemovedBytes = removed;
    if (edit.Inserted.TryAssign(text) != foundation::StringStatus::Ok)
    {
        mIsValid = false;
        return;
    }
    ++mFix.EditCount;
}

void FixBuilder::InsertBefore(uint32 token, std::string_view text) noexcept
{
    if (isEditable(token) == false)
    {
        mIsValid = false;
        return;
    }
    addEdit(token, mInput.Tokens[token].SpellingOffset, 0, text);
}

void FixBuilder::InsertAfter(uint32 token, std::string_view text) noexcept
{
    if (isEditable(token) == false)
    {
        mIsValid = false;
        return;
    }
    const Token& spelled = mInput.Tokens[token];
    addEdit(token, spelled.SpellingOffset + spelled.SpellingSize, 0, text);
}

void FixBuilder::Replace(uint32 token, std::string_view text) noexcept
{
    if (isEditable(token) == false)
    {
        mIsValid = false;
        return;
    }
    const Token& spelled = mInput.Tokens[token];
    addEdit(token, spelled.SpellingOffset, spelled.SpellingSize, text);
}

void FixBuilder::Remove(uint32 token) noexcept
{
    if (isEditable(token) == false)
    {
        mIsValid = false;
        return;
    }
    const Token& spelled = mInput.Tokens[token];
    const foundation::BorrowedView<const char> text = mRequest.Files[spelled.Location.FileIndex].Text;
    uint32 removed = spelled.SpellingSize;
    const foundation::usize next = static_cast<foundation::usize>(spelled.SpellingOffset) + removed;
    if (next < text.GetSize() && text[next] == ' ')
    {
        ++removed;
    }
    addEdit(token, spelled.SpellingOffset, removed, {});
}
} // namespace ludus::tooling::shader_language::internal
