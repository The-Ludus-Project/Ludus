#include <ludus/content/procedural/core/generation_executor.h>

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::procedural
{
namespace
{
using foundation::Array;
using foundation::BorrowedView;

/// Every local field of a stage that its key and the plan digest cover.
void WriteStageIdentity(const GenerationStage& stage, GenerationByteWriter& outWriter) noexcept
{
    outWriter.WriteUInt32(stage.Operator);
    outWriter.WriteUInt32(stage.OperatorVersion);
    outWriter.WriteUInt32(stage.Schema);
    outWriter.WriteUInt32(stage.Scope);
    outWriter.WriteUInt32(stage.Stage);
    outWriter.WriteUInt64(stage.Parameters.GetSize());
    for (const uint8 byte : stage.Parameters)
    {
        outWriter.WriteUInt8(byte);
    }
}

[[nodiscard]] bool TryComputePlanDigest(BorrowedView<const GenerationStage> plan, GenerationDigest& outDigest) noexcept
{
    GenerationByteWriter writer;
    writer.WriteUInt32(GENERATION_STAGE_KEY_VERSION);
    writer.WriteUInt32(static_cast<uint32>(plan.GetSize()));
    for (const GenerationStage& stage : plan)
    {
        WriteStageIdentity(stage, writer);
        writer.WriteUInt32(static_cast<uint32>(stage.Inputs.GetSize()));
        for (const usize input : stage.Inputs)
        {
            writer.WriteUInt64(input);
        }
    }
    if (writer.HasFailed() == true)
    {
        return false;
    }
    outDigest = writer.ComputeDigest();
    return true;
}

[[nodiscard]] const GenerationProduct*
FindIn(BorrowedView<const GenerationProduct> products, uint32 scope, uint32 stage) noexcept
{
    for (const GenerationProduct& product : products)
    {
        if (product.Scope == scope && product.Stage == stage)
        {
            return &product;
        }
    }
    return nullptr;
}
} // namespace

bool GenerationCandidate::IsReady() const noexcept
{
    for (const GenerationProduct& product : Products)
    {
        if (product.State != GenerationProductState::Ready)
        {
            return false;
        }
    }
    return true;
}

bool GenerationCandidate::IsScopeReady(uint32 scope) const noexcept
{
    bool hasStage = false;
    for (const GenerationProduct& product : Products)
    {
        if (product.Scope != scope)
        {
            continue;
        }
        if (product.State != GenerationProductState::Ready)
        {
            return false;
        }
        hasStage = true;
    }
    return hasStage;
}

const GenerationProduct* GenerationCandidate::FindProduct(uint32 scope, uint32 stage) const noexcept
{
    return FindIn({Products.GetData(), Products.GetSize()}, scope, stage);
}

const GenerationProduct* GenerationManifest::FindProduct(uint32 scope, uint32 stage) const noexcept
{
    return FindIn({Products.GetData(), Products.GetSize()}, scope, stage);
}

GenerationStatus ValidateGenerationPlan(BorrowedView<const GenerationStage> plan) noexcept
{
    if (plan.IsValid() == false || plan.IsEmpty() == true)
    {
        return GenerationStatus::InvalidInput;
    }
    if (plan.GetSize() > MAX_GENERATION_PLAN_STAGES)
    {
        return GenerationStatus::CapacityExceeded;
    }
    for (usize i = 0; i < plan.GetSize(); ++i)
    {
        const GenerationStage& stage = plan[i];
        if (stage.KernelOrNull == nullptr)
        {
            return GenerationStatus::InvalidInput;
        }
        if (stage.Parameters.GetSize() > MAX_GENERATION_STAGE_PARAMETER_BYTES ||
            stage.Inputs.GetSize() > MAX_GENERATION_STAGE_INPUTS)
        {
            return GenerationStatus::CapacityExceeded;
        }
        for (const usize input : stage.Inputs)
        {
            if (input >= i)
            {
                return GenerationStatus::InvalidInput;
            }
        }
        for (usize earlier = 0; earlier < i; ++earlier)
        {
            if (plan[earlier].Scope == stage.Scope && plan[earlier].Stage == stage.Stage)
            {
                return GenerationStatus::InvalidInput;
            }
        }
    }
    return GenerationStatus::Success;
}

bool TryComputeGenerationManifestDigest(BorrowedView<const GenerationProduct> products,
                                        GenerationDigest& outDigest) noexcept
{
    if (products.IsValid() == false)
    {
        return false;
    }
    GenerationByteWriter writer;
    writer.WriteUInt32(GENERATION_MANIFEST_VERSION);
    writer.WriteUInt32(static_cast<uint32>(products.GetSize()));
    for (const GenerationProduct& product : products)
    {
        writer.WriteUInt32(product.Scope);
        writer.WriteUInt32(product.Stage);
        writer.WriteDigest(product.Key);
        writer.WriteDigest(product.Output);
    }
    if (writer.HasFailed() == true)
    {
        return false;
    }
    outDigest = writer.ComputeDigest();
    return true;
}

GenerationCoordinator::GenerationCoordinator(GenerationCacheStore& inoutCacheStore) noexcept
    : mCacheStore(inoutCacheStore)
{
}

GenerationStatus GenerationCoordinator::TryBeginCandidate(BorrowedView<const GenerationStage> plan,
                                                          GenerationCandidate& outCandidate) noexcept
{
    GenerationCandidate candidate;
    candidate.BaseRevision = mAcceptedRevision;
    candidate.Status = ValidateGenerationPlan(plan);
    if (candidate.Status == GenerationStatus::Success &&
        (TryComputePlanDigest(plan, candidate.Plan) == false || candidate.Products.TryResize(plan.GetSize()) == false))
    {
        candidate.Status = GenerationStatus::OutOfMemory;
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        for (usize i = 0; i < plan.GetSize(); ++i)
        {
            candidate.Products[i].Scope = plan[i].Scope;
            candidate.Products[i].Stage = plan[i].Stage;
        }
    }
    else
    {
        candidate.Products.Clear();
    }
    outCandidate = foundation::Move(candidate);
    return outCandidate.Status;
}

GenerationStatus GenerationCoordinator::TryRequestScope(BorrowedView<const GenerationStage> plan,
                                                        uint32 scope,
                                                        GenerationTrace& inoutTrace,
                                                        GenerationCandidate& inoutCandidate) noexcept
{
    Array<bool> isRequested;
    if (plan.IsValid() == false || isRequested.TryResize(plan.GetSize(), false) == false)
    {
        return plan.IsValid() == false ? GenerationStatus::InvalidInput : GenerationStatus::OutOfMemory;
    }
    bool hasStage = false;
    for (usize i = 0; i < plan.GetSize(); ++i)
    {
        isRequested[i] = plan[i].Scope == scope;
        hasStage = hasStage == true || isRequested[i] == true;
    }
    if (hasStage == false)
    {
        return GenerationStatus::InvalidInput;
    }
    return requestStages(plan, {isRequested.GetData(), isRequested.GetSize()}, inoutTrace, inoutCandidate);
}

GenerationStatus GenerationCoordinator::TryRequestAll(BorrowedView<const GenerationStage> plan,
                                                      GenerationTrace& inoutTrace,
                                                      GenerationCandidate& inoutCandidate) noexcept
{
    Array<bool> isRequested;
    if (plan.IsValid() == false || isRequested.TryResize(plan.GetSize(), true) == false)
    {
        return plan.IsValid() == false ? GenerationStatus::InvalidInput : GenerationStatus::OutOfMemory;
    }
    return requestStages(plan, {isRequested.GetData(), isRequested.GetSize()}, inoutTrace, inoutCandidate);
}

GenerationStatus GenerationCoordinator::requestStages(BorrowedView<const GenerationStage> plan,
                                                      BorrowedView<const bool> isRequested,
                                                      GenerationTrace& inoutTrace,
                                                      GenerationCandidate& inoutCandidate) noexcept
{
    if (inoutCandidate.Status != GenerationStatus::Success)
    {
        return inoutCandidate.Status;
    }
    GenerationDigest planDigest;
    if (plan.GetSize() != inoutCandidate.Products.GetSize())
    {
        return GenerationStatus::InvalidInput;
    }
    if (TryComputePlanDigest(plan, planDigest) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    if (planDigest != inoutCandidate.Plan)
    {
        return GenerationStatus::InvalidInput;
    }
    // Propagate demand upstream: inputs precede their consumers, so one reverse pass closes the set.
    Array<bool> isDemanded;
    if (isDemanded.TryAddRange(isRequested) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    for (usize i = plan.GetSize(); i > 0; --i)
    {
        if (isDemanded[i - 1] == true)
        {
            for (const usize input : plan[i - 1].Inputs)
            {
                isDemanded[input] = true;
            }
        }
    }
    for (usize i = 0; i < plan.GetSize(); ++i)
    {
        if (isDemanded[i] == false || inoutCandidate.Products[i].State != GenerationProductState::Pending)
        {
            continue;
        }
        const GenerationStatus status = evaluateStage(plan[i], i, inoutTrace, inoutCandidate);
        if (status != GenerationStatus::Success)
        {
            inoutCandidate.Products[i].State = GenerationProductState::Failed;
            inoutCandidate.Status = status;
            return status;
        }
    }
    return GenerationStatus::Success;
}

GenerationStatus GenerationCoordinator::evaluateStage(const GenerationStage& stage,
                                                      usize index,
                                                      GenerationTrace& inoutTrace,
                                                      GenerationCandidate& inoutCandidate) noexcept
{
    // Inputs are ready: each precedes this stage and was demanded with it.
    foundation::StaticArray<BorrowedView<const uint8>, MAX_GENERATION_STAGE_INPUTS> inputViews{};
    mWriter.Clear();
    mWriter.WriteUInt32(GENERATION_STAGE_KEY_VERSION);
    WriteStageIdentity(stage, mWriter);
    mWriter.WriteUInt32(static_cast<uint32>(stage.Inputs.GetSize()));
    for (usize i = 0; i < stage.Inputs.GetSize(); ++i)
    {
        const GenerationProduct& input = inoutCandidate.Products[stage.Inputs[i]];
        inputViews[i] = {input.Bytes.GetData(), input.Bytes.GetSize()};
        mWriter.WriteUInt32(input.Scope);
        mWriter.WriteUInt32(input.Stage);
        mWriter.WriteDigest(input.Output);
    }
    if (mWriter.HasFailed() == true)
    {
        return GenerationStatus::OutOfMemory;
    }
    const GenerationInputProducts inputs{inputViews.GetData(), stage.Inputs.GetSize()};
    GenerationStageKernel& kernel = *stage.KernelOrNull;

    GenerationStageRecord record;
    record.Scope = stage.Scope;
    record.Stage = stage.Stage;
    record.Key = mWriter.ComputeDigest();
    Array<uint8> bytes;
    record.Cache = TryFindGenerationProduct(mCacheStore, record.Key, stage.Schema, bytes);
    if (record.Cache == GenerationCacheOutcome::Hit &&
        kernel.IsValidProduct(stage, inputs, {bytes.GetData(), bytes.GetSize()}) == false)
    {
        // The framing verified but the content does not fit the stage; discard it like any corrupt entry.
        mCacheStore.Remove(record.Key);
        record.Cache = GenerationCacheOutcome::Corrupt;
    }
    if (record.Cache == GenerationCacheOutcome::Hit)
    {
        record.Outcome = GenerationStageOutcome::CacheHit;
    }
    else
    {
        mWriter.Clear();
        GenerationStatus status = kernel.TryExecute(stage, inputs, mWriter);
        bytes.Clear();
        if (status == GenerationStatus::Success &&
            (mWriter.HasFailed() == true || bytes.TryAddRange(mWriter.GetBytes()) == false))
        {
            status = GenerationStatus::OutOfMemory;
        }
        if (status != GenerationStatus::Success)
        {
            record.Outcome = GenerationStageOutcome::Failed;
            inoutTrace.Record(record);
            return status;
        }
        record.Outcome = GenerationStageOutcome::Executed;
        record.IsStored = TryStoreGenerationProduct(mCacheStore, record.Key, stage.Schema, mWriter.GetBytes());
    }
    record.Output = ComputeGenerationDigest({bytes.GetData(), bytes.GetSize()});
    GenerationProduct& product = inoutCandidate.Products[index];
    product.Key = record.Key;
    product.Output = record.Output;
    product.Bytes = foundation::Move(bytes);
    product.State = GenerationProductState::Ready;
    inoutTrace.Record(record);
    return GenerationStatus::Success;
}

GenerationStatus GenerationCoordinator::TryPublish(GenerationCandidate& inoutCandidate) noexcept
{
    if (inoutCandidate.Status != GenerationStatus::Success || inoutCandidate.IsReady() == false)
    {
        return GenerationStatus::NotReady;
    }
    if (inoutCandidate.BaseRevision != mAcceptedRevision)
    {
        return GenerationStatus::StaleCandidate;
    }
    GenerationDigest digest;
    if (TryComputeGenerationManifestDigest({inoutCandidate.Products.GetData(), inoutCandidate.Products.GetSize()},
                                           digest) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    mAccepted.Products = foundation::Move(inoutCandidate.Products);
    mAccepted.Digest = digest;
    ++mAcceptedRevision;
    Reject(inoutCandidate);
    return GenerationStatus::Success;
}

void GenerationCoordinator::Reject(GenerationCandidate& inoutCandidate) noexcept
{
    inoutCandidate = GenerationCandidate{};
}
} // namespace ludus::content::procedural
