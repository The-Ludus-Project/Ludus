// Demand-driven generation executor contracts: plan validation, per-scope requests
// that leave other scopes pending, keys over input product digests, plan binding,
// latched failure, stale candidates and kernel-rejected cache entries. See
// docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation.

#include <ludus/content/procedural/core/generation_executor.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::content::procedural;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

constexpr GenerationCacheLimits ROOMY_LIMITS{64, 1U << 16U};
constexpr usize TRACE_CAPACITY = 32;
constexpr uint32 TOY_OPERATOR = 900;
constexpr uint32 TOY_SCHEMA = 4;
constexpr uint32 SHARED_SCOPE = 0;
constexpr uint32 SCOPE_A = 1;
constexpr uint32 SCOPE_B = 2;
constexpr uint32 STAGE = 1;
/// A parameter value the toy kernel refuses, to exercise stage failure.
constexpr uint32 FAILING_VALUE = 0xDEADU;

/// Product = the stage's value plus the sum of its input products. Parameters are the value and an ignored label,
/// so a label edit changes the stage key but not the product.
class SumKernel final : public GenerationStageKernel
{
public:
    GenerationStatus TryExecute(const GenerationStage& stage,
                                GenerationInputProducts inputs,
                                GenerationByteWriter& outProduct) noexcept override
    {
        ++ExecutionCount;
        GenerationByteReader parameters({stage.Parameters.GetData(), stage.Parameters.GetSize()});
        uint32 sum = parameters.ReadUInt32();
        if (sum == FAILING_VALUE)
        {
            return GenerationStatus::ConstraintFailed;
        }
        for (const BorrowedView<const uint8> input : inputs)
        {
            GenerationByteReader reader(input);
            sum += reader.ReadUInt32();
        }
        outProduct.WriteUInt32(sum);
        return GenerationStatus::Success;
    }
    bool IsValidProduct(const GenerationStage& /*stage*/,
                        GenerationInputProducts /*inputs*/,
                        BorrowedView<const uint8> product) noexcept override
    {
        return product.GetSize() == sizeof(uint32);
    }

    usize ExecutionCount = 0;
};

/// The toy stage's parameters.
struct ToyParameters final
{
    uint32 Value = 0;
    uint32 Label = 0;
};

void WriteParameters(ToyParameters parameters, Array<uint8>& outBytes)
{
    GenerationByteWriter writer;
    writer.WriteUInt32(parameters.Value);
    writer.WriteUInt32(parameters.Label);
    outBytes.Clear();
    outBytes.AddRange(writer.GetBytes());
}

[[nodiscard]] GenerationStage MakeStage(uint32 scope, ToyParameters parameters, SumKernel& inoutKernel)
{
    GenerationStage stage;
    stage.Scope = scope;
    stage.Stage = STAGE;
    stage.Operator = TOY_OPERATOR;
    stage.OperatorVersion = 1;
    stage.Schema = TOY_SCHEMA;
    WriteParameters(parameters, stage.Parameters);
    stage.KernelOrNull = &inoutKernel;
    return stage;
}

/// A shared stage feeding one stage in each of two scopes.
struct BranchPlan final
{
    explicit BranchPlan(SumKernel& inoutKernel)
    {
        Stages.Add(MakeStage(SHARED_SCOPE, {10, 0}, inoutKernel));
        Stages.Add(MakeStage(SCOPE_A, {1, 0}, inoutKernel));
        Stages.Add(MakeStage(SCOPE_B, {2, 0}, inoutKernel));
        Stages[1].Inputs.Add(0);
        Stages[2].Inputs.Add(0);
    }
    [[nodiscard]] BorrowedView<const GenerationStage> GetView() const
    {
        return {Stages.GetData(), Stages.GetSize()};
    }
    void SetParameters(usize index, ToyParameters parameters)
    {
        WriteParameters(parameters, Stages[index].Parameters);
    }

    Array<GenerationStage> Stages;
};

[[nodiscard]] uint32 ReadValue(const GenerationProduct& product)
{
    REQUIRE(product.State == GenerationProductState::Ready);
    GenerationByteReader reader({product.Bytes.GetData(), product.Bytes.GetSize()});
    return reader.ReadUInt32();
}

[[nodiscard]] usize CountOutcome(const GenerationTrace& trace, GenerationStageOutcome outcome)
{
    usize count = 0;
    for (const GenerationStageRecord& record : trace.GetRecords())
    {
        count += record.Outcome == outcome ? 1 : 0;
    }
    return count;
}
} // namespace

