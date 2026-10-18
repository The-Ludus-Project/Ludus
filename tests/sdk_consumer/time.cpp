#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>

int ExerciseInstalledTime() noexcept
{
    using namespace ludus::foundation::time;
    const auto before = Now();
    const auto after = Now();
    Duration elapsed;
    Stopwatch timer;
    Deadline deadline;
    FrameClock frame{Duration{250'000'000}};
    timer.Start(before);
    if (TryElapsed(before, after, elapsed) != TimeStatus::Ok || timer.Pause(after) != TimeStatus::Ok ||
        timer.Read(after, elapsed) != TimeStatus::Ok || deadline.Arm(after, {}) != TimeStatus::Ok ||
        deadline.IsExpired(after) == false || frame.Sample(before).IsBaseline == false ||
        frame.Sample(after).Status != TimeStatus::Ok)
    {
        return 8;
    }
    return 0;
}
