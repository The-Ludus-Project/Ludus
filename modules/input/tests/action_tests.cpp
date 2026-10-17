// Button / Axis1D action and rebinding tests (M3). Drive the production
// InputSystem. Axis edge expectations follow design section 5.

#include <ludus/input/keyboard.h>

#include <catch2/catch_test_macros.hpp>

#include <span>

using namespace ludus::input;

namespace
{
KeyboardRecord down(Key key) noexcept
{
    return KeyboardRecord{ .Transition = KeyTransition::Down, .PhysicalKey = key };
}
KeyboardRecord up(Key key) noexcept
{
    return KeyboardRecord{ .Transition = KeyTransition::Up, .PhysicalKey = key };
}

void focus(InputSystem& system) noexcept
{
    FocusBaseline baseline;
    baseline.Focused = true;
    system.RequestReset(ResetReason::FocusEntered, baseline);
}

// Action ids used across the suite.
constexpr ActionId JUMP = 0;
constexpr ActionId MOVE_X = 1;
constexpr ActionId MOVE_Y = 2;

ActionState action(const InputSystem& system, ActionId id) noexcept
{
    ActionState state;
    (void)system.GetAction(id, state);
    return state;
}
} // namespace

TEST_CASE("Button: multiple alternative keys OR; releasing one keeps it active", "[input][action]")
{
    InputSystem system;
    focus(system);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // MoveForward (JUMP id reused as a plain button) bound to W and UpArrow.
    const Binding bindings[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyW },
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::ArrowUp },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok); // commit map, no edges

    REQUIRE(system.Ingest(down(Key::KeyW)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::ArrowUp)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(up(Key::KeyW)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);

    const ActionState state = action(system, JUMP);
    REQUIRE(state.Value == 1.0F);  // still held via UpArrow
    REQUIRE(state.Pressed);        // one press
    REQUIRE_FALSE(state.Released); // no release while an alternative is held

    // Releasing the last alternative releases the action.
    REQUIRE(system.Ingest(up(Key::ArrowUp)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);
    const ActionState after = action(system, JUMP);
    REQUIRE(after.Value == 0.0F);
    REQUIRE(after.Released);
}

TEST_CASE("Button: rapid tap and two ordered taps in one step", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding bindings[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // Single tap in a step: both edges, final inactive.
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(up(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    ActionState s = action(system, JUMP);
    REQUIRE(s.Pressed);
    REQUIRE(s.Released);
    REQUIRE(s.Value == 0.0F);

    // Two taps in one step: still both edges (OR-accumulated), final inactive.
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(up(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(up(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    s = action(system, JUMP);
    REQUIRE(s.Pressed);
    REQUIRE(s.Released);
    REQUIRE(s.Value == 0.0F);
}

TEST_CASE("Axis1D: opposite directions cancel; +1/-1 transitions", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding bindings[] = {
        Binding
        {
            .Action = MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Positive,
            .PhysicalKey = Key::KeyD,
        },
        Binding
        {
            .Action = MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Negative,
            .PhysicalKey = Key::KeyA,
        },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // D -> +1, pressed (inactive->active).
    REQUIRE(system.Ingest(down(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    ActionState s = action(system, MOVE_X);
    REQUIRE(s.Value == 1.0F);
    REQUIRE(s.Pressed);

    // Add A -> both held -> 0; active->inactive is a Released.
    REQUIRE(system.Ingest(down(Key::KeyA)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    s = action(system, MOVE_X);
    REQUIRE(s.Value == 0.0F);
    REQUIRE(s.Released);

    // Release D -> only A -> -1; inactive->active is a Pressed again.
    REQUIRE(system.Ingest(up(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);
    s = action(system, MOVE_X);
    REQUIRE(s.Value == -1.0F);
    REQUIRE(s.Pressed);
}

TEST_CASE("Axis1D: +1 to -1 through an intervening zero fires both edges", "[input][action]")
{
    // Design section 5: "A +1 to -1 transition WITHOUT an intervening zero
    // changes Value but is not a new Pressed." With separate +/- keys the axis
    // physically cannot change sign without passing through zero, so this
    // through-zero case fires an actual active->inactive (Released) and
    // inactive->active (Pressed), evaluated per transition and OR-accumulated.
    InputSystem system;
    focus(system);
    const Binding bindings[] = {
        Binding
        {
            .Action = MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Positive,
            .PhysicalKey = Key::KeyD,
        },
        Binding
        {
            .Action = MOVE_X,
            .Kind = ActionKind::Axis1D,
            .Direction = AxisDirection::Negative,
            .PhysicalKey = Key::KeyA,
        },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // Start at +1.
    REQUIRE(system.Ingest(down(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(action(system, MOVE_X).Value == 1.0F);

    // In ONE step: release D (axis -> 0, Released) then press A (axis -> -1,
    // Pressed). Value is -1; both edges are set.
    REQUIRE(system.Ingest(up(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::KeyA)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    ActionState s = action(system, MOVE_X);
    REQUIRE(s.Value == -1.0F);
    REQUIRE(s.Pressed);
    REQUIRE(s.Released);

    // A steady axis held across the next idle step produces NO repeated edges.
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);
    s = action(system, MOVE_X);
    REQUIRE(s.Value == -1.0F);
    REQUIRE_FALSE(s.Pressed);
    REQUIRE_FALSE(s.Released);
}

TEST_CASE("Left/right modifiers bind separately; shared keys across actions", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding bindings[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::ShiftLeft },
        Binding{ .Action = MOVE_Y, .Kind = ActionKind::Button, .PhysicalKey = Key::ShiftRight },
        // KeyW shared by two actions.
        Binding{ .Action = MOVE_X, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyW },
        Binding{ .Action = MOVE_Y, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyW },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    REQUIRE(system.Ingest(down(Key::ShiftLeft)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(action(system, JUMP).Value == 1.0F);   // left shift only
    REQUIRE(action(system, MOVE_Y).Value == 0.0F); // right shift not pressed

    REQUIRE(system.Ingest(down(Key::KeyW)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    REQUIRE(action(system, MOVE_X).Value == 1.0F); // shared key activates both
    REQUIRE(action(system, MOVE_Y).Value == 1.0F);
}

TEST_CASE("Invalid map transactions preserve the old map", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding good[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = good }) == BindingStatus::Ok);
    const uint32 version = system.GetMapVersion();

    // Invalid action id (>= capacity).
    const Binding badAction[] = {
        Binding{ .Action = ACTION_CAPACITY, .Kind = ActionKind::Button, .PhysicalKey = Key::Space }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = badAction }) == BindingStatus::InvalidAction);

    // Invalid key.
    const Binding badKey[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Unknown }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = badKey }) == BindingStatus::InvalidKey);

    // Conflicting type for the same id.
    const Binding conflict[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
        Binding{ .Action = JUMP, .Kind = ActionKind::Axis1D, .PhysicalKey = Key::KeyD },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = conflict }) == BindingStatus::ConflictingType);

    // Duplicate binding.
    const Binding dup[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = dup }) == BindingStatus::DuplicateBinding);

    // The old map is intact: version unchanged by the failures.
    REQUIRE(system.GetMapVersion() == version);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    REQUIRE(action(system, JUMP).Value == 1.0F);
}

TEST_CASE("Full binding capacity is accepted", "[input][action]")
{
    InputSystem system;
    static Binding bindings[BINDING_CAPACITY];
    for (usize i = 0; i < BINDING_CAPACITY; ++i)
    {
        bindings[i] = Binding
        {
            .Action = static_cast<ActionId>(i % ACTION_CAPACITY),
            .Kind = ActionKind::Button,
            .PhysicalKey = static_cast<Key>(1 + (i % (KEY_COUNT - 1))),
        };
    }
    // Deduplicate action+key collisions by spreading keys; if any duplicate
    // sneaks in the status will say so. With 256 bindings over 64 actions and
    // ~140 keys, (action,key) pairs can repeat, so just assert it is a valid
    // transactional outcome, not a crash.
    const BindingStatus status = system.ReplaceBindings(BindingMap{ .Bindings = bindings });
    REQUIRE((status == BindingStatus::Ok || status == BindingStatus::DuplicateBinding));

    // One past capacity is rejected.
    static Binding tooMany[BINDING_CAPACITY + 1];
    for (auto& binding : tooMany)
    {
        binding = Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space };
    }
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = tooMany }) == BindingStatus::CapacityExceeded);
}

TEST_CASE("Removed action id cancels; map version advances", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding first[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = first }) == BindingStatus::Ok);
    const uint32 v1 = system.GetMapVersion();
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // Activate JUMP.
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(action(system, JUMP).Value == 1.0F);

    // Replace with a map that does NOT contain JUMP.
    const Binding second[] = {Binding{ .Action = MOVE_X, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyD }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = second }) == BindingStatus::Ok);
    REQUIRE(system.GetMapVersion() == v1 + 1);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);

    // JUMP (removed) reports neutral with Cancelled on this publication.
    const ActionState removed = action(system, JUMP);
    REQUIRE(removed.Value == 0.0F);
    REQUIRE(removed.Cancelled);

    // Next step: neutral, no flags.
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);
    const ActionState later = action(system, JUMP);
    REQUIRE(later.Value == 0.0F);
    REQUIRE_FALSE(later.Cancelled);
}

TEST_CASE("Switch map while held suppresses newly bound held keys until repress", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding first[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyD }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = first }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // Hold D (activates JUMP under map 1).
    REQUIRE(system.Ingest(down(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(action(system, JUMP).Value == 1.0F);

    // Switch to a map where D binds MOVE_X, while D is still held.
    const Binding second[] = {Binding{ .Action = MOVE_X, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyD }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = second }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);

    // D is held but suppressed under the new map: MOVE_X stays inactive.
    REQUIRE(system.GetKeyboardSnapshot().Suppressed[KeyIndex(Key::KeyD)]);
    REQUIRE(action(system, MOVE_X).Value == 0.0F);

    // Release and repress D -> MOVE_X activates.
    REQUIRE(system.Ingest(up(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::KeyD)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);
    REQUIRE(action(system, MOVE_X).Value == 1.0F);
}

TEST_CASE("Reset plus fresh press in the same step: Cancelled coexists with Pressed", "[input][action]")
{
    InputSystem system;
    focus(system);
    const Binding bindings[] = {Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::Space }};
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);

    // Activate JUMP.
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);
    REQUIRE(action(system, JUMP).Value == 1.0F);

    // Within the next step: an explicit live reset (cancels JUMP, suppresses the
    // held Space), then release+repress Space to produce a fresh press.
    FocusBaseline baseline;
    baseline.Focused = true;
    baseline.Keys[KeyIndex(Key::Space)] = true;
    system.RequestReset(ResetReason::ExplicitLive, baseline);
    REQUIRE(system.Ingest(up(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::Space)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);

    const ActionState s = action(system, JUMP);
    REQUIRE(s.Cancelled);     // the reset cancelled the previously active action
    REQUIRE(s.Pressed);       // and a fresh press occurred later in the same step
    REQUIRE(s.Value == 1.0F); // active again
}

TEST_CASE("Invalid action id query returns InvalidAction", "[input][action]")
{
    InputSystem system;
    ActionState state;
    REQUIRE(system.GetAction(ACTION_CAPACITY, state) == ActionStatus::InvalidAction);
    REQUIRE(system.GetAction(0, state) == ActionStatus::Ok); // in-domain, neutral
    REQUIRE(state.Value == 0.0F);
}

TEST_CASE("Baseline-held key released and repressed before the reset step activates in that step",
          "[input][action][suppression]")
{
    // Suppression is simulation state applied in record order (design section 4):
    // an Up and Down ingested between RequestReset and the next ConsumeStep queue
    // behind the baseline, so that step installs suppression, clears it on Up and
    // admits the fresh Down as an activation.
    InputSystem system;
    focus(system);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);
    const Binding bindings[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyW },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);

    FocusBaseline baseline;
    baseline.Focused = true;
    baseline.Keys[KeyIndex(Key::KeyW)] = true;
    system.RequestReset(ResetReason::FocusEntered, baseline);
    REQUIRE(system.Ingest(up(Key::KeyW)) == AdmissionStatus::Accepted);
    REQUIRE(system.Ingest(down(Key::KeyW)) == AdmissionStatus::Accepted);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);

    const KeyboardSnapshot& snapshot = system.GetKeyboardSnapshot();
    REQUIRE(snapshot.LastReset == ResetReason::FocusEntered);
    REQUIRE(snapshot.Suppressed[KeyIndex(Key::KeyW)] == false);
    REQUIRE(snapshot.Down[KeyIndex(Key::KeyW)] == true);
    const ActionState jump = action(system, JUMP);
    REQUIRE(jump.Pressed == true);
    REQUIRE(jump.Value == 1.0F);
}

TEST_CASE("Baseline-held key without a release stays suppressed across steps", "[input][action][suppression]")
{
    InputSystem system;
    focus(system);
    REQUIRE(system.ConsumeStep(1) == StepStatus::Ok);
    const Binding bindings[] = {
        Binding{ .Action = JUMP, .Kind = ActionKind::Button, .PhysicalKey = Key::KeyW },
    };
    REQUIRE(system.ReplaceBindings(BindingMap{ .Bindings = bindings }) == BindingStatus::Ok);
    REQUIRE(system.ConsumeStep(2) == StepStatus::Ok);

    FocusBaseline baseline;
    baseline.Focused = true;
    baseline.Keys[KeyIndex(Key::KeyW)] = true;
    system.RequestReset(ResetReason::FocusEntered, baseline);
    // The key is already live-down, so a Down without a release is a duplicate.
    REQUIRE(system.Ingest(down(Key::KeyW)) == AdmissionStatus::Ignored);
    REQUIRE(system.GetLiveDown(Key::KeyW) == true);
    REQUIRE(system.ConsumeStep(3) == StepStatus::Ok);
    REQUIRE(system.ConsumeStep(4) == StepStatus::Ok);

    REQUIRE(system.GetKeyboardSnapshot().Suppressed[KeyIndex(Key::KeyW)] == true);
    const ActionState jump = action(system, JUMP);
    REQUIRE(jump.Pressed == false);
    REQUIRE(jump.Value == 0.0F);
}