TEST_CASE("generation plans are validated before a candidate begins", "[procedural][core][executor]")
{
    SumKernel kernel;
    BranchPlan plan(kernel);
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationCandidate candidate;

    SECTION("a valid plan")
    {
        REQUIRE(ValidateGenerationPlan(plan.GetView()) == GenerationStatus::Success);
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::Success);
        REQUIRE(candidate.Products.GetSize() == 3);
        for (const GenerationProduct& product : candidate.Products)
        {
            REQUIRE(product.State == GenerationProductState::Pending);
        }
        return;
    }
    SECTION("an empty plan")
    {
        REQUIRE(coordinator.TryBeginCandidate({}, candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("a missing kernel")
    {
        plan.Stages[1].KernelOrNull = nullptr;
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("an input that does not precede its consumer")
    {
        plan.Stages[1].Inputs[0] = 2;
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("a repeated scope and stage")
    {
        plan.Stages[2].Scope = SCOPE_A;
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::InvalidInput);
    }
    SECTION("too many inputs")
    {
        for (usize i = 0; i < MAX_GENERATION_STAGE_INPUTS; ++i)
        {
            plan.Stages[2].Inputs.Add(0);
        }
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::CapacityExceeded);
    }
    REQUIRE(candidate.Products.IsEmpty() == true);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
}

TEST_CASE("a scope request evaluates only its upstream closure", "[procedural][core][executor]")
{
    SumKernel kernel;
    BranchPlan plan(kernel);
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::Success);

    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_A, trace, candidate) == GenerationStatus::Success);
    REQUIRE(kernel.ExecutionCount == 2);
    REQUIRE(trace.GetRecords().GetSize() == 2);
    REQUIRE(trace.GetRecords()[0].Scope == SHARED_SCOPE);
    REQUIRE(trace.GetRecords()[1].Scope == SCOPE_A);
    REQUIRE(candidate.IsScopeReady(SCOPE_A) == true);
    REQUIRE(candidate.IsScopeReady(SCOPE_B) == false);
    REQUIRE(candidate.FindProduct(SCOPE_B, STAGE)->State == GenerationProductState::Pending);
    REQUIRE(ReadValue(*candidate.FindProduct(SCOPE_A, STAGE)) == 11);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
    REQUIRE(coordinator.GetAcceptedRevision() == 0);

    // The shared stage is ready, so requesting B evaluates B alone; repeating a request evaluates nothing.
    trace.Clear();
    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_B, trace, candidate) == GenerationStatus::Success);
    REQUIRE(kernel.ExecutionCount == 3);
    REQUIRE(trace.GetRecords().GetSize() == 1);
    REQUIRE(trace.GetRecords()[0].Scope == SCOPE_B);
    REQUIRE(ReadValue(*candidate.FindProduct(SCOPE_B, STAGE)) == 12);
    trace.Clear();
    REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, candidate) == GenerationStatus::Success);
    REQUIRE(trace.GetRecords().IsEmpty() == true);
    REQUIRE(candidate.IsReady() == true);

    GenerationDigest digest;
    REQUIRE(TryComputeGenerationManifestDigest({candidate.Products.GetData(), candidate.Products.GetSize()}, digest) ==
            true);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    REQUIRE(coordinator.GetAccepted().Digest == digest);
    REQUIRE(candidate.Status == GenerationStatus::NotReady);
    REQUIRE(candidate.Products.IsEmpty() == true);

    SECTION("an unknown scope or an edited plan is refused")
    {
        GenerationCandidate next;
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), next) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_B + 1, trace, next) ==
                GenerationStatus::InvalidInput);
        plan.SetParameters(1, {3, 0});
        REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_A, trace, next) == GenerationStatus::InvalidInput);
        REQUIRE(next.Status == GenerationStatus::Success);
        REQUIRE(next.FindProduct(SCOPE_A, STAGE)->State == GenerationProductState::Pending);
    }
    SECTION("an upstream edit that reproduces its product keeps downstream keys")
    {
        plan.SetParameters(0, {10, 1});
        GenerationCandidate next;
        trace.Clear();
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), next) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, next) == GenerationStatus::Success);
        REQUIRE(trace.GetRecords()[0].Outcome == GenerationStageOutcome::Executed);
        REQUIRE(trace.GetRecords()[0].Key != coordinator.GetAccepted().Products[0].Key);
        REQUIRE(trace.GetRecords()[0].Output == coordinator.GetAccepted().Products[0].Output);
        REQUIRE(CountOutcome(trace, GenerationStageOutcome::CacheHit) == 2);
    }
    SECTION("an upstream edit that changes its product invalidates every dependent")
    {
        plan.SetParameters(0, {20, 0});
        GenerationCandidate next;
        trace.Clear();
        REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), next) == GenerationStatus::Success);
        REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, next) == GenerationStatus::Success);
        REQUIRE(CountOutcome(trace, GenerationStageOutcome::Executed) == 3);
        REQUIRE(ReadValue(*next.FindProduct(SCOPE_B, STAGE)) == 22);
    }
}

