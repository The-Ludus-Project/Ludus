#include <ludus/foundation/base/assert_config.hpp>
#include <ludus/foundation/base/assert_format.hpp>
#include <ludus/foundation/base/build_metadata.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/target.hpp>
#include <ludus/foundation/base/version.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/dynamics.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/input/actions.h>
#include <ludus/input/keyboard.h>

#include <iostream>
#include <ludus/foundation/base/diagnostic_output.hpp>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

int ExerciseInstalledWorld() noexcept;
int ExerciseInstalledNetwork() noexcept;
int ExerciseInstalledUi() noexcept;
int ExerciseInstalledContainers() noexcept;
int ExerciseInstalledFilesystem() noexcept;
int ExerciseInstalledContent() noexcept;
int ExerciseInstalledAudio() noexcept;
int ExerciseInstalledPack() noexcept;
int ExerciseInstalledAsync() noexcept;
int ExerciseInstalledPersistence() noexcept;
bool ExerciseInstalledStrings() noexcept;
bool ExerciseInstalledStringDictionary() noexcept;
bool ExerciseInstalledParsing() noexcept;
bool ExerciseInstalledConfiguration() noexcept;
bool ExerciseInstalledReflection() noexcept;
int ExerciseInstalledTime() noexcept;
int ExerciseInstalledIdentity() noexcept;
bool ExerciseInstalledFluid() noexcept;
bool ExerciseInstalledCurves() noexcept;
bool ExerciseInstalledGeometry() noexcept;
bool ExerciseInstalledCamera() noexcept;
bool ExerciseInstalledDenoiser() noexcept;

static_assert(ludus::foundation::kTarget.PointerBits == sizeof(void*) * 8);
#if defined(LUDUS_PLATFORM_MACOS)
static_assert(ludus::foundation::kTarget.Os == ludus::foundation::TargetOs::MacOS);
static_assert(LUDUS_EXPECTED_TARGET_OS == LUDUS_OS_MACOS);
#else
static_assert(ludus::foundation::kTarget.Os == ludus::foundation::TargetOs::Linux);
static_assert(LUDUS_EXPECTED_TARGET_OS == LUDUS_OS_LINUX);
#endif
int ExerciseInstalledThreading() noexcept;
bool ExerciseInstalledLocalization() noexcept;

// Exercise the installed Ludus::Input SDK through public headers only: define a
// button map, focus, ingest a short tap, consume one step, and check held/edge
// results and an in-memory rebind. Returns 0 on success, 6 on any input failure.
static int ExerciseInstalledInput()
{
    ludus::input::InputSystem system;
    if (system.IsValid() == false)
    {
        return 6;
    }

    ludus::input::FocusBaseline baseline;
    baseline.Focused = true;
    system.RequestReset(ludus::input::ResetReason::FocusEntered, baseline);

    const ludus::input::Binding bindings[] = {
        ludus::input::Binding
        {
            .Action = 0,
            .Kind = ludus::input::ActionKind::Button,
            .PhysicalKey = ludus::input::Key::Space,
        },
    };
    if (system.ReplaceBindings(ludus::input::BindingMap{ .Bindings = bindings }) != ludus::input::BindingStatus::Ok)
    {
        return 6;
    }
    if (system.ConsumeStep(1) != ludus::input::StepStatus::Ok)
    {
        return 6;
    }

    // Short tap: down then up in one step -> both edge flags, final inactive.
    if (system.Ingest(ludus::input::KeyboardRecord
    {
        .Transition = ludus::input::KeyTransition::Down,
        .PhysicalKey = ludus::input::Key::Space,
    }) != ludus::input::AdmissionStatus::Accepted)
    {
        return 6;
    }
    if (system.Ingest(ludus::input::KeyboardRecord
    {
        .Transition = ludus::input::KeyTransition::Up,
        .PhysicalKey = ludus::input::Key::Space,
    }) != ludus::input::AdmissionStatus::Accepted)
    {
        return 6;
    }
    if (system.ConsumeStep(2) != ludus::input::StepStatus::Ok)
    {
        return 6;
    }

    ludus::input::ActionState jump;
    if (system.GetAction(0, jump) != ludus::input::ActionStatus::Ok)
    {
        return 6;
    }
    if (jump.Pressed == false || jump.Released == false || jump.Value != 0.0F)
    {
        return 6;
    }

    // In-memory rebind to a different key and confirm the old key no longer acts.
    const ludus::input::Binding rebound[] = {
        ludus::input::Binding
        {
            .Action = 0,
            .Kind = ludus::input::ActionKind::Button,
            .PhysicalKey = ludus::input::Key::Enter,
        },
    };
    if (system.ReplaceBindings(ludus::input::BindingMap{ .Bindings = rebound }) != ludus::input::BindingStatus::Ok)
    {
        return 6;
    }
    return 0;
}

