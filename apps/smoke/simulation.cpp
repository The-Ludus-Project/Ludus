#include "internal/simulation.h"
namespace ludus::smoke
{
namespace
{
using namespace foundation;
float64 Bound(float64 value) noexcept
{
    return value < -1 ? -1 : (value > 1 ? 1 : value);
}
} // namespace
time::FrameSample SampleFrame(time::FrameClock& inoutClock, time::Timestamp now, bool isVisible) noexcept
{
    if (isVisible == false)
    {
        inoutClock.Reset();
        return {};
    }
    return inoutClock.Sample(now);
}
void Advance(Simulation& inoutSimulation,
             const platform::browser::WindowState& input,
             const time::FrameSample& frame) noexcept
{
    if (input.Visible == false)
    {
        return;
    }
    const float64 delta = time::ToSeconds(frame.Elapsed);
    inoutSimulation.Phase += delta;
    if (inoutSimulation.Phase >= 2)
    {
        inoutSimulation.Phase -= 2;
    }
    if (input.Focused == false)
    {
        return;
    }
    using platform::browser::Key;
    const auto held = [&input](Key key) noexcept { return input.Keys[static_cast<foundation::usize>(key)]; };
    inoutSimulation.X = Bound(inoutSimulation.X + delta * ((held(Key::KeyD) || held(Key::ArrowRight) ? 1 : 0) -
                                                           (held(Key::KeyA) || held(Key::ArrowLeft) ? 1 : 0)));
    inoutSimulation.Y = Bound(inoutSimulation.Y + delta * ((held(Key::KeyW) || held(Key::ArrowUp) ? 1 : 0) -
                                                           (held(Key::KeyS) || held(Key::ArrowDown) ? 1 : 0)));
    if ((input.Buttons & 1U) != 0 && input.CssWidth > 0 && input.CssHeight > 0)
    {
        inoutSimulation.X = Bound(input.PointerX * 2 / input.CssWidth - 1);
        inoutSimulation.Y = Bound(1 - input.PointerY * 2 / input.CssHeight);
    }
}
} // namespace ludus::smoke
