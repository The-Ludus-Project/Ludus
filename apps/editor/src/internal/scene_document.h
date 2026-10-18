#pragma once

#include <ludus/foundation/base/pointer.hpp>
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
/// Undo steps one scene document keeps. Every step stores a whole before/after snapshot pair.
inline constexpr foundation::usize SCENE_HISTORY_CAPACITY = 32;
using SceneHistory = DocumentHistory<SceneSnapshot, SCENE_HISTORY_CAPACITY>;
/// Most entities one transform gesture moves.
inline constexpr foundation::usize MAX_SCENE_TRANSFORM_TARGETS = 32;
/// Inline budget for SceneDocument. Hosts embed the document by value and tests construct it on the stack,
/// so its three working snapshots stay far below the smallest default thread stack Ludus runs on (512 KiB
/// for macOS secondary threads). The snapshot history is owned out of line and has its own budget.
inline constexpr foundation::usize MAX_SCENE_DOCUMENT_INLINE_BYTES = foundation::usize{64} * 1024;
/// Budget for the history one document allocates on its first Load: SCENE_HISTORY_CAPACITY snapshot pairs,
/// about 535 KiB with the scatter-capable Level. Growing Level or the capacity past this should be a
/// deliberate decision about per-document memory, not a side effect of a schema change.
inline constexpr foundation::usize MAX_SCENE_HISTORY_BYTES = foundation::usize{1024} * 1024;
// Fixed capacity history, allocated once by the first successful Load and reused
// after Clear. Snapshot copies cannot fail. Load and commit validate a candidate
// before publishing anything.
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
        return Gesture_ == false && mHistoryOrNull.Get() != nullptr && mHistoryOrNull->CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return Gesture_ == false && mHistoryOrNull.Get() != nullptr && mHistoryOrNull->CanRedo();
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
    [[nodiscard]] foundation::uint64 getRevision() const noexcept;

    SceneSnapshot Draft_{};
    SceneSnapshot Saved_{};
    SceneSnapshot Preview_{};
    /// Empty until the first successful Load; kept, and reset, across Clear and later loads.
    foundation::UniquePtr<SceneHistory> mHistoryOrNull;
    world_demo::Name mTargets[MAX_SCENE_TRANSFORM_TARGETS]{};
    foundation::usize TargetCount_ = 0;
    foundation::uint64 Identity_ = 0;
    SceneStamp GestureStamp_{};
    foundation::uint64 PreviewRevision_ = 0;
    bool Gesture_ = false;
    bool Loaded_ = false;
};
static_assert(sizeof(SceneDocument) <= MAX_SCENE_DOCUMENT_INLINE_BYTES,
              "SceneDocument must stay small enough to embed and to construct on a thread stack");
static_assert(sizeof(SceneHistory) <= MAX_SCENE_HISTORY_BYTES,
              "Scene history outgrew its per-document budget; shrink Level or SCENE_HISTORY_CAPACITY");
} // namespace ludus::editor