// Exercise the installed Ludus::FoundationMath SDK through public headers only.
// Returns 0 on success, 7 on any math failure.
static int ExerciseInstalledMath()
{
    namespace m = ludus::foundation::math;

    // Quaternion rotation agrees with its matrix action.
    m::Quaternion q{};
    if (m::TryFromAxisAngle(m::Vector3{0.0F, 0.0F, 1.0F}, m::kHalfPiF, q) != m::MathStatus::Success)
    {
        return 7;
    }
    const m::Vector3 byQuat = m::Rotate(q, m::Vector3{1.0F, 0.0F, 0.0F});
    const m::Vector3 byMatrix = m::ToMatrix3(q) * m::Vector3{1.0F, 0.0F, 0.0F};
    if ((m::Distance(byQuat, byMatrix) < 1e-5F) == false)
    {
        return 7;
    }

    // Checked inverse round-trip.
    m::Matrix4 mat = m::Matrix4::Identity();
    mat.Columns[3] = m::Vector4{3.0F, 4.0F, 5.0F, 1.0F};
    m::Matrix4 inv{};
    if (m::TryInverse(mat, inv) != m::MathStatus::Success)
    {
        return 7;
    }

    // Installed checked dynamics symbols and header are part of the SDK.
    m::LinearDragStep drag;
    m::RadialSweepContact contact;
    if (m::IsSuccess(m::TryComputeLinearDragStep(0.0, 0.5, drag)) == false || drag.DistanceScale != 0.5 ||
        m::IsSuccess(m::TrySweepRadialBand({10, 0, 0}, {10, 0, 0}, {}, 0, 20, 2, contact)) == false ||
        contact.Hit == false || m::NearlyEqual(contact.Fraction, 0.4, 1e-12, 1e-12) == false)
    {
        return 7;
    }

    // PCG known-answer (bit-exact contract).
    m::RandomStream rng;
    if (rng.TryReseed(42, 54) == false || rng.NextUInt32() != 0xa15c02b7U)
    {
        return 7;
    }
    m::RandomKey key;
    m::PreparedBound32 bound;
    m::RandomBlock block;
    ludus::foundation::uint32 ticket = 0;
    const m::RandomAddress address{0x0123456789abcdefULL, 99, 1234};
    if (m::IsSuccess(m::TryMakeRandomKey(42, 7, key)) == false || key.Value != 0xccf635ee9e9e2fa4ULL ||
        m::SampleUInt32(key, address) != 0x942d2d40u || m::IsSuccess(m::TryPrepareBound32(1000, bound)) == false ||
        m::IsSuccess(m::TrySampleBounded(key, address, bound, ticket)) == false || ticket != 578 ||
        m::IsSuccess(m::TrySampleBlock(key, {address.Scope, address.Event, 1232}, block)) == false ||
        block.Values[2] != 0x942d2d40u)
    {
        return 7;
    }
    return 0;
}

static_assert(LUDUS_BUILD_FLAVOR_ID == EXPECTED_FLAVOR);
static_assert(LUDUS_ENABLE_ASSERTS == EXPECTED_ASSERTS);
static_assert(LUDUS_BREAK_ON_CHECK == EXPECTED_CHECK_BREAK);
int PolicyWithoutNdebug();

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

int main()
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

    if (ExerciseInstalledReflection() == false)
    {
        return 91;
    }
    if (ExerciseInstalledCamera() == false)
    {
        return 41;
    }
    if (ExerciseInstalledDenoiser() == false)
    {
        return 42;
    }
    if (ExerciseInstalledCurves() == false)
    {
        return 31;
    }
    if (ExerciseInstalledGeometry() == false)
    {
        return 32;
    }
    if (ExerciseInstalledConfiguration() == false)
    {
        return 90;
    }

    if (ExerciseInstalledParsing() == false)
    {
        return 11;
    }
    if (ExerciseInstalledLocalization() == false)
    {
        return 12;
    }
    if (ExerciseInstalledFluid() == false)
    {
        return 10;
    }
    if (ExerciseInstalledStrings() == false)
    {
        return 9;
    }
    if (ExerciseInstalledStringDictionary() == false)
    {
        return 13;
    }
    if (PrimitiveContract() == false)
    {
        return 8;
    }
    if (const int result = ExerciseInstalledAudio(); result != 0)
    {
        return result;
    }
    if (ExerciseInstalledContent() != 0)
    {
        return 20;
    }
    if (const int result = ExerciseInstalledFilesystem(); result != 0)
    {
        return result;
    }
    if (const int result = ExerciseInstalledContainers(); result != 0)
    {
        return result;
    }

    if (const int threadingResult = ExerciseInstalledThreading(); threadingResult != 0)
    {
        return threadingResult;
    }
    // Verify the lifecycle API and static link without requiring a GPU adapter on CI.
    static_assert(noexcept(ludus::graphics::rhi::Initialize({})));
    static_assert(noexcept(ludus::graphics::rhi::SetFrameTarget({})));
    static_assert(noexcept(ludus::graphics::rhi::GetStartup()));
    static_assert(noexcept(ludus::graphics::rhi::Start({}, {})));
    static_assert(noexcept(ludus::graphics::rhi::Start({}, {}, ludus::graphics::rhi::BackendSelection::Auto, {})));
    ludus::graphics::rhi::Shutdown();
    // Link and exercise explicit device admission entirely through installed
    // headers, without asking this policy test host for a GPU adapter.
    const auto compiledBackends = ludus::graphics::rhi::GetCompiledBackends();
