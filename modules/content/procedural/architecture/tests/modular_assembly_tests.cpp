// Modular assembly stages on the shared executor: canonical catalogs, exact span
// closure with the fewest adapters, persistent slot identities, validation and
// rejected cache entries. See
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include <ludus/content/procedural/architecture/modular_assembly.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::procedural;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

constexpr GenerationCacheLimits CACHE_LIMITS{16, 1U << 16U};
constexpr usize TRACE_CAPACITY = 16;
constexpr uint32 SHARED_SCOPE = 100;
constexpr uint32 ROOM_SCOPE = 1;
constexpr uint32 CATALOG_STAGE = 1;
constexpr uint32 ASSEMBLY_STAGE = 2;
constexpr uint16 WALL = 1;
constexpr uint16 WINDOW = 2;
constexpr uint16 NARROW_ADAPTER = 10;
constexpr uint16 WIDE_ADAPTER = 11;

constexpr AssemblyModule LIBRARY[] = {
    {WIDE_ADAPTER, 15, AssemblyModuleRole::Adapter},
    {WINDOW, 40, AssemblyModuleRole::Repeat},
    {NARROW_ADAPTER, 10, AssemblyModuleRole::Adapter},
    {WALL, 40, AssemblyModuleRole::Repeat},
};
/// 130 closes with three walls and one narrow adapter; 95 with two walls and a wide adapter; 80 with walls alone.
constexpr AssemblySpan SPANS[] = {{3, 130}, {5, 95}, {8, 80}};

struct AssemblyRun final
{
    ModuleCatalogKernel CatalogKernel;
    SpanAssemblyKernel AssemblyKernel;
    Array<GenerationStage> Plan;
    MemoryGenerationCacheStore Store{CACHE_LIMITS};
    GenerationCoordinator Coordinator{Store};
    GenerationTrace Trace{TRACE_CAPACITY};
    GenerationCandidate Candidate;

    [[nodiscard]] GenerationStatus Run(BorrowedView<const AssemblyModule> modules,
                                       BorrowedView<const AssemblySpan> spans)
    {
        Plan.Clear();
        usize catalog = 0;
        REQUIRE(TryAddModuleCatalogStage({SHARED_SCOPE, CATALOG_STAGE, modules}, CatalogKernel, Plan, catalog) ==
                GenerationStatus::Success);
        REQUIRE(TryAddSpanAssemblyStage({ROOM_SCOPE, ASSEMBLY_STAGE, spans}, catalog, AssemblyKernel, Plan) ==
                GenerationStatus::Success);
        Trace.Clear();
        REQUIRE(Coordinator.TryBeginCandidate({Plan.GetData(), Plan.GetSize()}, Candidate) ==
                GenerationStatus::Success);
        return Coordinator.TryRequestScope({Plan.GetData(), Plan.GetSize()}, ROOM_SCOPE, Trace, Candidate);
    }
    [[nodiscard]] Array<AssemblyInstance> ReadInstances() const
    {
        const GenerationProduct* productOrNull = Candidate.FindProduct(ROOM_SCOPE, ASSEMBLY_STAGE);
        REQUIRE(productOrNull != nullptr);
        Array<AssemblyInstance> instances;
        REQUIRE(TryReadSpanAssembly({productOrNull->Bytes.GetData(), productOrNull->Bytes.GetSize()},
                                    ROOM_SCOPE,
                                    instances) == true);
        return instances;
    }
};
} // namespace

