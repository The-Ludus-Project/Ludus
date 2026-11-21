#include "player_renderer.h"
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/gameplay/camera/control.hpp>
#include <ludus/gameplay/weather/weather_simulation.h>
#include <ludus/platform/base/window.h>

#include <signal.h>

namespace
{
using namespace ludus;
using namespace foundation;
using namespace first_person;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
namespace camera = gameplay::camera;
LUDUS_DEFINE_LOG_CATEGORY(LOG_PLAYER, "FirstPersonPlayer");

volatile sig_atomic_t gShouldStop = 0;

// The supervisor requests a normal main-thread shutdown. The handler only latches it.
void RequestStop(int) noexcept
{
    gShouldStop = 1;
}

bool Load(std::string_view path, PlayableSnapshot& outSnapshot) noexcept
{
    const usize slash = path.rfind('/');
    const std::string_view root = slash == path.npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    filesystem::Directory directory;
    filesystem::File file;
    Array<uint8> bytes;
    if (directory.Open(root).Succeeded() == false ||
        directory.OpenRead(path.substr(slash + 1), file).Succeeded() == false || file.Size() == 0 ||
        file.Size() > MAX_PLAYABLE_SNAPSHOT_BYTES || bytes.TryResize(static_cast<usize>(file.Size())) == false)
    {
        return false;
    }
    const filesystem::ReadResult read = file.ReadAt(0, {bytes.GetData(), bytes.GetSize()});
    return read.Outcome.Succeeded() == true && read.BytesRead == bytes.GetSize() &&
           DecodePlayableSnapshot(bytes.AsView(), outSnapshot) == true;
}
struct Controls final
{
    StaticArray<bool, static_cast<usize>(input::Key::Count)> IsDown{};
    bool ShouldJump = false;
    bool ShouldCapture = false;
    bool ShouldRelease = false;
};
void Record(void* context, const input::KeyboardRecord& record) noexcept
{
    Controls& controls = *static_cast<Controls*>(context);
    const usize index = static_cast<usize>(record.PhysicalKey);
    if (record.Repeat == true || index >= controls.IsDown.GetSize())
    {
        return;
    }
    const bool isPressed = record.Transition == input::KeyTransition::Down;
    controls.IsDown[index] = isPressed;
    if (isPressed == true)
    {
        controls.ShouldJump |= record.PhysicalKey == input::Key::Space;
        controls.ShouldCapture |= record.PhysicalKey == input::Key::Enter;
        controls.ShouldRelease |= record.PhysicalKey == input::Key::Escape;
    }
}
void ResetControls(void* context, input::ResetReason, const input::FocusBaseline&) noexcept
{
    *static_cast<Controls*>(context) = {};
}
// The keyboard sink borrows Controls, including on a failed frame or a cancelled launch.
struct InputAttachment final
{
    platform::Window& Window;

