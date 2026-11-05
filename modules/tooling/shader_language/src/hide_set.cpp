#include "internal/hide_set.hpp"

#include "internal/array_growth.hpp"

namespace ludus::tooling::shader_language::internal
{
using foundation::uint32;
using foundation::usize;

HideSetTable::Range HideSetTable::getRange(uint32 set) const noexcept
{
    // Set zero is the empty set and is never stored.
    if (set == EMPTY_HIDE_SET || set > mSets.GetSize())
    {
        return Range{};
    }
    return mSets[set - 1];
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a set and an element, as in every set API.
bool HideSetTable::Contains(uint32 set, uint32 macroIndex) const noexcept
{
    const Range range = getRange(set);
    for (uint32 i = 0; i < range.Count; ++i)
    {
        const uint32 element = mElements[range.First + i];
        if (element == macroIndex)
        {
            return true;
        }
        if (element > macroIndex)
        {
            return false;
        }
    }
    return false;
}

HideSetOutcome HideSetTable::TryInsert(uint32 set, uint32 macroIndex, uint32& outSet) noexcept
{
    if (Contains(set, macroIndex) == true)
    {
        outSet = set;
        return HideSetOutcome::Ok;
    }
    const usize first = mElements.GetSize();
    const Range range = getRange(set);
    if (first + range.Count + 1 > MAX_HIDE_SET_ELEMENTS)
    {
        return HideSetOutcome::Limit;
    }
    if (TryReserveGrowing(mElements, first + range.Count + 1) == false ||
        TryReserveGrowing(mSets, mSets.GetSize() + 1) == false)
    {
        return HideSetOutcome::OutOfMemory;
    }
    bool isInserted = false;
    for (uint32 i = 0; i < range.Count; ++i)
    {
        const uint32 element = mElements[range.First + i];
        if (isInserted == false && macroIndex < element)
        {
            mElements.Add(macroIndex);
            isInserted = true;
        }
        mElements.Add(element);
    }
    if (isInserted == false)
    {
        mElements.Add(macroIndex);
    }
    mSets.Add(Range{static_cast<uint32>(first), range.Count + 1});
    outSet = static_cast<uint32>(mSets.GetSize());
    return HideSetOutcome::Ok;
}

HideSetOutcome HideSetTable::TryUnite(uint32 left, uint32 right, uint32& outSet) noexcept
{
    if (left == right || right == EMPTY_HIDE_SET)
    {
        outSet = left;
        return HideSetOutcome::Ok;
    }
    if (left == EMPTY_HIDE_SET)
    {
        outSet = right;
        return HideSetOutcome::Ok;
    }
    return combine(left, right, Combination::Union, outSet);
}

HideSetOutcome HideSetTable::TryIntersect(uint32 left, uint32 right, uint32& outSet) noexcept
{
    if (left == right)
    {
        outSet = left;
        return HideSetOutcome::Ok;
    }
    if (left == EMPTY_HIDE_SET || right == EMPTY_HIDE_SET)
    {
        outSet = EMPTY_HIDE_SET;
        return HideSetOutcome::Ok;
    }
    return combine(left, right, Combination::Intersection, outSet);
}

HideSetOutcome HideSetTable::combine(uint32 left, uint32 right, Combination combination, uint32& outSet) noexcept
{
    const Range leftRange = getRange(left);
    const Range rightRange = getRange(right);
    const usize first = mElements.GetSize();
    const usize most = static_cast<usize>(leftRange.Count) + rightRange.Count;
    if (first + most > MAX_HIDE_SET_ELEMENTS)
    {
        return HideSetOutcome::Limit;
    }
    if (TryReserveGrowing(mElements, first + most) == false || TryReserveGrowing(mSets, mSets.GetSize() + 1) == false)
    {
        return HideSetOutcome::OutOfMemory;
    }
    // Merge two sorted sequences. Capacity is reserved, so Add cannot reallocate while reading.
    uint32 leftIndex = 0;
    uint32 rightIndex = 0;
    while (leftIndex < leftRange.Count || rightIndex < rightRange.Count)
    {
        const bool hasLeft = leftIndex < leftRange.Count;
        const bool hasRight = rightIndex < rightRange.Count;
        const uint32 leftElement = hasLeft == true ? mElements[leftRange.First + leftIndex] : 0;
        const uint32 rightElement = hasRight == true ? mElements[rightRange.First + rightIndex] : 0;
        if (hasLeft == true && hasRight == true && leftElement == rightElement)
        {
            mElements.Add(leftElement);
            ++leftIndex;
            ++rightIndex;
            continue;
        }
        const bool isLeftNext = hasRight == false || (hasLeft == true && leftElement < rightElement);
        if (combination == Combination::Union)
        {
            mElements.Add(isLeftNext == true ? leftElement : rightElement);
        }
        if (isLeftNext == true)
        {
            ++leftIndex;
        }
        else
        {
            ++rightIndex;
        }
    }
    const uint32 count = static_cast<uint32>(mElements.GetSize() - first);
    if (count == 0)
    {
        outSet = EMPTY_HIDE_SET;
        return HideSetOutcome::Ok;
    }
    mSets.Add(Range{static_cast<uint32>(first), count});
    outSet = static_cast<uint32>(mSets.GetSize());
    return HideSetOutcome::Ok;
}
} // namespace ludus::tooling::shader_language::internal
