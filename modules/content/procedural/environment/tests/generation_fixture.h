#pragma once

// The fixed-seed two-scope generation fixture of slice P1, shared by the scatter
// coordinator tests, the demand-driven request tests and the persistent cache
// recovery tests. Rooms A and B each have scatter layers and a span assembly; both
// assemblies read one shared module catalog stage, and B's spans are its separately
// editable local input.

#include <ludus/content/procedural/architecture/modular_assembly.h>
#include <ludus/content/procedural/environment/scatter_generation.h>

#include "scatter_golden.h"

#include <catch2/catch_test_macros.hpp>

namespace ludus::content::procedural::generation_fixture
{
inline constexpr GenerationCacheLimits CACHE_LIMITS{64, 1U << 20U};
inline constexpr usize TRACE_CAPACITY = 64;
inline constexpr uint16 ROOM_A = 1;
inline constexpr uint16 ROOM_B = 2;
inline constexpr uint8 TREE_LAYER = 1;
inline constexpr uint8 STONE_LAYER = 2;
/// Scope of the module catalog both rooms' assemblies read.
inline constexpr uint32 SHARED_SCOPE = 100;
inline constexpr uint32 CATALOG_STAGE = 1;
/// Assembly stage ID in each room, above every scatter layer ID.
inline constexpr uint32 ASSEMBLY_STAGE = 0x100;

inline constexpr ScatterVariant TREE_VARIANTS[] = {{10U}, {20U}};
inline constexpr ScatterVariant STONE_VARIANTS[] = {{5U}};
inline constexpr ScatterRectangle ROOM_B_KEEP_OUTS[] = {{0, 0, 50, 50}};
/// The keep-out edit of room A, with known removals: every accepted placement inside it.
inline constexpr ScatterRectangle ROOM_A_EDIT[] = {{100, 80, 220, 200}};

/// Two rooms: A with trees and stones, B with trees and a keep-out. Views borrow the fixture's own arrays.
struct TwoScopeFixture final
{
    TwoScopeFixture()
    {
        RoomALayers[0] = {TREE_LAYER, 40, 3, foundation::BorrowedView<const ScatterVariant>(TREE_VARIANTS), 900, 1100};
        RoomALayers[1] =
            {STONE_LAYER, 25, 2, foundation::BorrowedView<const ScatterVariant>(STONE_VARIANTS), 1000, 1000};
        RoomBLayers[0] = {TREE_LAYER, 30, 2, foundation::BorrowedView<const ScatterVariant>(TREE_VARIANTS), 800, 1200};
        Scopes[0].Recipe = {ROOM_A, {0, 0, 400, 300}, {}, foundation::BorrowedView<const ScatterLayer>(RoomALayers)};
        Scopes[1].Recipe = {ROOM_B,
                            {-200, -100, 200, 200},
                            foundation::BorrowedView<const ScatterRectangle>(ROOM_B_KEEP_OUTS),
                            {RoomBLayers}};
        Input.WorldSeed = golden::WORLD_SEED;
        Input.Domains = golden::DOMAINS;
        Input.Scopes = foundation::BorrowedView<const ScatterScope>(Scopes);
    }
    TwoScopeFixture(const TwoScopeFixture&) = delete;
    TwoScopeFixture& operator=(const TwoScopeFixture&) = delete;

    ScatterLayer RoomALayers[2] = {};
    ScatterLayer RoomBLayers[1] = {};
    ScatterScope Scopes[2] = {};
    ScatterGenerationInput Input;
};

/// The two-scope fixture as one plan: the shared catalog, both rooms' scatter layers and both rooms' assemblies.
struct DemandFixture final
{
    DemandFixture()
    {
        Modules[0] = {1, 40, AssemblyModuleRole::Repeat};
        Modules[1] = {2, 30, AssemblyModuleRole::Repeat};
        Modules[2] = {10, 10, AssemblyModuleRole::Adapter};
        Modules[3] = {11, 15, AssemblyModuleRole::Adapter};
        RoomASpans[0] = {1, 400};
        RoomASpans[1] = {2, 305};
        RoomBSpans[0] = {1, 400};
        RoomBSpans[1] = {2, 130};
    }
    DemandFixture(const DemandFixture&) = delete;
    DemandFixture& operator=(const DemandFixture&) = delete;

    /// Rebuild the plan from the current inputs, as an edit does.
    void Rebuild()
    {
        Plan.Clear();
        usize catalog = 0;
        REQUIRE(TryAddModuleCatalogStage(
                    {SHARED_SCOPE, CATALOG_STAGE, foundation::BorrowedView<const AssemblyModule>(Modules)},
                    CatalogKernel,
                    Plan,
                    catalog) == GenerationStatus::Success);
        REQUIRE(TryAddScatterStages(Scatter.Input, ScatterKernel, Plan) == GenerationStatus::Success);
        REQUIRE(
            TryAddSpanAssemblyStage({ROOM_A, ASSEMBLY_STAGE, foundation::BorrowedView<const AssemblySpan>(RoomASpans)},
                                    catalog,
                                    AssemblyKernel,
                                    Plan) == GenerationStatus::Success);
        REQUIRE(
            TryAddSpanAssemblyStage({ROOM_B, ASSEMBLY_STAGE, foundation::BorrowedView<const AssemblySpan>(RoomBSpans)},
                                    catalog,
                                    AssemblyKernel,
                                    Plan) == GenerationStatus::Success);
    }
    [[nodiscard]] foundation::BorrowedView<const GenerationStage> GetPlan() const
    {
        return {Plan.GetData(), Plan.GetSize()};
    }

