// Host-owned ICU4C lifetime for Ludus::Unicode.
//
// Thanks to Unicode, Inc. and the ICU project for ICU4C 78.3, particularly the
// u_setMemoryFunctions, udata_setCommonData and u_cleanup API references and the
// ICU User Guide, "ICU Data" and "Design": https://unicode-org.github.io/icu/userguide/
// The startup order, warm-up set and fail-stop hook follow the L0 allocation-
// failure findings (docs/architecture/localization-l0.md); the implementation is
// original Ludus code. ICU4C is used under the Unicode License v3.

#include <ludus/foundation/unicode/unicode.hpp>

#include <ludus/foundation/base/core.h>

#include "internal/icu_host.hpp"

#include <atomic>
#include <cstring>

#include <unicode/localpointer.h>
#include <unicode/numberformatter.h>
#include <unicode/plurrule.h>
#include <unicode/uchar.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/ures.h>
#include <unicode/uversion.h>

#if defined(__cpp_exceptions)
#    error Ludus::Unicode and every ICU translation unit must compile without C++ exceptions
#endif
#if UCONFIG_NO_FILE_IO == 0 || UCONFIG_NO_MF2 == 0 || UCONFIG_NO_SERVICE == 0
#    error The pinned ICU profile registers embedded data only and excludes MF2 and ICU services
#endif

#ifndef LUDUS_UNICODE_PINNED_LABEL
#    error LUDUS_UNICODE_PINNED_LABEL must name the pinned linguistic data (cmake/IcuProfile.cmake)
#endif

