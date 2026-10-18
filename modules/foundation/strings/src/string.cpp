#include <ludus/foundation/strings/string.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <cstring>
#include <utility>

namespace ludus::foundation
{
namespace
{
constexpr usize kMaxCapacity = (static_cast<usize>(-1) >> 1) - 1;
usize Growth(usize current, // NOLINT(bugprone-easily-swappable-parameters): current/requested capacity policy
             usize requested) noexcept
{
    const usize doubled = current <= kMaxCapacity / 2 ? current * 2 : kMaxCapacity;
    return requested > doubled ? requested : doubled;
}
void CopyBytes(char* destination, std::string_view source) noexcept
{
    if (source.empty() == false)
    {
        std::memmove(destination, source.data(), source.size());
    }
}
} // namespace

StringStatus detail::AssignBounded(char* destination, usize& size, usize capacity, std::string_view source) noexcept
{
    if (source.size() > capacity)
    {
        return StringStatus::CapacityExceeded;
    }
    CopyBytes(destination, source);
    size = source.size();
    destination[size] = '\0';
    return StringStatus::Ok;
}
StringStatus detail::AppendBounded(char* destination, usize& size, usize capacity, std::string_view source) noexcept
{
    if (source.size() > capacity - size)
    {
        return StringStatus::CapacityExceeded;
    }
    CopyBytes(destination + size, source);
    size += source.size();
    destination[size] = '\0';
    return StringStatus::Ok;
}

String::String(const AllocationDomain& domain) noexcept : mDomain(&domain) {}
String::~String() noexcept
{
    Release();
}
String::String(String&& other) noexcept : mDomain(other.mDomain)
{
    *this = std::move(other);
}
String& String::operator=(String&& other) noexcept
{
    if (this != &other)
    {
        Release();
        mDomain = other.mDomain;
        mSize = other.mSize;
        mCapacity = other.mCapacity;
        mStorage = other.mStorage;
        other.mCapacity = kInlineCapacity;
        other.mSize = 0;
        other.mStorage.Inline[0] = '\0';
    }
    return *this;
}
const char* String::GetData() const& noexcept
{
    return mCapacity == kInlineCapacity ? mStorage.Inline : mStorage.Heap;
}
char* String::MutableData() noexcept
{
    return mCapacity == kInlineCapacity ? mStorage.Inline : mStorage.Heap;
}
void String::Release() noexcept
{
    if (mCapacity != kInlineCapacity)
    {
        mDomain->Free(mStorage.Heap, mCapacity + 1, alignof(char));
    }
}
void String::Clear() noexcept
{
    mSize = 0;
    MutableData()[0] = '\0';
}
StringStatus String::TryEnsureCapacity(usize capacity) noexcept
{
    if (capacity > kMaxCapacity)
    {
        return StringStatus::TooLarge;
    }
    if (capacity <= mCapacity)
    {
        return StringStatus::Ok;
    }
    const usize grown = Growth(mCapacity, capacity);
    auto* bytes = static_cast<char*>(mDomain->TryAllocate(grown + 1, alignof(char)));
    if (bytes == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    CopyBytes(bytes, GetView());
    bytes[mSize] = '\0';
    Release();
    mStorage.Heap = bytes;
    mCapacity = grown;
    return StringStatus::Ok;
}
StringStatus String::TryAssign(std::string_view value) noexcept
{
    if (value.size() > kMaxCapacity)
    {
        return StringStatus::TooLarge;
    }
    if (value.size() <= mCapacity)
    {
        return detail::AssignBounded(MutableData(), mSize, mCapacity, value);
    }
    const usize grown = Growth(mCapacity, value.size());
    auto* bytes = static_cast<char*>(mDomain->TryAllocate(grown + 1, alignof(char)));
    if (bytes == nullptr)
    {
        return StringStatus::OutOfMemory;
    }
    // Source may point into old storage; read it before releasing that storage.
    CopyBytes(bytes, value);
    bytes[value.size()] = '\0';
    Release();
    mStorage.Heap = bytes;
    mCapacity = grown;
    mSize = value.size();
    return StringStatus::Ok;
}
StringStatus String::TryAppend(std::string_view value) noexcept
{
    return TryAppendMany(std::span<const std::string_view>{&value, 1});
}
StringStatus String::TryAppendMany(std::span<const std::string_view> values) noexcept
{
    usize total = mSize;
    for (auto value : values)
    {
        if (TryAdd(total, value.size(), total) == false || total > kMaxCapacity)
        {
            return StringStatus::TooLarge;
        }
    }
    char* destination = MutableData();
    usize capacity = mCapacity;
    if (total > mCapacity)
    {
        capacity = Growth(mCapacity, total);
        destination = static_cast<char*>(mDomain->TryAllocate(capacity + 1, alignof(char)));
        if (destination == nullptr)
        {
            return StringStatus::OutOfMemory;
        }
        CopyBytes(destination, GetView());
    }
    usize offset = mSize;
    for (auto value : values)
    {
        CopyBytes(destination + offset, value);
        offset += value.size();
    }
    destination[total] = '\0';
    if (destination != MutableData())
    {
        Release();
        mStorage.Heap = destination;
        mCapacity = capacity;
    }
    mSize = total;
    return StringStatus::Ok;
}
StringStatus String::CloneTo(const AllocationDomain& domain, String& output) const noexcept
{
    String copy(domain);
    const StringStatus status = copy.TryAssign(GetView());
    if (status == StringStatus::Ok)
    {
        output = std::move(copy);
    }
    return status;
}
String StringBuilder::TakeString() noexcept
{
    return std::move(mString);
}
} // namespace ludus::foundation
