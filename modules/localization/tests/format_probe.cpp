// L2 formatting evidence shared by native and wasm32 (LUDUS_BUILD_UNICODE builds):
// the corpus formatted from cooked packages, measured dynamic cost, and isolated
// ICU allocation fail points that must terminate (fail-stop). Driven by
// tests/test_format_probe.py; see docs/modules/localization.md#validation.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/diagnostic_output.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/unicode/unicode.hpp>
#include <ludus/localization/catalog.hpp>
#include <ludus/localization/formatter.hpp>

#include "fixture.hpp"
#include "internal/icu_host.hpp"

#include <cstdlib>
#include <span>
#include <string_view>

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::logging;
using namespace ludus::localization;
namespace host = ludus::foundation::unicode;
namespace corpus = localization_corpus;
namespace cases = localization_corpus_cases;

inline constexpr LogCategory LOG_L2{"LocalizationL2"};
inline constexpr usize OUTPUT_BYTES = 1024;
inline constexpr usize PREPARE_REPETITIONS = 200;
inline constexpr usize FORMAT_REPETITIONS = 2000;
inline constexpr usize COUNT_CYCLE = 100;

/// Phases a fail point can target; the driver sweeps every allocation of each.
enum class Phase : uint8
{
    HostInitialization,
    Prepare,
    Format,
};
inline constexpr usize PHASE_COUNT = 3;
inline constexpr StaticArray<std::string_view, PHASE_COUNT> PHASE_NAMES = {"host-initialization", "prepare", "format"};

bool Check(bool isPassed, std::string_view name) noexcept
{
    if (isPassed == true)
    {
        LUDUS_LOG_INFO(LOG_L2, "L2 PASS {}", name);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_L2, "L2 FAIL {}", name);
    }
    return isPassed;
}

uint64 GetElapsed(time::Timestamp start) noexcept
{
    time::Duration elapsed;
    return time::TryElapsed(start, time::Now(), elapsed) == time::TimeStatus::Ok ? elapsed.Nanoseconds : 0;
}

uint64 GetAllocations() noexcept
{
    return host::GetUnicodeAllocationStatistics().Allocations;
}

std::span<const uint8> FindPackage(std::string_view locale) noexcept
{
    for (const cases::CorpusPackage& package : cases::kCorpusPackages)
    {
        if (package.Locale == locale)
        {
            return package.Bytes;
        }
    }
    return {};
}

/// The measured message: Arabic nested select/plural, as in the L0 cost record.
struct MeasuredMessage final
{
    Catalog Lease;
    MessageBinding Binding;
};

bool LoadMeasuredMessage(MeasuredMessage& outMessage) noexcept
{
    Diagnostic diagnostic;
    return PrepareCatalog(kCorpusAr, GetSystemAllocationDomain(), outMessage.Lease, diagnostic) == Status::Ok &&
           outMessage.Lease.BindMessage(corpus::kLootPickedUp, outMessage.Binding) == Status::Ok;
}

Status FormatMeasured(FormatterContext& inoutContext, MessageBinding binding, int64 count) noexcept
{
    char output[OUTPUT_BYTES];
    FormatResult result;
    return corpus::FormatLootPickedUp(inoutContext, binding, "masculine", count, output, result);
}

bool CheckCorpus() noexcept
{
    char output[OUTPUT_BYTES];
    uint64 digest = 0;
    usize formatted = 0;
    usize rejected = 0;
    bool isPassed = true;
    for (const cases::FormatCase& testCase : cases::kFormatCases)
    {
        Catalog lease;
        Diagnostic diagnostic;
        MessageBinding binding;
        FormatterContext context;
        FormatResult result;
        Status status = PrepareCatalog(FindPackage(testCase.Locale), GetSystemAllocationDomain(), lease, diagnostic);
        status = status == Status::Ok ? lease.BindMessage(*testCase.Descriptor, binding) : status;
        const MessageBinding bindings[] = {binding};
        status = status == Status::Ok ? PrepareFormatterContext(lease, bindings, GetSystemAllocationDomain(), context)
                                      : status;
        status = status == Status::Ok ? Format(context, binding, testCase.Arguments, output, result) : status;
        const std::string_view text(output, status == Status::Ok ? result.Written : 0);
        const bool isExpected = testCase.IsFormatted == true ? status == Status::Ok && text == testCase.Expected
                                                             : status == Status::InvalidArguments;
        if (isExpected == false)
        {
            LUDUS_LOG_ERROR(LOG_L2,
                            "L2 case mismatch: {} {} status={} actual=[{}] expected=[{}]",
                            testCase.Locale,
                            testCase.Descriptor->Key.Key,
                            static_cast<uint32>(status),
                            text,
                            testCase.Expected);
            isPassed = false;
        }
        // One digest over every status and output byte, printed identically by native and wasm32.
        const uint8 statusByte = static_cast<uint8>(status);
        digest = digest * 31 + TableHash64({&statusByte, 1});
        digest = digest * 31 + TableHash64({reinterpret_cast<const uint8*>(text.data()), text.size()});
        formatted += testCase.IsFormatted == true ? 1 : 0;
        rejected += testCase.IsFormatted == true ? 0 : 1;
    }
    LUDUS_LOG_INFO(LOG_L2,
                   "L2 METRIC corpus formatted={} rejected-arguments={} digest={}",
                   formatted,
                   rejected,
                   digest);
    return Check(isPassed && formatted == 109 && rejected == 16, "corpus-from-cooked-packages");
}

