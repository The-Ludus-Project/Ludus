#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <new>

// R6 domain storage: existing Ludus arrays own buffers; pages keep native callback
// addresses stable. Budgets cover requested bytes and reallocating overlap.
// See docs/architecture/rhi-gdi.md, R6, and the retained R2 lifetime contract.
namespace ludus::graphics::rhi::internal
{
const StorageLimits& CurrentStorageLimits() noexcept;
bool ValidStorageLimits(const StorageLimits&) noexcept;
bool ConfigureStorage(const StorageLimits&) noexcept;
RasterStatus StorageFailure() noexcept;
bool ReserveHostStorage(foundation::usize) noexcept;
void ReleaseHostStorage(foundation::usize) noexcept;
void StorageAllocationFailed() noexcept;
void StorageCapacityFailed() noexcept;
#if defined(LUDUS_RHI_TEST_STORAGE)
void FailNextStorageAllocation() noexcept;
#endif
bool ReserveResourceStorage(foundation::uint64) noexcept;
void ReleaseResourceStorage(foundation::uint64) noexcept;
StorageUsage CurrentStorageUsage() noexcept;
bool FrameOpen() noexcept;

template <typename T>
class StorageArray final
{
public:
    StorageArray() noexcept = default;
    StorageArray(const StorageArray&) = delete;
    StorageArray& operator=(const StorageArray&) = delete;
    StorageArray(StorageArray&& other) noexcept : mValues(Move(other.mValues)) {}
    StorageArray& operator=(StorageArray&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            mValues = Move(other.mValues);
        }
        return *this;
    }
    ~StorageArray()
    {
        ReleaseHostStorage(mValues.GetCapacity() * sizeof(T));
    }
    [[nodiscard]] bool TryResize(foundation::usize size) noexcept
    {
        using namespace foundation;
        if (size > mValues.GetCapacity())
        {
            usize bytes = 0;
            if (TryMultiply(size, sizeof(T), bytes) == false)
            {
                StorageCapacityFailed();
                return false;
            }
            if (ReserveHostStorage(bytes) == false)
            {
                return false;
            }
            const auto prior = mValues.GetCapacity() * sizeof(T);
            if (mValues.TryEnsureCapacity(size) == false)
            {
                ReleaseHostStorage(bytes);
                StorageAllocationFailed();
                return false;
            }
            ReleaseHostStorage(prior);
        }
        return mValues.TryResize(size);
    }
    void Clear() noexcept
    {
        ReleaseHostStorage(mValues.GetCapacity() * sizeof(T));
        foundation::Array<T> empty;
        mValues.Swap(empty);
    }
    [[nodiscard]] foundation::usize GetSize() const noexcept
    {
        return mValues.GetSize();
    }
    [[nodiscard]] foundation::usize GetCapacity() const noexcept
    {
        return mValues.GetCapacity();
    }
    [[nodiscard]] T* GetData() noexcept
    {
        return mValues.GetData();
    }
    [[nodiscard]] const T* GetData() const noexcept
    {
        return mValues.GetData();
    }
    [[nodiscard]] T& operator[](foundation::usize i) noexcept
    {
        return mValues[i];
    }
    [[nodiscard]] const T& operator[](foundation::usize i) const noexcept
    {
        return mValues[i];
    }
    [[nodiscard]] auto begin() noexcept
    {
        return mValues.begin();
    }
    [[nodiscard]] auto end() noexcept
    {
        return mValues.end();
    }
    [[nodiscard]] auto begin() const noexcept
    {
        return mValues.begin();
    }
    [[nodiscard]] auto end() const noexcept
    {
        return mValues.end();
    }

private:
    foundation::Array<T> mValues;
};

template <typename T>
class StorageTable final
{
    static constexpr foundation::usize STORAGE_PAGE_SLOTS = 64;
    struct Page final
    {
        // Elements use their default member initializers; the check misreports them as zero enums.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        T Values[STORAGE_PAGE_SLOTS]{};
    };

    struct PageReference final
    {
        Page* Address = nullptr;
    };

public:
    StorageTable() noexcept = default;
    StorageTable(const StorageTable&) = delete;
    StorageTable& operator=(const StorageTable&) = delete;
    ~StorageTable()
    {
        for (const auto& reference : mPages)
        {
            delete reference.Address;
            ReleaseHostStorage(sizeof(Page));
        }
    }
    [[nodiscard]] bool EnsureSlots(foundation::usize count) noexcept
    {
        const auto required = count / STORAGE_PAGE_SLOTS + (count % STORAGE_PAGE_SLOTS != 0 ? 1U : 0U);
        while (mPages.GetSize() < required)
        {
            if (ReserveHostStorage(sizeof(Page)) == false)
            {
                return false;
            }
            auto* page = new (std::nothrow) Page;
            if (page == nullptr)
            {
                ReleaseHostStorage(sizeof(Page));
                StorageAllocationFailed();
                return false;
            }
            const auto index = mPages.GetSize();
            if (mPages.TryResize(index + 1) == false)
            {
                delete page;
                ReleaseHostStorage(sizeof(Page));
                return false;
            }
            mPages[index].Address = page;
        }
        if (count > mSize)
        {
            mSize = count;
        }
        return true;
    }
    [[nodiscard]] foundation::usize GetSize() const noexcept
    {
        return mSize;
    }
    [[nodiscard]] T& operator[](foundation::usize i) noexcept
    {
        LUDUS_ASSERT(i < mSize);
        return mPages[i / STORAGE_PAGE_SLOTS].Address->Values[i % STORAGE_PAGE_SLOTS];
    }
    [[nodiscard]] const T& operator[](foundation::usize i) const noexcept
    {
        LUDUS_ASSERT(i < mSize);
        return mPages[i / STORAGE_PAGE_SLOTS].Address->Values[i % STORAGE_PAGE_SLOTS];
    }
    struct Iterator final
    {
        StorageTable* Table;
        foundation::usize Index;
        T& operator*() const noexcept
        {
            return (*Table)[Index];
        }
        Iterator& operator++() noexcept
        {
            ++Index;
            return *this;
        }
        bool operator!=(const Iterator& other) const noexcept
        {
            return Index != other.Index;
        }
    };
    [[nodiscard]] Iterator begin() noexcept
    {
        return {this, 0};
    }
    [[nodiscard]] Iterator end() noexcept
    {
        return {this, mSize};
    }

private:
    StorageArray<PageReference> mPages;
    foundation::usize mSize = 0;
};
} // namespace ludus::graphics::rhi::internal