    ~InputAttachment() noexcept
    {
        Window.DetachKeyboardSink();
        Window.ReleaseRelativePointer();
    }
};

bool Play(const PlayableSnapshot& snapshot,
          const PlayerSpawn& spawn,
          WalkingController& inoutWalk,
          PlayerRenderer& inoutRenderer,
          platform::Window& inoutWindow,
          rhi::DeviceHandle device,
          rhi::SurfaceHandle surface,
          bool shouldVerifyFixture) noexcept
{
    PlayerScene scene;
    if (scene.Build(snapshot) == false || inoutRenderer.Open(device) == false)
    {
        return false;
    }
    const math::Vector3 directionWorld = math::Rotate(spawn.Pose.Orientation, {0, 0, -1});
    camera::CameraControlView aim{
        math::Atan2(-directionWorld.X, -directionWorld.Z),
        math::Atan2(directionWorld.Y,
                    math::Sqrt(directionWorld.X * directionWorld.X + directionWorld.Z * directionWorld.Z))};
    math::Vector3d eyeWorld = spawn.Pose.Position;
    Controls controls;
    const InputAttachment inputAttachment{inoutWindow};
    uint32 width = 0;
    uint32 height = 0;
    uint32 frames = 0;
    bool hasPresented = false;
    uint64 previous = time::NowTicks();
    while (gShouldStop == 0 && inoutWindow.HandleEvent({}) == true)
    {
        const platform::NativeWindowInfo native = inoutWindow.GetNativeWindowInfo();
        const uint64 now = time::NowTicks();
        constexpr float64 FIXTURE_TICK_SECONDS = 1.0 / 60;
        const float64 elapsed = shouldVerifyFixture == true ? FIXTURE_TICK_SECONDS
                                                            : static_cast<float64>(now - previous) /
                                                                  static_cast<float64>(time::NANOSECONDS_PER_SECOND);
        previous = now;
        if (native.Width == 0 || native.Height == 0)
        {
            threading::SleepFor(time::Milliseconds(1));
            continue;
        }
        const bool isResized = width != native.Width || height != native.Height;
        if (isResized == true)
        {
            width = native.Width;
            height = native.Height;
            if (rhi::SetFrameTarget(device,
                                    surface,
                                    {
                                        .Width = width,
                                        .Height = height,
                                    }) != rhi::DeviceStatus::Ready ||
                inoutRenderer.Resize(width, height) == false)
            {
                LUDUS_LOG_ERROR(LOG_PLAYER,
                                "Cannot prepare the Metal drawable. Reopen Play in a Metal-capable window.");
                return false;
            }
        }
        if (controls.ShouldCapture == true)
        {
            if (native.System == platform::WindowSystem::Cocoa && inoutWindow.TryCaptureRelativePointer() == false)
            {
                LUDUS_LOG_INFO(LOG_PLAYER, "Focus the game window and press Enter to capture the pointer.");
            }
            controls.ShouldCapture = false;
        }
        if (controls.ShouldRelease == true)
        {
            inoutWindow.ReleaseRelativePointer();
            controls.ShouldRelease = false;
        }
        input::RelativePointerMotion motion = inoutWindow.TakeRelativePointerMotion();
        constexpr uint32 FIXTURE_WALK_FRAMES = 60;
        constexpr uint32 FIXTURE_JUMP_FRAME = 2;
        constexpr uint32 FIXTURE_LOOK_FRAME = FIXTURE_WALK_FRAMES * 2;
        if (shouldVerifyFixture == true)
        {
            controls.IsDown[static_cast<usize>(input::Key::KeyW)] = frames > 0 && frames < FIXTURE_WALK_FRAMES;
            controls.IsDown[static_cast<usize>(input::Key::KeyD)] = frames >= FIXTURE_WALK_FRAMES;
            controls.ShouldJump = frames == FIXTURE_JUMP_FRAME;
            motion.Horizontal = frames == FIXTURE_LOOK_FRAME ? 1 : 0;
        }
        const math::Vector3d priorEyeWorld = eyeWorld;
        if (camera::TryAdvanceControlView(aim, {{motion.Horizontal, motion.Vertical}, {}}, {}, elapsed, aim) !=
            camera::CameraStatus::Success)
        {
            return false;
        }
        math::Vector3 forwardWorld;
        if (camera::TryComputeMovementForward(aim, forwardWorld) != camera::CameraStatus::Success)
        {
            return false;
        }
        const auto axis = [&](input::Key positive, input::Key negative) {
            return static_cast<float32>(controls.IsDown[static_cast<usize>(positive)]) -
                   static_cast<float32>(controls.IsDown[static_cast<usize>(negative)]);
        };
        math::Vector3 movementWorld = forwardWorld * axis(input::Key::KeyW, input::Key::KeyS) +
                                      math::Cross(forwardWorld, {0, 1, 0}) * axis(input::Key::KeyD, input::Key::KeyA);
        movementWorld = movementWorld * (spawn.Speed / math::Max(1.0F, math::Length(movementWorld)));
        eyeWorld = inoutWalk.Advance({static_cast<float64>(movementWorld.X),
                                      static_cast<float64>(movementWorld.Y),
                                      static_cast<float64>(movementWorld.Z)},
                                     elapsed,
                                     controls.ShouldJump);
        controls.ShouldJump = false;
        if (isResized == true || hasPresented == false || eyeWorld != priorEyeWorld || motion.Horizontal != 0 ||
            motion.Vertical != 0)
        {
            rr::ViewDescription view;
            math::Quaternion orientation;
            if (camera::TryComputeControlOrientation(aim, orientation) != camera::CameraStatus::Success)
            {
                return false;
            }
            view.CameraForward = math::Rotate(orientation, {0, 0, -1});
            view.CameraPosition = {static_cast<float32>(eyeWorld.X),
                                   static_cast<float32>(eyeWorld.Y),
                                   static_cast<float32>(eyeWorld.Z)};
            math::Matrix4 worldToView;
            math::Matrix4 viewToClip;
            constexpr float32 NEAR_METRES = 0.05F;
            constexpr float32 FAR_METRES = 10000.0F;
            if (math::TryLookAt(view.CameraPosition,
                                view.CameraPosition + view.CameraForward,
                                math::Rotate(orientation, {0, 1, 0}),
                                worldToView) != math::MathStatus::Success ||
                math::TryPerspectiveReverseZ(snapshot.Scene.VerticalFov,
                                             static_cast<float32>(width) / static_cast<float32>(height),
                                             NEAR_METRES,
                                             FAR_METRES,
                                             viewToClip) != math::MathStatus::Success)
            {
                return false;
            }
            view.Depth = rr::DepthConvention::ReverseZ;
            view.WorldToClip = viewToClip * worldToView;
            view.Atmosphere = snapshot.Atmosphere;
            if (inoutRenderer.Update(scene, view) == false)
            {
                LUDUS_LOG_ERROR(
                    LOG_PLAYER,
                    "Raster rejected the prepared World. Check its materials and lights and prepare it again.");
                return false;
            }
        }
        const rhi::RasterStatus presented = inoutRenderer.Present(surface);
        if (presented == rhi::RasterStatus::NotReady)
        {
            threading::SleepFor(time::Milliseconds(1));
            continue;
        }
        if (presented != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (hasPresented == false)
        {
            hasPresented = true;
            inoutWindow.AttachKeyboardSink({Record, ResetControls, &controls});
            LUDUS_LOG_INFO(LOG_PLAYER, "World ready. WASD moves and Space jumps.");
            if (native.System == platform::WindowSystem::Cocoa)
            {
                LUDUS_LOG_INFO(LOG_PLAYER, "Press Enter for mouse look and Escape to release the pointer.");
            }
        }
        if (shouldVerifyFixture == true)
        {
            if (frames == 0 || frames == FIXTURE_LOOK_FRAME)
            {
                uint32 rgb = 0;
                const rr::RenderGraphReport report = inoutRenderer.GetReport();
                if (inoutRenderer.ReadFixturePixel(width / 2, height / 2, rgb) == false ||
                    report.UnrenderedWorldFeatures != 0)
                {
                    return false;
                }
                LUDUS_LOG_INFO(LOG_PLAYER,
                               "Fixture sample {} rgb {} textures {} shadow lights {}",
                               frames,
                               rgb,
                               report.Resources.CreatedTextures,
                               report.Resources.DirectionalShadowLights);
            }
            LUDUS_LOG_INFO(LOG_PLAYER,
                           "Fixture frame {} eye {} {} {} yaw {}",
                           frames,
                           eyeWorld.X,
                           eyeWorld.Y,
                           eyeWorld.Z,
                           aim.Yaw);
            if (frames == FIXTURE_LOOK_FRAME)
            {
                return true;
            }
        }
        if (shouldVerifyFixture == true)
        {
            ++frames;
        }
    }
    return shouldVerifyFixture == false;
}
const char* DescribeCollisionRepair(WalkingController::StartStatus status) noexcept
{
    using StartStatus = WalkingController::StartStatus;
    switch (status)
    {
        case StartStatus::NoGeometry:
            return "Add Static Collider to a visible World floor and prepare the World again.";
        case StartStatus::GeometryUnavailable:
            return "Reimport the collider meshes and prepare the World again.";
        case StartStatus::GeometryTooLarge:
            return "Remove Static Collider from surfaces the player never touches and prepare the World again.";
        case StartStatus::GeometryOutOfRange:
            return "Move the collider geometry closer to the World origin and prepare the World again.";
        case StartStatus::SpawnObstructed:
            return "Place the player's eye above the floor and clear of walls, then prepare the World again.";
        case StartStatus::SpawnOutOfRange:
            return "Move the player closer to the World origin and prepare the World again.";
        case StartStatus::Ready:
            return "";
    }
    return "";
}

/// Mount the World's cooked climate at Play's fixed tick rate and report the weather at the spawn. A World
/// without a climate needs nothing. The climate was admitted by the same loader rules as the Editor's cooker.
bool MountWeather(const PlayableSnapshot& snapshot,
                  const PlayerSpawn& spawn,
                  gameplay::weather::WeatherSimulation& outWeather) noexcept
{
    namespace weather = gameplay::weather;
    if (snapshot.Climate.IsEmpty() == true)
    {
        return true;
    }
    weather::WeatherSample sample;
    if (weather::WeatherSimulation::TryMount(snapshot.Climate,
                                             content::scene_format::PLAYABLE_WORLD_TICKS_PER_SECOND,
                                             0,
                                             outWeather) != weather::WeatherSimulationStatus::Success ||
        outWeather.TrySample(spawn.Pose.Position.X, spawn.Pose.Position.Z, sample) !=
            weather::WeatherSimulationStatus::Success)
    {
        LUDUS_LOG_ERROR(LOG_PLAYER,
                        "Cannot mount the World climate at spawn {}. Move the spawn inside the climate domain and "
                        "prepare the World again.",
                        spawn.Entity);
        return false;
    }
    const weather::WeatherConditions conditions = weather::ConvertWeatherChannels(sample.Channels);
    LUDUS_LOG_INFO(LOG_PLAYER,
                   "Weather at spawn: {} C, {} mm/h precipitation, {} cloud coverage.",
                   conditions.TemperatureCelsius,
                   conditions.PrecipitationMillimetresPerHour,
                   conditions.CloudCoverage);
    return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command line's .luplay path, then the spawn entity.
bool Run(std::string_view path, std::string_view entity, bool shouldVerifyFixture) noexcept
{
    PlayableSnapshot snapshot;
    if (Load(path, snapshot) == false)
    {
        LUDUS_LOG_ERROR(LOG_PLAYER, "Cannot read the cooked World. Prepare the World again and pass its .luplay file.");
        return false;
    }
    for (const PlayerSpawn& spawn : snapshot.Spawns)
    {
        if (spawn.Entity != entity)
        {
            continue;
        }
        WalkingController walk;
        const WalkingController::StartStatus status = StartWalking(snapshot, spawn, walk);
        if (status != WalkingController::StartStatus::Ready)
        {
            LUDUS_LOG_ERROR(LOG_PLAYER, "Cannot start at spawn {}. {}", entity, DescribeCollisionRepair(status));
            return false;
        }
        gameplay::weather::WeatherSimulation weather;
        if (MountWeather(snapshot, spawn, weather) == false)
        {
            return false;
        }
        if (gShouldStop != 0)
        {
            return true;
        }
        platform::WindowManager manager;
        core::UniquePtr<platform::Window> window;
        constexpr uint32 WINDOW_WIDTH = 640;
        constexpr uint32 WINDOW_HEIGHT = 480;
        if (manager.Initialize({}) == false || manager.CreateWindow(
                                                   {
                                                       .Name = "Ludus First Person",
                                                       .Width = WINDOW_WIDTH,
                                                       .Height = WINDOW_HEIGHT,
                                                       .ShouldReceiveHostedRelativePointer = true,
                                                   },
                                                   window) == false)
        {
            LUDUS_LOG_ERROR(LOG_PLAYER, "Could not open the game window. Start Play from a macOS desktop session.");
            return false;
        }
        rhi::DeviceHandle device;
        rhi::SurfaceHandle surface;
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        bool isSuccessful = false;
        if (rhi::CreateDevice({ .Name = "First Person", .Version = 1 },
                              window->GetNativeWindowInfo(),
                              description,
                              device,
                              surface) == rhi::DeviceStatus::Ready)
        {
            PlayerRenderer renderer;
            isSuccessful = Play(snapshot, spawn, walk, renderer, *window, device, surface, shouldVerifyFixture);
        }
        rhi::Shutdown();
        if (isSuccessful == false)
        {
            LUDUS_LOG_ERROR(LOG_PLAYER,
                            "Play could not finish. Check the Metal device and the cooked World, then try again.");
        }
        return isSuccessful;
    }
    LUDUS_LOG_ERROR(LOG_PLAYER,
                    "Spawn {} is missing. Choose an existing First Person Player and prepare the World again.",
                    entity);
    return false;
}
} // namespace
int main(int argc, char** argv)
{
    struct sigaction stopAction{};
    stopAction.sa_handler = RequestStop;
    if (sigemptyset(&stopAction.sa_mask) != 0 || sigaction(SIGTERM, &stopAction, nullptr) != 0)
    {
        return 1;
    }
    logging::LogConfig config;
    config.EnableConsole = true;
    config.EnableFile = false;
    logging::LogSystem::Initialize(config);
    constexpr int REQUIRED_ARGUMENTS = 3;
    const bool shouldVerifyFixture =
        argc == REQUIRED_ARGUMENTS + 1 && std::string_view(argv[REQUIRED_ARGUMENTS]) == "--verify-fixture";
    bool isSuccessful = false;
    if (argc == REQUIRED_ARGUMENTS || shouldVerifyFixture == true)
    {
        isSuccessful = Run(argv[1], argv[2], shouldVerifyFixture);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_PLAYER, "Usage: ludus_first_person_player <world.luplay> <spawn-entity>");
    }
    logging::LogSystem::Shutdown();
    return isSuccessful == true ? 0 : 1;
}