TEST_CASE("a failed stage latches its candidate and publishes nothing", "[procedural][core][executor]")
{
    SumKernel kernel;
    BranchPlan plan(kernel);
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    plan.SetParameters(2, {FAILING_VALUE, 0});
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_A, trace, candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_B, trace, candidate) ==
            GenerationStatus::ConstraintFailed);
    REQUIRE(trace.GetRecords()[2].Outcome == GenerationStageOutcome::Failed);
    REQUIRE(candidate.FindProduct(SCOPE_B, STAGE)->State == GenerationProductState::Failed);
    REQUIRE(candidate.FindProduct(SCOPE_A, STAGE)->State == GenerationProductState::Ready);
    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_A, trace, candidate) ==
            GenerationStatus::ConstraintFailed);
    REQUIRE(coordinator.TryPublish(candidate) == GenerationStatus::NotReady);
    REQUIRE(coordinator.GetAcceptedRevision() == 0);
    REQUIRE(coordinator.GetAccepted().Products.IsEmpty() == true);
}

TEST_CASE("a candidate begun against an older revision cannot publish", "[procedural][core][executor]")
{
    SumKernel kernel;
    BranchPlan plan(kernel);
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate first;
    GenerationCandidate second;
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), first) == GenerationStatus::Success);
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), second) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, first) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, second) == GenerationStatus::Success);
    REQUIRE(coordinator.TryPublish(first) == GenerationStatus::Success);
    const GenerationDigest accepted = coordinator.GetAccepted().Digest;
    REQUIRE(coordinator.TryPublish(second) == GenerationStatus::StaleCandidate);
    REQUIRE(second.IsReady() == true);
    REQUIRE(coordinator.GetAcceptedRevision() == 1);
    REQUIRE(coordinator.GetAccepted().Digest == accepted);
    coordinator.Reject(second);
    REQUIRE(second.Products.IsEmpty() == true);
}

TEST_CASE("a cached product the kernel rejects is discarded and rebuilt", "[procedural][core][executor]")
{
    SumKernel kernel;
    BranchPlan plan(kernel);
    MemoryGenerationCacheStore store(ROOMY_LIMITS);
    GenerationCoordinator coordinator(store);
    GenerationTrace trace(TRACE_CAPACITY);
    GenerationCandidate candidate;
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), candidate) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestAll(plan.GetView(), trace, candidate) == GenerationStatus::Success);
    const GenerationProduct expected = *candidate.FindProduct(SCOPE_A, STAGE);
    const uint8 wrongShape[] = {1, 2, 3};
    REQUIRE(TryStoreGenerationProduct(store, expected.Key, TOY_SCHEMA, BorrowedView<const uint8>(wrongShape)) == true);

    GenerationCandidate next;
    trace.Clear();
    REQUIRE(coordinator.TryBeginCandidate(plan.GetView(), next) == GenerationStatus::Success);
    REQUIRE(coordinator.TryRequestScope(plan.GetView(), SCOPE_A, trace, next) == GenerationStatus::Success);
    REQUIRE(trace.GetRecords()[1].Cache == GenerationCacheOutcome::Corrupt);
    REQUIRE(trace.GetRecords()[1].Outcome == GenerationStageOutcome::Executed);
    REQUIRE(trace.GetRecords()[1].IsStored == true);
    REQUIRE(next.FindProduct(SCOPE_A, STAGE)->Output == expected.Output);
    REQUIRE(next.FindProduct(SCOPE_A, STAGE)->Bytes == expected.Bytes);
}
