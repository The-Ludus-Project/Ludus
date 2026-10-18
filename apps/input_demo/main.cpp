// Keyboard input demonstration (M5). Shows the production loop wiring:
//   construct InputSystem + window, attach a keyboard sink, define a
//   configurable Jump / MoveX / MoveY / Quit map, drive ConsumeStep from the
//   window pump, and read held movement, action values, and tap edges.
//
// Gameplay defaults (the key choices below) live HERE in the application, not in
// the engine core. The demo also shows one in-memory rebind and a bounded
// diagnostic trace dump through the existing logger.
//
// Headless/no-compositor friendly: if the window never grants keyboard focus
// (e.g. headless CI, or a compositor that does not focus a buffer-less surface),
// the demo falls back to a scripted SYNTHETIC input sequence so it still
// demonstrates held input, tap edges, focus cancellation, and rebinding end to
// end. Real native focus/keys take over whenever the compositor provides them.

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/input/actions.h>
#include <ludus/input/input_debug.h>
#include <ludus/input/keyboard.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/native_window.h>

#include <string>

using namespace ludus::foundation::logging;
using ludus::input::ActionId;
using ludus::input::ActionKind;
using ludus::input::ActionState;
using ludus::input::ActionStatus;
using ludus::input::AxisDirection;
using ludus::input::Binding;
using ludus::input::BindingMap;
using ludus::input::DebugLabel;
using ludus::input::FocusBaseline;
using ludus::input::InputDebugTrace;
using ludus::input::InputSystem;
using ludus::input::Key;
using ludus::input::KeyboardRecord;
using ludus::input::KeyTransition;
using ludus::input::RecordSource;
using ludus::input::ResetReason;