/// Cold, prepared-construction and reformat cost of the measured message. Diagnostics, not budgets.
bool Measure() noexcept
{
    MeasuredMessage message;
    bool isPassed = LoadMeasuredMessage(message);
    const MessageBinding bindings[] = {message.Binding};
    // Cold: the first preparation and format after host start fills ICU's caches.
    uint64 allocations = GetAllocations();
    time::Timestamp start = time::Now();
    FormatterContext context;
    isPassed = isPassed &&
               PrepareFormatterContext(message.Lease, bindings, GetSystemAllocationDomain(), context) == Status::Ok;
    isPassed = isPassed && FormatMeasured(context, message.Binding, 2) == Status::Ok;
    const uint64 coldNanoseconds = GetElapsed(start);
    const uint64 coldAllocations = GetAllocations() - allocations;

    allocations = GetAllocations();
    start = time::Now();
    for (usize i = 0; i < PREPARE_REPETITIONS; ++i)
    {
        FormatterContext prepared;
        isPassed =
            isPassed &&
            PrepareFormatterContext(message.Lease, bindings, GetSystemAllocationDomain(), prepared) == Status::Ok;
    }
    const uint64 prepareNanoseconds = GetElapsed(start) / PREPARE_REPETITIONS;
    const uint64 prepareAllocations = (GetAllocations() - allocations) / PREPARE_REPETITIONS;

    allocations = GetAllocations();
    start = time::Now();
    for (usize i = 0; i < FORMAT_REPETITIONS; ++i)
    {
        isPassed =
            isPassed && FormatMeasured(context, message.Binding, static_cast<int64>(i % COUNT_CYCLE + 1)) == Status::Ok;
    }
    const uint64 formatNanoseconds = GetElapsed(start) / FORMAT_REPETITIONS;
    const uint64 formatAllocations = (GetAllocations() - allocations) / FORMAT_REPETITIONS;

    // A static record copies cooked bytes without ICU.
    MessageBinding play;
    isPassed = isPassed && message.Lease.BindMessage(corpus::kMenuPlay, play) == Status::Ok;
    char output[OUTPUT_BYTES];
    FormatResult result;
    allocations = GetAllocations();
    for (usize i = 0; i < FORMAT_REPETITIONS; ++i)
    {
        isPassed = isPassed && Format(context, play, {}, output, result) == Status::Ok;
    }
    const uint64 staticAllocations = GetAllocations() - allocations;
    LUDUS_LOG_INFO(LOG_L2,
                   "L2 METRIC dynamic-format cold-ns={} cold-allocations={} prepare-ns={} prepare-allocations={} "
                   "reformat-ns={} reformat-allocations={} static-allocations={} peak-icu-bytes={}",
                   coldNanoseconds,
                   coldAllocations,
                   prepareNanoseconds,
                   prepareAllocations,
                   formatNanoseconds,
                   formatAllocations,
                   staticAllocations,
                   host::GetUnicodeAllocationStatistics().PeakBytes);
    return Check(isPassed && staticAllocations == 0, "dynamic-format-measurements");
}

bool Run() noexcept
{
    uint64 allocations = GetAllocations();
    const time::Timestamp start = time::Now();
    bool isPassed = Check(host::InitializeUnicodeHost(GetSystemAllocationDomain()) == host::UnicodeStatus::Ok,
                          "host-initialization");
    LUDUS_LOG_INFO(LOG_L2,
                   "L2 METRIC host-initialization ns={} allocations={} linguistic-data={}",
                   GetElapsed(start),
                   GetAllocations() - allocations,
                   host::GetLinguisticDataLabel());
    isPassed = isPassed && Measure();
    isPassed = CheckCorpus() && isPassed;
    allocations = GetAllocations();
    isPassed = host::ShutdownUnicodeHost() == host::UnicodeStatus::Ok && isPassed;
    return Check(host::GetUnicodeAllocationStatistics().LiveBlocks == 0, "host-cleanup-releases-every-allocation") &&
           isPassed;
}

