#include "internal/token_text.hpp"

#include <ludus/tooling/shader_language/limits.hpp>

#include <span>

namespace ludus::tooling::shader_language::internal
{
std::string_view SpellToken(foundation::BorrowedView<const shader_compiler::SourceFile> files,
                            std::string_view synthesized,
                            const Token& token) noexcept
{
    std::string_view storage;
    if (token.IsSynthesized == true)
    {
        storage = synthesized;
    }
    else if (token.Location.FileIndex < files.GetSize())
    {
        const shader_compiler::SourceFile& file = files[token.Location.FileIndex];
        if (file.Text.IsValid() == false)
        {
            return {};
        }
        storage = std::string_view(file.Text.GetDataOrNull(), file.Text.GetSize());
    }
    const foundation::usize offset = token.SpellingOffset;
    const foundation::usize size = token.SpellingSize;
    if (offset > storage.size() || size > storage.size() - offset)
    {
        return {};
    }
    return storage.substr(offset, size);
}

std::string_view TokenText::Spell(const Token& token) const noexcept
{
    return SpellToken(mFiles, GetSynthesized(), token);
}

TextOutcome TokenText::TryAppend(std::string_view text, foundation::uint32& outOffset) noexcept
{
    const foundation::usize offset = mSynthesized.GetSize();
    if (text.size() > MAX_SYNTHESIZED_BYTES - offset)
    {
        return TextOutcome::Limit;
    }
    if (mSynthesized.TryAddRange(std::span<const char>(text.data(), text.size())) == false)
    {
        return TextOutcome::OutOfMemory;
    }
    outOffset = static_cast<foundation::uint32>(offset);
    return TextOutcome::Ok;
}
} // namespace ludus::tooling::shader_language::internal
