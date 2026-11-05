#include "internal/macro_table.hpp"

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

namespace ludus::tooling::shader_language::internal
{
namespace
{
/// Bytes of table storage per macro name; names are identifiers, usually far shorter.
constexpr foundation::usize NAME_TABLE_BYTES_PER_MACRO = 512;
/// Largest name; a macro name is a token of a file of at most MAX_SOURCE_BYTES bytes.
constexpr foundation::usize MAX_NAME_BYTES = 4096;
} // namespace

MacroTableOutcome MacroTable::Initialize() noexcept
{
    foundation::StringTableConfig config;
    config.MaxSpellingBytes = MAX_NAME_BYTES;
    config.MaxEntries = static_cast<foundation::uint32>(MAX_MACROS + 1);
    config.MaxAllocatedBytes = MAX_MACROS * NAME_TABLE_BYTES_PER_MACRO;
    config.HashPolicy = foundation::StringHashPolicy::Trusted;
    config.RequireUtf8 = false;
    const foundation::StringStatus status =
        foundation::CreateTable(config, foundation::GetSystemAllocationDomain(), mNames);
    return status == foundation::StringStatus::Ok ? MacroTableOutcome::Ok : MacroTableOutcome::OutOfMemory;
}

foundation::uint32 MacroTable::Find(std::string_view name) const noexcept
{
    foundation::NameId id;
    if (mNames.TryFind(name, id) != foundation::StringStatus::Ok || id.Index.Value == 0)
    {
        return NO_MACRO;
    }
    return id.Index.Value - 1;
}

MacroTableOutcome
MacroTable::TryIntern(std::string_view name, const Token& nameToken, foundation::uint32& outIndex) noexcept
{
    const foundation::uint32 existing = Find(name);
    if (existing != NO_MACRO)
    {
        outIndex = existing;
        return MacroTableOutcome::Ok;
    }
    if (mMacros.GetSize() >= MAX_MACROS)
    {
        return MacroTableOutcome::Limit;
    }
    if (mMacros.TryEnsureCapacity(mMacros.GetSize() + 1) == false)
    {
        return MacroTableOutcome::OutOfMemory;
    }
    foundation::NameId id;
    const foundation::StringStatus status = mNames.TryIntern(name, id);
    if (status == foundation::StringStatus::CapacityExceeded || status == foundation::StringStatus::TooLarge)
    {
        return MacroTableOutcome::Limit;
    }
    if (status != foundation::StringStatus::Ok)
    {
        return MacroTableOutcome::OutOfMemory;
    }
    MacroDefinition definition;
    definition.Name = nameToken;
    mMacros.Add(definition);
    outIndex = id.Index.Value - 1;
    return MacroTableOutcome::Ok;
}

MacroTableOutcome MacroTable::TryDefine(foundation::uint32 index,
                                        const MacroDefinition& definition,
                                        const foundation::Array<BodyToken>& body) noexcept
{
    const foundation::usize first = mBodies.GetSize();
    if (mBodies.TryAddRange(body.AsSpan()) == false)
    {
        return MacroTableOutcome::OutOfMemory;
    }
    MacroDefinition& stored = mMacros[index];
    stored = definition;
    stored.FirstBodyToken = static_cast<foundation::uint32>(first);
    stored.BodyTokenCount = static_cast<foundation::uint32>(body.GetSize());
    return MacroTableOutcome::Ok;
}
} // namespace ludus::tooling::shader_language::internal