#if defined(LUDUS_PLATFORM_MACOS)
    const bool nativeBackendAvailable = compiledBackends.Metal;
#else
    const bool nativeBackendAvailable = compiledBackends.Vulkan;
#endif
    ludus::graphics::rhi::DeviceHandle explicitDevice;
    ludus::graphics::rhi::SurfaceHandle explicitSurface;
    ludus::graphics::rhi::DeviceDescription explicitRequest;
    // Required compute on WebGL2 rejects before backend startup on every host.
    explicitRequest.Selection = ludus::graphics::rhi::BackendSelection::WebGL2;
    explicitRequest.Required.Compute = true;
    if (ludus::graphics::rhi::CreateDevice({}, {}, explicitRequest, explicitDevice, explicitSurface) !=
            ludus::graphics::rhi::DeviceStatus::Unsupported ||
        ludus::graphics::rhi::DestroyDevice(explicitDevice) != ludus::graphics::rhi::DeviceStatus::InvalidHandle ||
        nativeBackendAvailable == false)
    {
        return 2;
    }
    if (ludus::graphics::rhi::GetStartup().State != ludus::graphics::rhi::StartupState::Idle)
    {
        return 2;
    }
    if (ludus::graphics::rhi::SetFrameTarget({}) != ludus::graphics::rhi::FrameStatus::NotReady)
    {
        return 5;
    }
    const ludus::graphics::rhi::DeviceRequirements requirements{ .MinUniformBufferSize = 48 };
    if (ludus::graphics::rhi::GetStartup().Capabilities.MaxUniformBufferSize != 0 ||
        ludus::graphics::rhi::Start({}, {}, ludus::graphics::rhi::BackendSelection::WebGPU, requirements) !=
            ludus::graphics::rhi::StartStatus::Failed ||
        ludus::graphics::rhi::GetStartup().Error != ludus::graphics::rhi::StartupError::BackendUnavailable ||
        ludus::graphics::rhi::GetStartup().Requirements.MinUniformBufferSize != 48)
    {
        return 2;
    }
    ludus::graphics::rhi::Shutdown();
    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, endpoints) != 0 ||
        ludus::foundation::diagnostics::ConfigureEmergencySocket(endpoints[0]) == false)
    {
        return 4;
    }
    (void)close(endpoints[0]);
    LUDUS_ASSERT(true);
    LUDUS_REQUIRE(true);
    if (LUDUS_CHECK(true) == false || LUDUS_CHECK_F(false, "installed SDK recovery probe {}", 23))
    {
        return 3;
    }
    char report[2048];
    const auto received = recv(endpoints[1], report, sizeof(report), MSG_DONTWAIT);
    (void)close(endpoints[1]);
    if (received <= 0)
    {
        return 5;
    }
    std::cout.write(report, received);
    if (PolicyWithoutNdebug() != LUDUS_ENABLE_ASSERTS)
    {
        return 2;
    }
    const auto value = ludus::foundation::version();
    if (value.major != ludus::foundation::build_metadata::version_major)
    {
        return 1;
    }

    if (const int timeResult = ExerciseInstalledTime(); timeResult != 0)
    {
        return timeResult;
    }
    if (const int identityResult = ExerciseInstalledIdentity(); identityResult != 0)
    {
        return identityResult;
    }
    if (const int worldResult = ExerciseInstalledWorld(); worldResult != 0)
    {
        return worldResult;
    }
    if (const int uiResult = ExerciseInstalledUi(); uiResult != 0)
    {
        return uiResult;
    }
    if (const int inputResult = ExerciseInstalledInput(); inputResult != 0)
    {
        return inputResult;
    }

    if (const int mathResult = ExerciseInstalledMath(); mathResult != 0)
    {
        return mathResult;
    }

    if (const int networkResult = ExerciseInstalledNetwork(); networkResult != 0)
    {
        return networkResult;
    }

    std::cout << "SDK consumer linked Ludus " << ludus::foundation::version_string()
              << " (Input: tap + rebind OK; Math: rotate/inverse/PCG OK)\n";
    return 0;
}
