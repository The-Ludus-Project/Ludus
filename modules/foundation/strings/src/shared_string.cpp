#include <ludus/foundation/strings/shared_string.hpp>

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/string.hpp>

#include "internal/reference_count.hpp"

#include <atomic>
#include <cstring>
#include <new>
#include <utility>

namespace ludus::foundation
{
struct SharedString::Block final
{
    std::atomic<uint64> References{1};
    const AllocationDomain* Domain;
    usize Size;
    [[nodiscard]] char* Bytes() noexcept
    {
        return reinterpret_cast<char*>(this + 1);
    }
};

SharedString::~SharedString() noexcept
{
    Release();
}
SharedString::SharedString(const SharedString& other) noexcept : mBlock(other.mBlock)
{
    Retain();
}
SharedString& SharedString::operator=(const SharedString& other) noexcept
{
    SharedString retained(other);
    *this = std::move(retained);
    return *this;
}
SharedString::SharedString(SharedString&& other) noexcept : mBlock(std::exchange(other.mBlock, nullptr)) {}
SharedString& SharedString::operator=(SharedString&& other) noexcept
{
    if (this != &other)
    {
        Release();
        mBlock = std::exchange(other.mBlock, nullptr);
    }
    return *this;
}
void SharedString::Retain() const noexcept
{
    if (mBlock != nullptr && detail::RetainReference(mBlock->References))
    {
        LUDUS_CHECK(false, "SharedString reference count saturated; pinning allocation");
    }
}
void SharedString::Release() noexcept
{
    if (mBlock != nullptr && detail::ReleaseReference(mBlock->References))
    {
        const AllocationDomain* domain = mBlock->Domain;
        const usize bytes = sizeof(Block) + mBlock->Size + 1;
        mBlock->~Block();
        domain->Free(mBlock, bytes, alignof(Block));
    }
}
usize SharedString::GetSize() const noexcept
{
    return mBlock == nullptr ? 0 : mBlock->Size;
}
const char* SharedString::GetData() const& noexcept
{
    return mBlock == nullptr ? "" : mBlock->Bytes();
}
bool operator==(const SharedString& left, const SharedString& right) noexcept
{
    return left.mBlock == right.mBlock || left.GetView() == right.GetView();
}
StringStatus CreateShared(std::string_view value, const AllocationDomain& domain, SharedString& output) noexcept
{
    SharedString result;
    if (value.empty() == false)
    {
        usize bytes = 0;
        if (TryAdd(value.size(), sizeof(SharedString::Block) + 1, bytes) == false ||
            bytes > (static_cast<usize>(-1) >> 1))
        {
            return StringStatus::TooLarge;
        }
        void* storage = domain.TryAllocate(bytes, alignof(SharedString::Block));
        if (storage == nullptr)
        {
            return StringStatus::OutOfMemory;
        }
        result.mBlock = new (storage) SharedString::Block{ .Domain = &domain, .Size = value.size() };
        std::memcpy(result.mBlock->Bytes(), value.data(), value.size());
        result.mBlock->Bytes()[value.size()] = '\0';
    }
    output = std::move(result);
    return StringStatus::Ok;
}
StringStatus CopyToString(const SharedString& value, String& output) noexcept
{
    return output.TryAssign(value.GetView());
}
} // namespace ludus::foundation
