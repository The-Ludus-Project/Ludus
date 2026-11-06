#include "internal/scope_table.hpp"

#include <ludus/foundation/memory/allocation_domain.hpp>

namespace ludus::tooling::shader_language::internal
{
namespace
{
/// Bytes of name-table storage per name; names are identifiers, usually far shorter.
constexpr foundation::usize NAME_TABLE_BYTES_PER_NAME = 128;
/// Largest name; names are tokens of files of at most MAX_SOURCE_BYTES bytes.
constexpr foundation::usize MAX_NAME_BYTES = 4096;
} // namespace

ScopeOutcome ScopeTable::Initialize() noexcept
{
    foundation::StringTableConfig config;
    config.MaxSpellingBytes = MAX_NAME_BYTES;
    config.MaxEntries = static_cast<foundation::uint32>(MAX_SCOPE_NAMES + 1);
    config.MaxAllocatedBytes = MAX_SCOPE_NAMES * NAME_TABLE_BYTES_PER_NAME;
    config.HashPolicy = foundation::StringHashPolicy::Trusted;
    config.RequireUtf8 = false;
    if (foundation::CreateTable(config, foundation::GetSystemAllocationDomain(), mNames) !=
        foundation::StringStatus::Ok)
    {
        return ScopeOutcome::OutOfMemory;
    }
    return Enter();
}

ScopeOutcome ScopeTable::Enter() noexcept
{
    return mScopeStarts.TryAdd(mBindings.GetSize()) == true ? ScopeOutcome::Ok : ScopeOutcome::OutOfMemory;
}

void ScopeTable::Leave() noexcept
{
    if (mScopeStarts.IsEmpty() == true)
    {
        return;
    }
    const foundation::usize start = mScopeStarts.GetLast();
    mScopeStarts.RemoveLast();
    while (mBindings.GetSize() > start)
    {
        const Binding& binding = mBindings.GetLast();
        mHeads[binding.Name] = binding.Previous;
        mBindings.RemoveLast();
    }
}

ScopeOutcome
ScopeTable::TryBind(std::string_view name, foundation::uint32 symbol, foundation::uint32& outPrevious) noexcept
{
    outPrevious = UNBOUND;
    foundation::NameId id;
    const foundation::StringStatus interned = mNames.TryIntern(name, id);
    if (interned == foundation::StringStatus::CapacityExceeded || interned == foundation::StringStatus::TooLarge)
    {
        return ScopeOutcome::Limit;
    }
    if (interned != foundation::StringStatus::Ok)
    {
        return ScopeOutcome::OutOfMemory;
    }
    const foundation::uint32 nameIndex = id.Index.Value;
    if (nameIndex >= mHeads.GetSize() && mHeads.TryResize(nameIndex + 1, UNBOUND) == false)
    {
        return ScopeOutcome::OutOfMemory;
    }
    const foundation::uint32 depth = static_cast<foundation::uint32>(mScopeStarts.GetSize());
    const foundation::uint32 head = mHeads[nameIndex];
    if (head != UNBOUND && mBindings[head].Depth == depth)
    {
        outPrevious = mBindings[head].Symbol;
        return ScopeOutcome::Ok;
    }
    Binding binding;
    binding.Symbol = symbol;
    binding.Name = nameIndex;
    binding.Depth = depth;
    binding.Previous = head;
    if (mBindings.TryAdd(binding) == false)
    {
        return ScopeOutcome::OutOfMemory;
    }
    mHeads[nameIndex] = static_cast<foundation::uint32>(mBindings.GetSize() - 1);
    return ScopeOutcome::Ok;
}

foundation::uint32 ScopeTable::Find(std::string_view name) const noexcept
{
    foundation::NameId id;
    if (mNames.TryFind(name, id) != foundation::StringStatus::Ok || id.Index.Value >= mHeads.GetSize())
    {
        return UNBOUND;
    }
    const foundation::uint32 head = mHeads[id.Index.Value];
    return head == UNBOUND ? UNBOUND : mBindings[head].Symbol;
}
} // namespace ludus::tooling::shader_language::internal