    TwoScopeFixture Scatter;
    AssemblyModule Modules[4] = {};
    AssemblySpan RoomASpans[2] = {};
    AssemblySpan RoomBSpans[2] = {};
    ModuleCatalogKernel CatalogKernel;
    ScatterLayerKernel ScatterKernel;
    SpanAssemblyKernel AssemblyKernel;
    foundation::Array<GenerationStage> Plan;
};

[[nodiscard]] inline bool AreEqual(const ScatterPlacement& a, const ScatterPlacement& b) noexcept
{
    return a.Identity == b.Identity && a.LatticeX == b.LatticeX && a.LatticeY == b.LatticeY && a.Variant == b.Variant &&
           a.Rotation == b.Rotation && a.Scale == b.Scale;
}

[[nodiscard]] inline const GenerationStageRecord& FindRecord(const GenerationTrace& trace, uint32 scope, uint32 stage)
{
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        if (record.Scope == scope && record.Stage == stage)
        {
            return record;
        }
    }
    FAIL("no trace record for the stage");
    return trace.GetRecords()[0];
}

/// Evaluations recorded with an outcome.
[[nodiscard]] inline usize CountOutcome(const GenerationTrace& trace, GenerationStageOutcome outcome)
{
    usize count = 0;
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        count += record.Outcome == outcome ? 1 : 0;
    }
    return count;
}

/// Whether the trace recorded any evaluation of a scope.
[[nodiscard]] inline bool HasScopeRecord(const GenerationTrace& trace, uint32 scope)
{
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        if (record.Scope == scope)
        {
            return true;
        }
    }
    return false;
}

/// Products are equal stage by stage: identity, state, key, output digest and canonical bytes.
inline void RequireSameProducts(foundation::BorrowedView<const GenerationProduct> expected,
                                foundation::BorrowedView<const GenerationProduct> actual)
{
    REQUIRE(actual.GetSize() == expected.GetSize());
    for (usize i = 0; i < expected.GetSize(); ++i)
    {
        REQUIRE(actual[i].Scope == expected[i].Scope);
        REQUIRE(actual[i].Stage == expected[i].Stage);
        REQUIRE(actual[i].State == expected[i].State);
        REQUIRE(actual[i].Key == expected[i].Key);
        REQUIRE(actual[i].Output == expected[i].Output);
        REQUIRE(actual[i].Bytes == expected[i].Bytes);
    }
}

/// A clean full cook of the plan with an empty cache: every stage runs.
[[nodiscard]] inline GenerationManifest CookClean(foundation::BorrowedView<const GenerationStage> plan)
{
    MemoryGenerationCacheStore store(CACHE_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    REQUIRE(coordinator.TryBeginCandidate(plan, candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestAll(plan, trace, candidate) == GenerationStatus::Success);
    REQUIRE(CountOutcome(trace, GenerationStageOutcome::Executed) == plan.GetSize());
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::Success);
    return coordinator.GetAccepted();
}

/// Every placement of a manifest's scatter layers, and every assembly instance, in plan order.
struct ManifestContents final
{
    foundation::Array<ScatterPlacement> Placements;
    foundation::Array<AssemblyInstance> Instances;
};

[[nodiscard]] inline ManifestContents ReadContents(const GenerationManifest& manifest)
{
    ManifestContents contents;
    for (const GenerationProduct& product : manifest.Products)
    {
        const foundation::BorrowedView<const uint8> bytes{product.Bytes.GetData(), product.Bytes.GetSize()};
        if (product.Scope == SHARED_SCOPE)
        {
            continue;
        }
        if (product.Stage == ASSEMBLY_STAGE)
        {
            foundation::Array<AssemblyInstance> instances;
            REQUIRE(TryReadSpanAssembly(bytes, product.Scope, instances) == true);
            contents.Instances.AddRange({instances.GetData(), instances.GetSize()});
            continue;
        }
        foundation::Array<ScatterPlacement> placements;
        REQUIRE(TryReadScatterLayerProduct(bytes, placements) == true);
        contents.Placements.AddRange({placements.GetData(), placements.GetSize()});
    }
    return contents;
}

/// Same placement and instance identities and values, in the same order.
inline void RequireSameContents(const ManifestContents& expected, const ManifestContents& actual)
{
    REQUIRE(actual.Placements.GetSize() == expected.Placements.GetSize());
    for (usize i = 0; i < expected.Placements.GetSize(); ++i)
    {
        REQUIRE(AreEqual(actual.Placements[i], expected.Placements[i]) == true);
    }
    REQUIRE(actual.Instances.GetSize() == expected.Instances.GetSize());
    for (usize i = 0; i < expected.Instances.GetSize(); ++i)
    {
        REQUIRE(actual.Instances[i].Identity == expected.Instances[i].Identity);
        REQUIRE(actual.Instances[i].Module == expected.Instances[i].Module);
        REQUIRE(actual.Instances[i].Offset == expected.Instances[i].Offset);
    }
}
} // namespace ludus::content::procedural::generation_fixture