namespace
{
// Application-owned action identities and the default bindings. These are a
// DEMO policy, not an engine default.
constexpr ActionId ACTION_JUMP = 0;
constexpr ActionId ACTION_MOVE_X = 1;
constexpr ActionId ACTION_MOVE_Y = 2;
constexpr ActionId ACTION_QUIT = 3;

inline constexpr ludus::foundation::logging::LogCategory LOG_DEMO{"InputDemo"};

// Sink user-data: forward native records/resets into the InputSystem.
struct SinkContext
{
    InputSystem* system = nullptr;
    bool everFocused = false;
};

void onRecord(void* userData, const KeyboardRecord& record) noexcept
{
    auto* context = static_cast<SinkContext*>(userData);
    (void)context->system->Ingest(record);
}
void onReset(void* userData, ResetReason reason, const FocusBaseline& baseline) noexcept
{
    auto* context = static_cast<SinkContext*>(userData);
    if (reason == ResetReason::FocusEntered && baseline.Focused)
    {
        context->everFocused = true;
    }
    context->system->RequestReset(reason, baseline);
}

KeyboardRecord synthDown(Key key) noexcept
{
    return KeyboardRecord{ .Source = RecordSource::Synthetic, .Transition = KeyTransition::Down, .PhysicalKey = key };
}
KeyboardRecord synthUp(Key key) noexcept
{
    return KeyboardRecord{ .Source = RecordSource::Synthetic, .Transition = KeyTransition::Up, .PhysicalKey = key };
}

// `label` is only consumed by LUDUS_LOG_INFO, which compiles out at log levels
// above Info (e.g. Release builds). Mark it maybe_unused so the demo stays
// warning-clean under -Werror at every compiled log level.
void logActions(InputSystem& system, [[maybe_unused]] const char* label)
{
    ActionState jump;
    ActionState moveX;
    ActionState moveY;
    (void)system.GetAction(ACTION_JUMP, jump);
    (void)system.GetAction(ACTION_MOVE_X, moveX);
    (void)system.GetAction(ACTION_MOVE_Y, moveY);
    LUDUS_LOG_INFO(LOG_DEMO,
                   "{}: MoveX={} MoveY={} Jump(v={} down={} up={} cancel={})",
                   label,
                   moveX.Value,
                   moveY.Value,
                   jump.Value,
                   jump.Pressed,
                   jump.Released,
                   jump.Cancelled);
}

// Scripted synthetic demonstration used when no native keyboard focus arrives.
// Returns after exercising held movement, a tap, focus cancellation, and rebind.
void runSyntheticDemo(InputSystem& system)
{
    LUDUS_LOG_INFO(LOG_DEMO, "No native keyboard focus; running scripted synthetic demonstration.");

    FocusBaseline focused;
    focused.Focused = true;
    system.RequestReset(ResetReason::FocusEntered, focused);
    ludus::foundation::uint64 step = 1;
    (void)system.ConsumeStep(step++);

    // Hold D (MoveX -> +1) across two steps: edge only on the first.
    (void)system.Ingest(synthDown(Key::KeyD));
    (void)system.ConsumeStep(step++);
    logActions(system, "hold D (press)");
    (void)system.ConsumeStep(step++);
    logActions(system, "hold D (held, no new edge)");

    // Tap Space (Jump) in one step: both edge flags, final inactive.
    (void)system.Ingest(synthDown(Key::Space));
    (void)system.Ingest(synthUp(Key::Space));
    (void)system.ConsumeStep(step++);
    logActions(system, "tap Space");

    // Simultaneous opposites on MoveX cancel to zero.
    (void)system.Ingest(synthDown(Key::KeyA));
    (void)system.ConsumeStep(step++);
    logActions(system, "A+D held -> MoveX 0");

    // Focus loss cancels everything (alt-tab while held).
    (void)system.RequestReset(ResetReason::FocusLost, FocusBaseline{});
    (void)system.ConsumeStep(step++);
    logActions(system, "focus lost (cancelled)");

    // Regain focus with D held: synchronized but suppressed until repress.
    FocusBaseline refocus;
    refocus.Focused = true;
    refocus.Keys[ludus::input::KeyIndex(Key::KeyD)] = true;
    system.RequestReset(ResetReason::FocusEntered, refocus);
    (void)system.ConsumeStep(step++);
    logActions(system, "refocus with D held (suppressed)");
    LUDUS_LOG_INFO(LOG_DEMO,
                   "  D physically down={} suppressed={}",
                   system.GetKeyboardSnapshot().Down[ludus::input::KeyIndex(Key::KeyD)],
                   system.GetKeyboardSnapshot().Suppressed[ludus::input::KeyIndex(Key::KeyD)]);

    // Release+repress D clears suppression and MoveX activates.
    (void)system.Ingest(synthUp(Key::KeyD));
    (void)system.Ingest(synthDown(Key::KeyD));
    (void)system.ConsumeStep(step++);
    logActions(system, "release+repress D -> MoveX +1");

    // In-memory rebind: MoveX positive now on ArrowRight; held D no longer moves.
    const Binding rebound[] = {
        Binding
        {
            .Action = ACTION_MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Positive,
            .PhysicalKey = Key::ArrowRight,
        },
    };
    (void)system.ReplaceBindings(BindingMap{ .Bindings = rebound });
    (void)system.ConsumeStep(step++);
    logActions(system, "after rebind MoveX+ -> ArrowRight (D suppressed/unbound)");
    (void)system.Ingest(synthDown(Key::ArrowRight));
    (void)system.ConsumeStep(step++);
    logActions(system, "press ArrowRight -> MoveX +1 (map v changed)");
    LUDUS_LOG_INFO(LOG_DEMO, "map version now {}", system.GetMapVersion());
}
} // namespace

