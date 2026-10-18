#include "internal/scene_document.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/math/scalar.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
using ludus::world_demo::LevelError;
namespace
{
void Load(SceneDocument& document)
{
    const auto bytes = ludus::world_demo::ExampleLevel();
    REQUIRE(document.Load(bytes.data(), bytes.size()).Error == LevelError::None);
}
} // namespace
TEST_CASE("Scene candidate failure preserves identity draft preview and history", "[editor][scene]")
{
    SceneDocument doc;
    Load(doc);
    const auto stamp = doc.Stamp(1, 1);
    const auto original = doc.Draft();
    CHECK(doc.Load("{}", 2).Error != LevelError::None);
    CHECK(doc.Draft() == original);
    CHECK(doc.Stamp(1, 1) == stamp);
    auto invalid = original;
    invalid.Level.EntityCount = 33;
    CHECK_FALSE(doc.Commit(invalid, stamp));
    CHECK_FALSE(doc.CanUndo());
    invalid = original;
    invalid.Level.Entities[0].Scale.X = 0;
    CHECK_FALSE(doc.Commit(invalid, stamp));
    CHECK(doc.Draft() == original);
    auto changed = original;
    changed.Level.Entities[0].Position.X += 0.1F;
    REQUIRE(doc.Commit(changed, stamp));
    CHECK_FALSE(doc.Commit(original, stamp));
    const auto revision = doc.Stamp(1, 1).Revision;
    REQUIRE(doc.Undo());
    CHECK_FALSE(doc.Dirty());
    CHECK(doc.Stamp(1, 1).Revision > revision);
    REQUIRE(doc.Redo());
    CHECK(doc.Dirty());
    CHECK(doc.Load("{}", 2).Error != LevelError::None);
    CHECK(doc.Draft() == changed);
    CHECK(doc.CanUndo());
    doc.Clear();
    CHECK_FALSE(doc.Loaded());
    Load(doc);
    CHECK(doc.Stamp(1, 1).Document != stamp.Document);
}
TEST_CASE("Scene transform prepares every target and previews one cancellable history item", "[editor][scene]")
{
    SceneDocument doc;
    Load(doc);
    const auto original = doc.Draft();
    ludus::world_demo::Name ids[] = {original.Level.Entities[0].Id, original.Level.Entities[1].Id};
    SceneTransform values[] = {{original.Level.Entities[0].Position, 0, {1, 1}},
                               {original.Level.Entities[1].Position, 0, {1, 1}}};
    REQUIRE(doc.BeginTransform(ids, 2, doc.Stamp(0, 0)));
    values[0].Position.X += 0.1F;
    values[1].Position.X += 0.1F;
    REQUIRE(doc.PreviewTransform(values, 2));
    CHECK(doc.Draft() == original);
    CHECK_FALSE(doc.Dirty());
    CHECK_FALSE(doc.CanUndo());
    auto invalid = values[1];
    values[1].Scale.Y = -1;
    CHECK_FALSE(doc.PreviewTransform(values, 2));
    CHECK(doc.Visible().Level.Entities[0].Position.X == values[0].Position.X);
    values[1] = invalid;
    doc.CancelTransform();
    CHECK(doc.Visible() == original);
    CHECK_FALSE(doc.CanUndo());
    REQUIRE(doc.BeginTransform(ids, 2, doc.Stamp(0, 0)));
    REQUIRE(doc.PreviewTransform(values, 2));
    values[0].Position.X += 0.1F;
    REQUIRE(doc.PreviewTransform(values, 2));
    REQUIRE(doc.AcceptTransform());
    REQUIRE(doc.Undo());
    CHECK(doc.Draft() == original);
    CHECK_FALSE(doc.CanUndo());
    REQUIRE(doc.Redo());
    CHECK(doc.Draft().Level.Entities[0].Position.X == values[0].Position.X);
}
TEST_CASE("Scene save acknowledgements and bounded history retain content identity", "[editor][scene]")
{
    SceneDocument doc;
    Load(doc);
    const auto saved = doc.Draft();
    const auto identity = doc.Stamp(0, 0).Document;
    auto first = saved;
    first.Level.Entities[0].Position.X += 0.1F;
    REQUIRE(doc.Commit(first, doc.Stamp(0, 0)));
    auto second = first;
    second.Level.Entities[0].Position.X += 0.1F;
    REQUIRE(doc.Commit(second, doc.Stamp(0, 0)));
    REQUIRE(doc.AcknowledgeSave(first, identity));
    CHECK(doc.Dirty());
    REQUIRE(doc.Undo());
    CHECK_FALSE(doc.Dirty());
    CHECK_FALSE(doc.AcknowledgeSave(saved, identity + 1));
    CHECK_FALSE(doc.Dirty());
    for (usize i = 0; i < 40; ++i)
    {
        auto next = doc.Draft();
        next.Level.Entities[0].Rotation += 0.01F;
        REQUIRE(doc.Commit(next, doc.Stamp(0, 0)));
    }
    for (usize i = 0; i < 32; ++i)
    {
        REQUIRE(doc.Undo());
    }
    CHECK_FALSE(doc.CanUndo());
    CHECK(doc.Dirty());
}
TEST_CASE("Scene picking rejects stale frames resize camera identity and cancelled previews", "[editor][scene]")
{
    SceneDocument doc;
    Load(doc);
    auto frame = doc.Stamp(3, 4);
    auto id = doc.Draft().Level.Entities[0].Id;
    const auto position = doc.Draft().Level.Entities[0].Position;
    ludus::world_demo::Name picked;
    REQUIRE(doc.Pick(position, frame, frame, picked));
    CHECK(picked == id);
    auto sentinel = picked;
    CHECK_FALSE(doc.Pick(position, frame, doc.Stamp(3, 5), picked));
    CHECK(picked == sentinel);
    CHECK_FALSE(doc.Pick(position, frame, doc.Stamp(4, 4), picked));
    SceneTransform value{position, math::kHalfPiF, {2, 1}};
    REQUIRE(doc.BeginTransform(&id, 1, frame));
    CHECK_FALSE(doc.Pick(position, frame, frame, picked));
    REQUIRE(doc.PreviewTransform(&value, 1));
    REQUIRE(doc.AcceptTransform());
    CHECK_FALSE(doc.Pick(position, frame, frame, picked));
    frame = doc.Stamp(3, 4);
    REQUIRE(doc.Pick(position, frame, frame, picked));
    CHECK(picked == id);
    REQUIRE(doc.Load(ludus::world_demo::ExampleLevel().data(), ludus::world_demo::ExampleLevel().size()).Error ==
            LevelError::None);
    CHECK_FALSE(doc.Pick(position, frame, frame, picked));
}
TEST_CASE("Scene history is empty before the first load and reset by Clear and reload", "[editor][scene]")
{
    SceneDocument doc;
    CHECK(doc.Stamp(0, 0).Revision == 0);
    CHECK_FALSE(doc.CanUndo());
    CHECK_FALSE(doc.CanRedo());
    CHECK_FALSE(doc.Undo());
    CHECK_FALSE(doc.Redo());
    Load(doc);
    SceneSnapshot changed = doc.Draft();
    changed.Level.Entities[0].Position.X += 0.1F;
    REQUIRE(doc.Commit(changed, doc.Stamp(0, 0)));
    CHECK(doc.CanUndo());
    doc.Clear();
    CHECK_FALSE(doc.CanUndo());
    CHECK_FALSE(doc.Undo());
    CHECK(doc.Stamp(0, 0).Revision == 0);
    Load(doc);
    CHECK_FALSE(doc.CanUndo());
    CHECK(doc.Stamp(0, 0).Revision == 0);
    REQUIRE(doc.Commit(changed, doc.Stamp(0, 0)));
    REQUIRE(doc.Undo());
    CHECK_FALSE(doc.Dirty());
}