TEST_CASE("a span assembly closes every span exactly with the fewest adapters", "[procedural][assembly]")
{
    AssemblyRun run;
    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(LIBRARY), BorrowedView<const AssemblySpan>(SPANS)) ==
            GenerationStatus::Success);
    const Array<AssemblyInstance> instances = run.ReadInstances();
    struct Expected final
    {
        uint16 Span;
        uint16 Slot;
        uint16 Module;
        uint32 Offset;
    };
    constexpr Expected EXPECTED[] = {
        {3, 0, WALL, 0},
        {3, 1, WALL, 40},
        {3, 2, WALL, 80},
        {3, 3, NARROW_ADAPTER, 120},
        {5, 0, WALL, 0},
        {5, 1, WALL, 40},
        {5, 2, WIDE_ADAPTER, 80},
        {8, 0, WALL, 0},
        {8, 1, WALL, 40},
    };
    REQUIRE(instances.GetSize() == sizeof(EXPECTED) / sizeof(EXPECTED[0]));
    for (usize i = 0; i < instances.GetSize(); ++i)
    {
        REQUIRE(instances[i].Identity.Scope == ROOM_SCOPE);
        REQUIRE(instances[i].Identity.Span == EXPECTED[i].Span);
        REQUIRE(instances[i].Identity.Slot == EXPECTED[i].Slot);
        REQUIRE(instances[i].Module == EXPECTED[i].Module);
        REQUIRE(instances[i].Offset == EXPECTED[i].Offset);
    }

    // The catalog product is canonical: sorted by ID whatever the authored order.
    Array<AssemblyModule> catalog;
    const GenerationProduct& catalogProduct = *run.Candidate.FindProduct(SHARED_SCOPE, CATALOG_STAGE);
    REQUIRE(TryReadModuleCatalog({catalogProduct.Bytes.GetData(), catalogProduct.Bytes.GetSize()}, catalog) == true);
    REQUIRE(catalog.GetSize() == 4);
    REQUIRE(catalog[0].Id == WALL);
    REQUIRE(catalog[3].Id == WIDE_ADAPTER);
}

TEST_CASE("reordering an authored library reproduces the same catalog and assembly", "[procedural][assembly]")
{
    AssemblyRun run;
    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(LIBRARY), BorrowedView<const AssemblySpan>(SPANS)) ==
            GenerationStatus::Success);
    const GenerationProduct catalog = *run.Candidate.FindProduct(SHARED_SCOPE, CATALOG_STAGE);
    const GenerationProduct assembly = *run.Candidate.FindProduct(ROOM_SCOPE, ASSEMBLY_STAGE);
    REQUIRE(run.Coordinator.TryPublish(run.Candidate) == GenerationStatus::Success);

    const AssemblyModule reordered[] = {LIBRARY[3], LIBRARY[2], LIBRARY[1], LIBRARY[0]};
    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(reordered), BorrowedView<const AssemblySpan>(SPANS)) ==
            GenerationStatus::Success);
    REQUIRE(run.Trace.GetRecords()[0].Outcome == GenerationStageOutcome::Executed);
    REQUIRE(run.Trace.GetRecords()[0].Key != catalog.Key);
    REQUIRE(run.Trace.GetRecords()[0].Output == catalog.Output);
    // The assembly key covers the catalog's output digest, not its parameters, so the assembly is a cache hit.
    REQUIRE(run.Trace.GetRecords()[1].Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(run.Candidate.FindProduct(ROOM_SCOPE, ASSEMBLY_STAGE)->Bytes == assembly.Bytes);
}

TEST_CASE("a span with no exact closure fails the stage", "[procedural][assembly]")
{
    AssemblyRun run;
    constexpr AssemblySpan UNCLOSABLE[] = {{1, 80}, {2, 5}};
    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(LIBRARY), BorrowedView<const AssemblySpan>(UNCLOSABLE)) ==
            GenerationStatus::ConstraintFailed);
    REQUIRE(run.Trace.GetRecords()[1].Outcome == GenerationStageOutcome::Failed);
    REQUIRE(run.Coordinator.TryPublish(run.Candidate) == GenerationStatus::NotReady);
}

