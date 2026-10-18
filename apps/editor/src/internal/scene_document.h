#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/document_history.h"
#include "internal/level.h"

namespace ludus::editor
{
// The first adapter owns the world-demo schema, not engine-wide entity kinds.
// No Qt, window, GPU, runtime handle, allocation or filesystem ownership here.
struct SceneSnapshot final
{
    world_demo::Level Level{};
    [[nodiscard]] bool operator==(const SceneSnapshot& other) const noexcept;
};
struct SceneStamp final
{
    foundation::uint64 Document = 0;
    foundation::uint64 Revision = 0;
    foundation::uint64 Camera = 0;
    foundation::uint64 Surface = 0;
    foundation::uint64 Preview = 0;
    [[nodiscard]] bool operator==(const SceneStamp&) const noexcept = default;
};
struct SceneTransform final
{
    world_demo::Vec2 Position{};
    foundation::float32 Rotation = 0;
    world_demo::Vec2 Scale{1, 1};
};
// Fixed capacity history (32 whole snapshots, <512 KiB). Snapshot copies cannot
// fail. Load and commit validate a candidate before publishing anything.
class SceneDocument final
{
public:
    [[nodiscard]] world_demo::LevelResult Load(const char* bytes, foundation::usize size) noexcept;
    [[nodiscard]] bool Loaded() const noexcept
    {
        return Loaded_;
    }
    void Clear() noexcept;
    [[nodiscard]] SceneStamp Stamp(foundation::uint64 camera, foundation::uint64 surface) const noexcept;
    [[nodiscard]] const SceneSnapshot& Draft() const noexcept
    {
        return Draft_;
    }
    [[nodiscard]] const SceneSnapshot& Visible() const noexcept
    {
        return Gesture_ ? Preview_ : Draft_;
    }
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Loaded() && (Draft_ == Saved_) == false;
    }
    [[nodiscard]] bool Previewing() const noexcept
    {
        return Gesture_;
    }
    [[nodiscard]] bool CanUndo() const noexcept
    {
        return Gesture_ == false && History_.CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return Gesture_ == false && History_.CanRedo();
    }
    [[nodiscard]] bool Undo() noexcept;
    [[nodiscard]] bool Redo() noexcept;
    [[nodiscard]] bool Commit(const SceneSnapshot& candidate, SceneStamp expected) noexcept;
    [[nodiscard]] bool
    BeginTransform(const world_demo::Name* ids, foundation::usize count, SceneStamp expected) noexcept;
    [[nodiscard]] bool PreviewTransform(const SceneTransform* values, foundation::usize count) noexcept;
    [[nodiscard]] bool AcceptTransform() noexcept;
    void CancelTransform() noexcept;
    // Acknowledges exactly the validated saved snapshot, even if a newer edit
    // arrived while the host wrote it. Identity mismatch preserves saved state.
    [[nodiscard]] bool AcknowledgeSave(const SceneSnapshot& snapshot, foundation::uint64 identity) noexcept;
    [[nodiscard]] bool Restore(const SceneSnapshot& snapshot) noexcept;
    // CPU picking uses the same rotated/scaled rectangles and draw order as
    // the shader. Reject obsolete document/camera/surface before touching out.
    [[nodiscard]] bool
    Pick(world_demo::Vec2 point, SceneStamp request, SceneStamp current, world_demo::Name& out) const noexcept;
    [[nodiscard]] static foundation::usize Find(const SceneSnapshot& snapshot, const world_demo::Name& id) noexcept;

private:
    SceneSnapshot Draft_{};
    SceneSnapshot Saved_{};
    SceneSnapshot Preview_{};
    DocumentHistory<SceneSnapshot, 32> History_;
    world_demo::Name Targets_[32]{};
    foundation::usize TargetCount_ = 0;
    foundation::uint64 Identity_ = 0;
    SceneStamp GestureStamp_{};
    foundation::uint64 PreviewRevision_ = 0;
    bool Gesture_ = false;
    bool Loaded_ = false;
};
static_assert(sizeof(SceneDocument) < foundation::usize{512} * 1024);
} // namespace ludus::editor
