#include "internal/scene_document.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
bool Less(const world_demo::Name& a, const world_demo::Name& b) noexcept
{
    for (foundation::usize i = 0; i < 32; ++i)
    {
        if (a.Text[i] != b.Text[i])
        {
            return a.Text[i] < b.Text[i];
        }
        if (a.Text[i] == 0)
        {
            return false;
        }
    }
    return false;
}
// Canonical game-owned ID order matches WriteLevel. Fixed-array insertion
// avoids allocation and makes save/reopen, draw order and picking agree.
void Canonicalize(world_demo::Level& level) noexcept
{
    for (foundation::usize i = 1; i < level.EntityCount; ++i)
    {
        const auto value = level.Entities[i];
        auto slot = i;
        while (slot > 0 && Less(value.Id, level.Entities[slot - 1].Id))
        {
            level.Entities[slot] = level.Entities[slot - 1];
            --slot;
        }
        level.Entities[slot] = value;
    }
    for (foundation::usize i = 1; i < level.CollisionCount; ++i)
    {
        const auto value = level.Collision[i];
        auto slot = i;
        while (slot > 0 && Less(value.Id, level.Collision[slot - 1].Id))
        {
            level.Collision[slot] = level.Collision[slot - 1];
            --slot;
        }
        level.Collision[slot] = value;
    }
}
bool Equal(world_demo::Vec2 a, world_demo::Vec2 b) noexcept
{
    return a.X == b.X && a.Y == b.Y;
}
bool Equal(const world_demo::Recipe& a, const world_demo::Recipe& b) noexcept
{
    return a.Id == b.Id && a.Type == b.Type && Equal(a.Position, b.Position) && a.Rotation == b.Rotation &&
           Equal(a.Scale, b.Scale) && a.Speed == b.Speed && a.Health == b.Health && a.Sprite == b.Sprite &&
           a.Target == b.Target && Equal(a.HalfExtent, b.HalfExtent) && a.NextLevel == b.NextLevel;
}
} // namespace
bool SceneSnapshot::operator==(const SceneSnapshot& other) const noexcept
{
    const auto& a = Level;
    const auto& b = other.Level;
    if ((a.Id == b.Id) == false || a.Seed != b.Seed || Equal(a.Minimum, b.Minimum) == false ||
        Equal(a.Maximum, b.Maximum) == false || Equal(a.Camera, b.Camera) == false ||
        a.VerticalExtent != b.VerticalExtent || a.CollisionCount != b.CollisionCount || a.EntityCount != b.EntityCount)
    {
        return false;
    }
    for (foundation::usize i = 0; i < a.CollisionCount; ++i)
    {
        if ((a.Collision[i].Id == b.Collision[i].Id) == false ||
            Equal(a.Collision[i].Center, b.Collision[i].Center) == false ||
            Equal(a.Collision[i].HalfExtent, b.Collision[i].HalfExtent) == false)
        {
            return false;
        }
    }
    for (foundation::usize i = 0; i < a.EntityCount; ++i)
    {
        if (Equal(a.Entities[i], b.Entities[i]) == false)
        {
            return false;
        }
    }
    return true;
}
world_demo::LevelResult SceneDocument::Load(const char* bytes, foundation::usize size) noexcept
{
    if (bytes == nullptr || size == 0)
    {
        return {world_demo::LevelError::Syntax, 0};
    }
    if (Identity_ == ~foundation::uint64{0})
    {
        return {world_demo::LevelError::Capacity, 0};
    }
    SceneSnapshot candidate;
    // Existing codec compatibility boundary; owned scene APIs accept bounded bytes.
    const auto result = world_demo::ReadLevel({bytes, size}, candidate.Level);
    if (result.Error != world_demo::LevelError::None)
    {
        return result;
    }
    Canonicalize(candidate.Level);
    Draft_ = Saved_ = candidate;
    ++Identity_;
    History_.Reset();
    Gesture_ = false;
    PreviewRevision_ = 0;
    Loaded_ = true;
    return {};
}
void SceneDocument::Clear() noexcept
{
    Loaded_ = false;
    Gesture_ = false;
    History_.Reset();
    if (Identity_ != ~foundation::uint64{0})
    {
        ++Identity_;
    }
}
SceneStamp SceneDocument::Stamp(foundation::uint64 camera, foundation::uint64 surface) const noexcept
{
    return {Identity_, History_.Revision(), camera, surface, PreviewRevision_};
}
foundation::usize SceneDocument::Find(const SceneSnapshot& snapshot, const world_demo::Name& id) noexcept
{
    for (foundation::usize i = 0; i < snapshot.Level.EntityCount; ++i)
    {
        if (snapshot.Level.Entities[i].Id == id)
        {
            return i;
        }
    }
    return snapshot.Level.EntityCount;
}
bool SceneDocument::Commit(const SceneSnapshot& candidate, SceneStamp expected) noexcept
{
    if (Loaded() == false || Gesture_ || expected.Document != Identity_ || expected.Revision != History_.Revision() ||
        world_demo::ValidateLevel(candidate.Level) != world_demo::LevelError::None)
    {
        return false;
    }
    auto canonical = candidate;
    Canonicalize(canonical.Level);
    if (History_.Commit(Draft_, canonical) == false)
    {
        return false;
    }
    Draft_ = canonical;
    return true;
}
bool SceneDocument::Undo() noexcept
{
    return Gesture_ == false && History_.Undo(Draft_);
}
bool SceneDocument::Redo() noexcept
{
    return Gesture_ == false && History_.Redo(Draft_);
}
bool SceneDocument::BeginTransform(const world_demo::Name* ids, foundation::usize count, SceneStamp expected) noexcept
{
    if (PreviewRevision_ == ~foundation::uint64{0} || Loaded() == false || Gesture_ || ids == nullptr || count == 0 ||
        count > 32 || expected.Document != Identity_ || expected.Revision != History_.Revision())
    {
        return false;
    }
    for (foundation::usize i = 0; i < count; ++i)
    {
        if (Find(Draft_, ids[i]) == Draft_.Level.EntityCount)
        {
            return false;
        }
        for (foundation::usize j = 0; j < i; ++j)
        {
            if (ids[i] == ids[j])
            {
                return false;
            }
        }
    }
    for (foundation::usize i = 0; i < count; ++i)
    {
        Targets_[i] = ids[i];
    }
    Preview_ = Draft_;
    TargetCount_ = count;
    GestureStamp_ = expected;
    Gesture_ = true;
    ++PreviewRevision_;
    return true;
}
bool SceneDocument::PreviewTransform(const SceneTransform* values, foundation::usize count) noexcept
{
    if (PreviewRevision_ == ~foundation::uint64{0} || Gesture_ == false || values == nullptr || count != TargetCount_)
    {
        return false;
    }
    auto candidate = Draft_;
    for (foundation::usize i = 0; i < count; ++i)
    {
        auto& entity = candidate.Level.Entities[Find(candidate, Targets_[i])];
        entity.Position = values[i].Position;
        entity.Rotation = values[i].Rotation;
        entity.Scale = values[i].Scale;
    }
    if (world_demo::ValidateLevel(candidate.Level) != world_demo::LevelError::None)
    {
        return false;
    }
    Preview_ = candidate;
    ++PreviewRevision_;
    return true;
}
void SceneDocument::CancelTransform() noexcept
{
    if (Gesture_ && PreviewRevision_ != ~foundation::uint64{0})
    {
        ++PreviewRevision_;
    }
    Gesture_ = false;
}
bool SceneDocument::AcceptTransform() noexcept
{
    if (Gesture_ == false)
    {
        return false;
    }
    CancelTransform();
    return Commit(Preview_, GestureStamp_);
}
bool SceneDocument::AcknowledgeSave(const SceneSnapshot& snapshot, foundation::uint64 identity) noexcept
{
    if (identity != Identity_ || Loaded() == false ||
        world_demo::ValidateLevel(snapshot.Level) != world_demo::LevelError::None)
    {
        return false;
    }
    Saved_ = snapshot;
    Canonicalize(Saved_.Level);
    History_.BreakGroup();
    return true;
}
bool SceneDocument::Restore(const SceneSnapshot& snapshot) noexcept
{
    return Commit(snapshot, Stamp(0, 0));
}
bool SceneDocument::Pick(world_demo::Vec2 point,
                         SceneStamp request,
                         SceneStamp current,
                         world_demo::Name& out) const noexcept
{
    namespace math = foundation::math;
    if (Loaded() == false || Gesture_ || (request == current) == false || request.Document != Identity_ ||
        request.Revision != History_.Revision() || request.Preview != PreviewRevision_ ||
        math::IsFinite(point.X) == false || math::IsFinite(point.Y) == false)
    {
        return false;
    }
    for (auto i = Draft_.Level.EntityCount; i > 0; --i)
    {
        const auto& e = Draft_.Level.Entities[i - 1];
        const auto dx = point.X - e.Position.X;
        const auto dy = point.Y - e.Position.Y;
        const auto c = math::Cos(e.Rotation);
        const auto s = math::Sin(e.Rotation);
        if (math::Abs(c * dx + s * dy) < e.HalfExtent.X * e.Scale.X &&
            math::Abs(-s * dx + c * dy) < e.HalfExtent.Y * e.Scale.Y)
        {
            out = e.Id;
            return true;
        }
    }
    out = {};
    return true;
}
} // namespace ludus::editor