TEST_CASE("invalid libraries and spans are refused before planning", "[procedural][assembly]")
{
    ModuleCatalogKernel catalogKernel;
    SpanAssemblyKernel assemblyKernel;
    Array<GenerationStage> plan;
    usize catalog = 0;

    SECTION("libraries")
    {
        const AssemblyModule duplicate[] = {LIBRARY[1], LIBRARY[1]};
        const AssemblyModule adaptersOnly[] = {LIBRARY[0], LIBRARY[2]};
        const AssemblyModule zeroWidth[] = {{WALL, 0, AssemblyModuleRole::Repeat}};
        for (const BorrowedView<const AssemblyModule> modules : {BorrowedView<const AssemblyModule>(duplicate),
                                                                 BorrowedView<const AssemblyModule>(adaptersOnly),
                                                                 BorrowedView<const AssemblyModule>(zeroWidth),
                                                                 BorrowedView<const AssemblyModule>{}})
        {
            REQUIRE(TryAddModuleCatalogStage({SHARED_SCOPE, CATALOG_STAGE, modules}, catalogKernel, plan, catalog) ==
                    GenerationStatus::InvalidInput);
        }
        REQUIRE(plan.IsEmpty() == true);
    }
    SECTION("spans")
    {
        REQUIRE(TryAddModuleCatalogStage({SHARED_SCOPE, CATALOG_STAGE, BorrowedView<const AssemblyModule>(LIBRARY)},
                                         catalogKernel,
                                         plan,
                                         catalog) == GenerationStatus::Success);
        const AssemblySpan unordered[] = {SPANS[1], SPANS[0]};
        const AssemblySpan empty[] = {{1, 0}};
        const AssemblySpan tooLong[] = {{1, MAX_ASSEMBLY_SPAN_LENGTH + 1}};
        for (const BorrowedView<const AssemblySpan> spans : {BorrowedView<const AssemblySpan>(unordered),
                                                             BorrowedView<const AssemblySpan>(empty),
                                                             BorrowedView<const AssemblySpan>(tooLong)})
        {
            REQUIRE(TryAddSpanAssemblyStage({ROOM_SCOPE, ASSEMBLY_STAGE, spans}, catalog, assemblyKernel, plan) ==
                    GenerationStatus::InvalidInput);
        }
        // The input must be a catalog stage already in the plan.
        REQUIRE(TryAddSpanAssemblyStage({ROOM_SCOPE, ASSEMBLY_STAGE, BorrowedView<const AssemblySpan>(SPANS)},
                                        catalog + 1,
                                        assemblyKernel,
                                        plan) == GenerationStatus::InvalidInput);
        REQUIRE(plan.GetSize() == 1);
    }
}

TEST_CASE("a well-framed assembly that does not close its spans is rebuilt", "[procedural][assembly][cache]")
{
    AssemblyRun run;
    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(LIBRARY), BorrowedView<const AssemblySpan>(SPANS)) ==
            GenerationStatus::Success);
    const GenerationProduct expected = *run.Candidate.FindProduct(ROOM_SCOPE, ASSEMBLY_STAGE);
    // One instance of span 3 whose module does not reach the span's length.
    GenerationByteWriter wrong;
    wrong.WriteUInt32(1);
    wrong.WriteUInt16(SPANS[0].Id);
    wrong.WriteUInt16(0);
    wrong.WriteUInt16(WALL);
    wrong.WriteUInt32(0);
    REQUIRE(TryStoreGenerationProduct(run.Store, expected.Key, SPAN_ASSEMBLY_PRODUCT_SCHEMA, wrong.GetBytes()) == true);

    REQUIRE(run.Run(BorrowedView<const AssemblyModule>(LIBRARY), BorrowedView<const AssemblySpan>(SPANS)) ==
            GenerationStatus::Success);
    REQUIRE(run.Trace.GetRecords()[0].Outcome == GenerationStageOutcome::CacheHit);
    REQUIRE(run.Trace.GetRecords()[1].Cache == GenerationCacheOutcome::Corrupt);
    REQUIRE(run.Trace.GetRecords()[1].Outcome == GenerationStageOutcome::Executed);
    REQUIRE(run.Candidate.FindProduct(ROOM_SCOPE, ASSEMBLY_STAGE)->Bytes == expected.Bytes);
}