int main()
{
    LogConfig config{};
    config.GlobalLevel = LogLevel::Info;
    config.EnableConsole = true;
    LogSystem::Initialize(config);
    SetCurrentThreadName("Main");

    InputSystem system;
    if (system.IsValid() == false)
    {
        LUDUS_LOG_FATAL(LOG_DEMO, "Failed to construct InputSystem");
        LogSystem::Shutdown();
        return 1;
    }

    // Opt-in bounded trace for the end-of-run diagnostic dump.
    InputDebugTrace trace;
    system.SetDebugTrace(&trace);

    // Configurable demo bindings (application policy, not engine policy):
    //   Jump   = Space OR KeyJ
    //   MoveX  = KeyD (+) / KeyA (-)
    //   MoveY  = KeyW (+) / KeyS (-)
    //   Quit   = Escape
    const Binding bindings[] = {
        Binding{ .Action = ACTION_JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
        Binding{ .Action = ACTION_JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyJ },
        Binding
        {
            .Action = ACTION_MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Positive,
            .PhysicalKey = Key::KeyD,
        },
        Binding
        {
            .Action = ACTION_MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Negative,
            .PhysicalKey = Key::KeyA,
        },
        Binding
        {
            .Action = ACTION_MOVE_Y,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Positive,
            .PhysicalKey = Key::KeyW,
        },
        Binding
        {
            .Action = ACTION_MOVE_Y,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Negative,
            .PhysicalKey = Key::KeyS,
        },
        Binding{ .Action = ACTION_QUIT, .Kind = ActionKind::Button, .PhysicalKey = Key::Escape },
    };
    if (system.ReplaceBindings(BindingMap{ .Bindings = bindings }) != ludus::input::BindingStatus::Ok)
    {
        LUDUS_LOG_FATAL(LOG_DEMO, "Failed to install demo bindings");
        LogSystem::Shutdown();
        return 1;
    }

    ludus::platform::WindowManager windowManager;
    if (windowManager.Initialize({}) == false)
    {
        // No windowing backend available: still demonstrate the reducer end to end.
        LUDUS_LOG_INFO(LOG_DEMO, "Window manager unavailable; synthetic demo only.");
        runSyntheticDemo(system);
    }
    else
    {
        const ludus::platform::Window::CreateInfo createInfo = { .Name = std::string("Ludus Input Demo") };
        ludus::foundation::core::UniquePtr<ludus::platform::Window> window;
        if (windowManager.CreateWindow(createInfo, window) == false || window.Get() == nullptr)
        {
            LUDUS_LOG_INFO(LOG_DEMO, "Window creation failed; synthetic demo only.");
            runSyntheticDemo(system);
        }
        else
        {
            SinkContext context{ .system = &system };
            const ludus::platform::KeyboardSink sink{ .OnRecord = onRecord, .OnReset = onReset, .UserData = &context };
            window->AttachKeyboardSink(sink);

            // Native loop: one ConsumeStep per outer iteration (one owner). A
            // headless window returns false immediately, so bound the loop.
            ludus::foundation::uint64 step = 1;
            int iterations = 0;
            bool quit = false;
            while (window->HandleEvent({}) && quit == false && iterations < 100000)
            {
                (void)system.ConsumeStep(step++);
                ActionState quitAction;
                if (system.GetAction(ACTION_QUIT, quitAction) == ActionStatus::Ok && quitAction.Pressed)
                {
                    LUDUS_LOG_INFO(LOG_DEMO, "Quit pressed; exiting demo loop.");
                    quit = true;
                }
                ++iterations;
            }
            window->DetachKeyboardSink();

            if (context.everFocused == false)
            {
                // The window never received keyboard focus (headless/CI). Run the
                // scripted synthetic demonstration so the behaviors are shown.
                runSyntheticDemo(system);
            }
        }
    }

    // Bounded diagnostic trace dump (NOT per-key logging by default). No disk
    // writes; uses the existing logger. These feed only LUDUS_LOG_INFO, which
    // compiles out above the Info level, so they are maybe_unused to stay
    // warning-clean under -Werror in Release.
    [[maybe_unused]] const auto view = trace.View();
    LUDUS_LOG_INFO(LOG_DEMO,
                   "Trace: {} entries recorded (total {}), wrapped={} complete-for-replay={}",
                   view.size(),
                   trace.GetTotalRecorded(),
                   trace.HasWrapped(),
                   trace.IsCompleteForReplay());
    [[maybe_unused]] const auto& counters = system.GetCounters();
    LUDUS_LOG_INFO(LOG_DEMO,
                   "Counters: accepted={} ignored={} rejected={} repeats={} overflows={} resets={}",
                   counters.Accepted,
                   counters.Ignored,
                   counters.Rejected,
                   counters.Repeats,
                   counters.Overflows,
                   counters.Resets);
    // Show the last few trace entries by physical label (bounded, not every key).
    const ludus::foundation::usize show = view.size() < 6 ? view.size() : 6;
    for (ludus::foundation::usize i = view.size() - show; i < view.size(); ++i)
    {
        LUDUS_LOG_INFO(LOG_DEMO,
                       "  trace[{}] key={} step={} seq={}",
                       i,
                       DebugLabel(view[i].PhysicalKey),
                       view[i].StepId,
                       view[i].Sequence);
    }

    system.SetDebugTrace(nullptr);
    LogSystem::Shutdown();
    return 0;
}