namespace ludus::foundation::unicode
{
namespace detail
{
alignas(16) extern const uint8 ICU_DATA[];
extern const usize ICU_DATA_BYTES;
} // namespace detail

namespace
{
/// Every ICU block carries its size so reallocation and release can use the domain's sized API.
struct alignas(16) BlockHeader final
{
    usize Bytes{};
};

constexpr usize BLOCK_ALIGNMENT = alignof(BlockHeader);
constexpr usize MAX_LABEL_BYTES = 128;
constexpr std::string_view PINNED_LABEL = LUDUS_UNICODE_PINNED_LABEL;
constexpr const char* ALLOCATION_FAILURE =
    "ICU4C allocation failed; ICU 78.3 cannot recover from it (docs/architecture/localization-l0.md)";

struct HostState final
{
    const AllocationDomain* DomainOrNull = nullptr;
    std::atomic<bool> IsInitialized{false};
    std::atomic<usize> Users{0};
    std::atomic<uint64> Attempts{0};
    std::atomic<uint64> FailAt{0};
    std::atomic<uint64> Allocations{0};
    std::atomic<uint64> Frees{0};
    std::atomic<usize> LiveBlocks{0};
    std::atomic<usize> LiveBytes{0};
    std::atomic<usize> PeakBytes{0};
    char Label[MAX_LABEL_BYTES]{};
    usize LabelBytes{};
};

HostState& GetHost() noexcept
{
    static HostState host;
    return host;
}

void AddLiveBytes(usize bytes) noexcept
{
    HostState& host = GetHost();
    const usize live = host.LiveBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    usize peak = host.PeakBytes.load(std::memory_order_relaxed);
    while (live > peak && host.PeakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed) == false)
    {
    }
}

void RemoveLiveBytes(usize bytes) noexcept
{
    GetHost().LiveBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

/// Allocates one ICU block; failure is fatal because ICU cannot propagate it safely.
void* AllocateBlock(usize bytes) noexcept
{
    HostState& host = GetHost();
    const uint64 attempt = host.Attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64 failAt = host.FailAt.load(std::memory_order_relaxed);
    void* storageOrNull = nullptr;
    if ((failAt == 0 || attempt != failAt) && host.DomainOrNull != nullptr && bytes <= ~usize{0} - sizeof(BlockHeader))
    {
        storageOrNull = host.DomainOrNull->TryAllocate(sizeof(BlockHeader) + bytes, BLOCK_ALIGNMENT);
    }
    if (storageOrNull == nullptr)
    {
        LUDUS_FATAL(ALLOCATION_FAILURE);
    }
    BlockHeader* header = ::new (storageOrNull) BlockHeader{bytes};
    host.Allocations.fetch_add(1, std::memory_order_relaxed);
    host.LiveBlocks.fetch_add(1, std::memory_order_relaxed);
    AddLiveBytes(bytes);
    return header + 1;
}

void FreeBlock(void* blockOrNull) noexcept
{
    if (blockOrNull == nullptr)
    {
        return;
    }
    HostState& host = GetHost();
    BlockHeader* header = static_cast<BlockHeader*>(blockOrNull) - 1;
    const usize bytes = header->Bytes;
    host.Frees.fetch_add(1, std::memory_order_relaxed);
    host.LiveBlocks.fetch_sub(1, std::memory_order_relaxed);
    RemoveLiveBytes(bytes);
    host.DomainOrNull->Free(header, sizeof(BlockHeader) + bytes, BLOCK_ALIGNMENT);
}

void* U_CALLCONV Allocate(const void* /*context*/, usize bytes) noexcept
{
    return AllocateBlock(bytes);
}

void U_CALLCONV Free(const void* /*context*/, void* blockOrNull) noexcept
{
    FreeBlock(blockOrNull);
}

// ICU fixes the parameter order in UMemReallocFn; it cannot use distinct wrappers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* U_CALLCONV Reallocate(const void* /*context*/, void* blockOrNull, usize bytes) noexcept
{
    if (blockOrNull == nullptr)
    {
        return AllocateBlock(bytes);
    }
    // The domain has no resize operation: copy into a new block, then release the old one.
    const usize oldBytes = (static_cast<BlockHeader*>(blockOrNull) - 1)->Bytes;
    void* resized = AllocateBlock(bytes);
    std::memcpy(resized, blockOrNull, oldBytes < bytes ? oldBytes : bytes);
    FreeBlock(blockOrNull);
    return resized;
}

/// Appends text to the label buffer; false when it would exceed the buffer.
bool AppendLabel(HostState& inoutHost, std::string_view text) noexcept
{
    if (text.size() > MAX_LABEL_BYTES - inoutHost.LabelBytes)
    {
        return false;
    }
    std::memcpy(inoutHost.Label + inoutHost.LabelBytes, text.data(), text.size());
    inoutHost.LabelBytes += text.size();
    return true;
}

/// Spells the running data like localization_release.linguistic_label.
bool BuildLabel(HostState& inoutHost, UErrorCode& inoutStatus) noexcept
{
    char icuVersion[U_MAX_VERSION_STRING_LENGTH]{};
    char unicodeVersion[U_MAX_VERSION_STRING_LENGTH]{};
    UVersionInfo version{};
    u_getVersion(version);
    u_versionToString(version, icuVersion);
    u_getUnicodeVersion(version);
    u_versionToString(version, unicodeVersion);
    icu::LocalUResourceBundlePointer bundle(ures_openDirect(nullptr, "icuver", &inoutStatus));
    int32 length = 0;
    const UChar* cldr = ures_getStringByKey(bundle.getAlias(), "CLDRVersion", &length, &inoutStatus);
    if (U_FAILURE(inoutStatus) || cldr == nullptr || length <= 0 || length > 16)
    {
        return false;
    }
    char cldrVersion[16]{};
    for (int32 i = 0; i < length; ++i)
    {
        if (cldr[i] < u'!' || cldr[i] > u'~')
        {
            return false;
        }
        cldrVersion[i] = static_cast<char>(cldr[i]);
    }
    inoutHost.LabelBytes = 0;
    return AppendLabel(inoutHost, "icu4c-") && AppendLabel(inoutHost, icuVersion) && AppendLabel(inoutHost, "/cldr-") &&
           AppendLabel(inoutHost, {cldrVersion, static_cast<usize>(length)}) && AppendLabel(inoutHost, "/unicode-") &&
           AppendLabel(inoutHost, unicodeVersion);
}

// Attempts stay cumulative for the process so a fail point set before initialization keeps its meaning.
void ResetStatistics(HostState& inoutHost) noexcept
{
    inoutHost.Allocations.store(0, std::memory_order_relaxed);
    inoutHost.Frees.store(0, std::memory_order_relaxed);
    inoutHost.LiveBlocks.store(0, std::memory_order_relaxed);
    inoutHost.LiveBytes.store(0, std::memory_order_relaxed);
    inoutHost.PeakBytes.store(0, std::memory_order_relaxed);
}
} // namespace

UnicodeStatus InitializeUnicodeHost(const AllocationDomain& domain) noexcept
{
    HostState& host = GetHost();
    if (host.IsInitialized.load(std::memory_order_acquire) == true)
    {
        return UnicodeStatus::AlreadyInitialized;
    }
    host.DomainOrNull = &domain;
    ResetStatistics(host);
    UErrorCode status = U_ZERO_ERROR;
    // 1. Hooks first: ICU refuses them once it has allocated anything.
    u_setMemoryFunctions(nullptr, Allocate, Reallocate, Free, &status);
    // 2. Immutable embedded package; UCONFIG_NO_FILE_IO means ICU never opens files.
    udata_setCommonData(detail::ICU_DATA, &status);
    // 3. Warm the lazily created process singletons. A failure inside any of them
    // is undefined behavior in ICU 78.3, which the fatal hook turns into fail-stop.
    // Format::Format() copies Locale::getRoot() (static locale cache).
    static_cast<void>(icu::Locale::getRoot());
    // UnifiedCache: plural rules are the cheapest public path that creates it.
    // Explicit ID: the default Locale constructor reads the host environment.
    const icu::LocalPointer<icu::PluralRules> rules(icu::PluralRules::forLocale(icu::Locale("und"), status));
    // Number-skeleton tables.
    static_cast<void>(icu::number::NumberFormatter::forSkeleton(icu::UnicodeString(u"precision-integer"), status));
    const bool isLabelled = U_SUCCESS(status) && BuildLabel(host, status) == true;
    if (isLabelled == false || std::string_view(host.Label, host.LabelBytes) != PINNED_LABEL)
    {
        u_cleanup();
        host.DomainOrNull = nullptr;
        host.LabelBytes = 0;
        return isLabelled == false ? UnicodeStatus::BackendFailure : UnicodeStatus::DataMismatch;
    }
    host.IsInitialized.store(true, std::memory_order_release);
    return UnicodeStatus::Ok;
}

UnicodeStatus ShutdownUnicodeHost() noexcept
{
    HostState& host = GetHost();
    if (host.IsInitialized.load(std::memory_order_acquire) == false)
    {
        return UnicodeStatus::NotInitialized;
    }
    if (host.Users.load(std::memory_order_acquire) != 0)
    {
        LUDUS_FATAL("ShutdownUnicodeHost with live ICU users; destroy formatter contexts first");
    }
    host.IsInitialized.store(false, std::memory_order_release);
    host.LabelBytes = 0;
    // Releases every cache and clears the hooks and data registration. Blocks are
    // freed through the hooks before they are cleared, so the domain stays valid
    // until this returns.
    u_cleanup();
    host.FailAt.store(0, std::memory_order_relaxed);
    host.DomainOrNull = nullptr;
    return UnicodeStatus::Ok;
}

bool IsUnicodeHostInitialized() noexcept
{
    return GetHost().IsInitialized.load(std::memory_order_acquire);
}

std::string_view GetLinguisticDataLabel() noexcept
{
    const HostState& host = GetHost();
    if (host.IsInitialized.load(std::memory_order_acquire) == false)
    {
        return {};
    }
    return {host.Label, host.LabelBytes};
}

UnicodeAllocationStatistics GetUnicodeAllocationStatistics() noexcept
{
    const HostState& host = GetHost();
    return
    {
        .Allocations = host.Allocations.load(std::memory_order_relaxed),
        .Frees = host.Frees.load(std::memory_order_relaxed),
        .LiveBlocks = host.LiveBlocks.load(std::memory_order_relaxed),
        .LiveBytes = host.LiveBytes.load(std::memory_order_relaxed),
        .PeakBytes = host.PeakBytes.load(std::memory_order_relaxed),
    };
}

namespace detail
{
bool TryRetainHost() noexcept
{
    HostState& host = GetHost();
    if (host.IsInitialized.load(std::memory_order_acquire) == false)
    {
        return false;
    }
    host.Users.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void ReleaseHost() noexcept
{
    GetHost().Users.fetch_sub(1, std::memory_order_acq_rel);
}

UErrorCode MakeLocale(std::string_view tag, icu::Locale& outLocale) noexcept
{
    if (tag.size() > MAX_LOCALE_TAG_BYTES)
    {
        return U_ILLEGAL_ARGUMENT_ERROR;
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale locale =
        icu::Locale::forLanguageTag(icu::StringPiece(tag.data(), static_cast<int32>(tag.size())), status);
    if (U_FAILURE(status))
    {
        return status;
    }
    if (locale.isBogus() == true)
    {
        return U_ILLEGAL_ARGUMENT_ERROR;
    }
    outLocale = locale;
    return U_ZERO_ERROR;
}

icu::UnicodeString MakeUnicodeString(std::string_view utf8) noexcept
{
    return icu::UnicodeString::fromUTF8(icu::StringPiece(utf8.data(), static_cast<int32>(utf8.size())));
}

void SetAllocationFailPoint(uint64 attempt) noexcept
{
    HostState& host = GetHost();
    const uint64 current = host.Attempts.load(std::memory_order_relaxed);
    host.FailAt.store(attempt == 0 ? 0 : current + attempt, std::memory_order_relaxed);
}
} // namespace detail
} // namespace ludus::foundation::unicode
