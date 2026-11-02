#include <ludus/gameplay/sequencer/sequence.hpp>

bool ExerciseInstalledSequencer() noexcept
{
    using namespace ludus::gameplay::sequencer;
    const SequencePositionKey position{0, {1.0e12 + 0.25, 2, 3}};
    const SequenceCameraTrack cameraTrack{ .BindingId = 1, .PositionKeys = {&position, 1} };
    PreparedSequence sequence;
    if (TryPrepareSequence({ .Cameras = {&cameraTrack, 1} }, {}, sequence) != SequenceStatus::Success)
    {
        return false;
    }
    SequenceTime time;
    if (TryGetSequenceFrameTime({ .Rate = {30000, 1001}, .TicksPerSecond = 24, .StartTick = 0 }, 1, time) !=
            SequenceStatus::Success ||
        time.Tick != 0 || time.Numerator != 1001 || time.Denominator != 1250)
    {
        return false;
    }
    ludus::gameplay::camera::CameraSample sample;
    return TryEvaluateSequenceCamera(sequence, 1, time, sample) == SequenceStatus::Success &&
           sequence.GetCameraCount() == 1 && sample.Pose.Position.X == position.PositionWorld.X &&
           sample.Pose.Position.Y == 2 && sample.Pose.Position.Z == 3;
}
