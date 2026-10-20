#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/assert_format.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>
#include <ludus/foundation/base/target.hpp>
#include <ludus/foundation/base/version.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <span>
#include <string_view>

#include <emscripten.h>

bool ExerciseInstalledStrings() noexcept;
int ExerciseInstalledContainers() noexcept;
int ExerciseInstalledPack() noexcept;
int ExerciseInstalledAsync() noexcept;
int ExerciseInstalledPersistence() noexcept;

using namespace ludus::foundation;

static_assert(LUDUS_PLATFORM_WEB == 1);
static_assert(LUDUS_ARCH_WASM32 == 1);
static_assert(kTarget.Os == TargetOs::Web);
static_assert(kTarget.Arch == TargetArch::Wasm32);
static_assert(kTarget.PointerBits == 32);
static_assert(kTarget.Endian == TargetEndian::Little);
static_assert(LUDUS_ASSERT_DIALOGS_AVAILABLE == 0);
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_DESKTOP) || defined(__cpp_exceptions)
#    error "Browser Base must not inherit native platform or exception policy"
#endif

namespace
{
// clang-format off
EM_JS(void, failConsole, (), {
    Module.savedConsoleError = console.error;
    console.error = function () { throw new Error('controlled console failure'); };
});
EM_JS(void, restoreConsole, (), {
    console.error = Module.savedConsoleError;
});
// clang-format on

bool transportContract() noexcept
{
    using namespace diagnostics;
    char frame[CONTROL_HEADER_SIZE]{};
    ControlHeader header{};
    return ConfigureEmergencySocket(-1) == false && ControlEndpointState() == ControlState::Unconfigured &&
           ConfigureControlEndpoint(-1) == ControlState::Failed &&
           RequestAssertDecision(1, nullptr, 0) == ControlDecision::Terminate &&
           TryWriteEmergencyBytes(nullptr, 1) == DeliveryStatus::Failed &&
           TryWriteEmergencyBytes("", CONTROL_MAX_PAYLOAD + 1) == DeliveryStatus::Failed &&
           EncodeControlHeader(frame, sizeof(frame), ControlMessageType::Hello, 0, 0) == CONTROL_HEADER_SIZE &&
           DecodeControlHeader(frame, sizeof(frame), header) && header.Length == 0;
}

void reportPassed() noexcept
{
    constexpr char passed[] = "[W1:after] probe returned normally";
    (void)diagnostics::WriteEmergencyBytes(passed, sizeof(passed) - 1);
}
} // namespace

constexpr bool PrimitiveContract(ludus::foundation::usize increment = 1) noexcept
{
    using namespace ludus::foundation;
    static_assert(sizeof(usize) == sizeof(void*));
    if (increment == 0 || increment > 255)
    {
        return false;
    }
    const uint32 word = 0x12345600 | static_cast<uint32>(increment);
    usize size = 42;
    uint32 length = 42;
    int64 signedProduct = 42;
    uint8 bytes[4]{};
    return TryAdd(~usize{0}, increment, size) == false && size == 42 &&
           TryMultiply(~usize{0}, increment + 1, size) == false && size == 42 &&
           TryMultiply(-int64{9223372036854775807} - 1, static_cast<int64>(increment) + 1, signedProduct) == false &&
           signedProduct == 42 && TryIntegerCast(int32{-1}, length) == false && length == 42 &&
           TryWriteLittleEndian(word, bytes) && bytes[0] == increment && bytes[3] == 0x12 &&
           TryReadLittleEndian(bytes, length) && length == word &&
           TryReadBigEndian(std::span<const uint8>{bytes, 3}, length) == false && length == word;
}
static_assert(PrimitiveContract());

int main(int argc, char** argv)
{
    if (const int result = ExerciseInstalledPersistence(); result != 0)
    {
        return result;
    }
    if (const int result = ExerciseInstalledAsync(); result != 0)
    {
        return result;
    }
    if (const int result = ExerciseInstalledPack(); result != 0)
    {
        return result;
    }
    if (const int result = ExerciseInstalledContainers(); result != 0)
    {
        return result;
    }
    if (ExerciseInstalledStrings() == false)
    {
        return 9;
    }
    if (PrimitiveContract(static_cast<usize>(argc)) == false)
    {
        return 8;
    }
    const std::string_view mode = argc > 1 ? argv[1] : "normal";
    filesystem::Directory directory;
    filesystem::File file;
    if (filesystem::ValidPath("audio/音.wav") == false || filesystem::ValidPath("../escape") == true ||
        directory.Open("assets").Code != filesystem::Status::NotFound ||
        directory.OpenRead("asset", file).Code != filesystem::Status::InvalidArgument ||
        file.ReadAt(0, {}).Outcome.Code != filesystem::Status::InvalidArgument ||
        file.Clone(file).Code != filesystem::Status::InvalidArgument || file.IsOpen() == true ||
        directory.IsOpen() == true)
    {
        return 8;
    }
    if (version_string().empty() || transportContract() == false)
    {
        return 2;
    }
    if (mode == "check")
    {
        int32 evaluations = 0;
        const bool checked = LUDUS_CHECK(++evaluations == 0, "browser check");
        if (checked || evaluations != 1)
        {
            return 3;
        }
        // A second report verifies the first CHECK released its ownership.
        if (LUDUS_CHECK_F(false, "value={} float={}", uint32{42}, float64{1.25}))
        {
            return 4;
        }
    }
    else if (mode == "console-failure")
    {
        failConsole();
        const auto delivered = diagnostics::TryWriteEmergencyBytes("probe", 5);
        restoreConsole();
        if (delivered != diagnostics::DeliveryStatus::Failed)
        {
            return 7;
        }
    }
    else if (mode == "assert")
    {
        int32 evaluations = 0;
        LUDUS_ASSERT(++evaluations == 0, "browser assert");
        if (evaluations != 0) // Only disabled ASSERT may reach this line.
        {
            return 5;
        }
    }
    else if (mode == "require")
    {
        LUDUS_REQUIRE(false, "browser require");
    }
    else if (mode == "fatal")
    {
        LUDUS_FATAL("browser fatal");
    }
    else if (mode == "trap")
    {
        LUDUS_DEBUG_BREAK();
    }
    else if (mode != "normal")
    {
        return 6;
    }
    reportPassed();
    return 0;
}