/// Prints the allocation attempts of each phase so the driver can sweep all of them.
bool CountPoints() noexcept
{
    uint64 allocations = GetAllocations();
    bool isPassed = host::InitializeUnicodeHost(GetSystemAllocationDomain()) == host::UnicodeStatus::Ok;
    StaticArray<uint64, PHASE_COUNT> counts{};
    counts[0] = GetAllocations() - allocations;
    {
        MeasuredMessage message;
        isPassed = isPassed && LoadMeasuredMessage(message);
        const MessageBinding bindings[] = {message.Binding};
        FormatterContext context;
        allocations = GetAllocations();
        isPassed = isPassed &&
                   PrepareFormatterContext(message.Lease, bindings, GetSystemAllocationDomain(), context) == Status::Ok;
        counts[1] = GetAllocations() - allocations;
        allocations = GetAllocations();
        isPassed = isPassed && FormatMeasured(context, message.Binding, 2) == Status::Ok;
        counts[2] = GetAllocations() - allocations;
    }
    isPassed = host::ShutdownUnicodeHost() == host::UnicodeStatus::Ok && isPassed;
    for (usize i = 0; i < PHASE_COUNT; ++i)
    {
        LUDUS_LOG_INFO(LOG_L2, "L2 POINTS phase={} allocations={}", PHASE_NAMES[i], counts[i]);
    }
    return isPassed;
}

/// Denies the Nth ICU allocation of phase; returning at all violates fail-stop.
int32 RunFailPoint(Phase phase, uint64 failAt) noexcept
{
    if (phase == Phase::HostInitialization)
    {
        host::detail::SetAllocationFailPoint(failAt);
    }
    if (host::InitializeUnicodeHost(GetSystemAllocationDomain()) != host::UnicodeStatus::Ok)
    {
        return 4;
    }
    MeasuredMessage message;
    if (LoadMeasuredMessage(message) == false)
    {
        return 4;
    }
    const MessageBinding bindings[] = {message.Binding};
    FormatterContext context;
    if (phase == Phase::Prepare)
    {
        host::detail::SetAllocationFailPoint(failAt);
    }
    const Status prepared = PrepareFormatterContext(message.Lease, bindings, GetSystemAllocationDomain(), context);
    if (phase == Phase::Format)
    {
        host::detail::SetAllocationFailPoint(failAt);
    }
    const Status formatted = prepared == Status::Ok ? FormatMeasured(context, message.Binding, 2) : prepared;
    LUDUS_LOG_ERROR(LOG_L2,
                    "L2 FAILPOINT SURVIVED phase={} fail-at={} prepare={} format={}",
                    PHASE_NAMES[static_cast<usize>(phase)],
                    failAt,
                    static_cast<uint32>(prepared),
                    static_cast<uint32>(formatted));
    return 3;
}

bool ParseCount(std::string_view text, uint64& outValue) noexcept
{
    constexpr usize MAX_DIGITS = 9;
    uint64 value = 0;
    if (text.empty() == true || text.size() > MAX_DIGITS)
    {
        return false;
    }
    for (char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        value = value * 10 + static_cast<uint64>(digit - '0');
    }
    outValue = value;
    return true;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    // Native fatal reports travel only through a configured emergency socket; the
    // driver passes one so it can attribute each termination. Browsers use console.error.
    const char* descriptorOrNull = std::getenv("LUDUS_FORMAT_PROBE_DIAGNOSTIC_FD");
    uint64 descriptor = 0;
    if (descriptorOrNull != nullptr && (ParseCount(descriptorOrNull, descriptor) == false ||
                                        diagnostics::ConfigureEmergencySocket(static_cast<int>(descriptor)) == false))
    {
        return 2;
    }
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    if (LogSystem::Initialize(config).Status != LogStatus::Ok)
    {
        return 2;
    }
    int32 result = 2;
    uint64 phase = 0;
    uint64 failAt = 0;
    if (argumentCount == 1)
    {
        result = Run() == true ? 0 : 1;
    }
    else if (argumentCount == 2 && std::string_view(arguments[1]) == "count-points")
    {
        result = CountPoints() == true ? 0 : 1;
    }
    else if (argumentCount == 4 && std::string_view(arguments[1]) == "fail-point" &&
             ParseCount(arguments[2], phase) == true && phase < PHASE_COUNT &&
             ParseCount(arguments[3], failAt) == true && failAt != 0)
    {
        result = RunFailPoint(static_cast<Phase>(phase), failAt);
    }
    LogSystem::Shutdown();
    return result;
}
