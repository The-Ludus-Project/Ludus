#include <ludus/content/procedural/environment/scatter_generation.h>

// Thanks to Kristian Hähnlein, Gilbert Louis Bernstein and Adriana Schulz, "DeCAD:
// Semantic Differencing of CAD Models", UIST 2024, sections 4.1-4.3 and 6.1,
// https://obikate.github.io/papers/decad.pdf. Their idea of tracing a difference
// between two model versions to the operation that produced it is adapted here as
// a comparison by persistent placement identity that names the first responsible
// scatter stage and the excluding input; Ludus derives the explanation by
// re-evaluating the deterministic kernel rather than from an edit history.

namespace ludus::content::procedural
{
namespace
{
using foundation::Array;
using foundation::BorrowedView;

/// Bytes of one placement record in a layer product: identity 8, lattice 8, variant 4, rotation 2, scale 2.
constexpr usize SCATTER_PLACEMENT_RECORD_BYTES = 24;
/// Bytes before the records: the placement count.
constexpr usize SCATTER_LAYER_PRODUCT_HEADER_BYTES = 4;

[[nodiscard]] GenerationStatus ToGenerationStatus(ScatterStatus status) noexcept
{
    switch (status)
    {
        case ScatterStatus::Success:
            return GenerationStatus::Success;
        case ScatterStatus::CapacityExceeded:
            return GenerationStatus::CapacityExceeded;
        case ScatterStatus::SamplingExhausted:
            return GenerationStatus::SearchExhausted;
        case ScatterStatus::InvalidRecipe:
        case ScatterStatus::InvalidRange:
            return GenerationStatus::InvalidInput;
    }
    return GenerationStatus::InvalidInput;
}

/// Canonical order within a layer: cell Y, cell X, candidate.
[[nodiscard]] bool IsCanonicallyBefore(const ScatterIdentity& a, const ScatterIdentity& b) noexcept
{
    if (a.CellY != b.CellY)
    {
        return a.CellY < b.CellY;
    }
    if (a.CellX != b.CellX)
    {
        return a.CellX < b.CellX;
    }
    return a.Candidate < b.Candidate;
}

[[nodiscard]] bool AreEqual(const ScatterPlacement& a, const ScatterPlacement& b) noexcept
{
    return a.Identity == b.Identity && a.LatticeX == b.LatticeX && a.LatticeY == b.LatticeY && a.Variant == b.Variant &&
           a.Rotation == b.Rotation && a.Scale == b.Scale;
}

[[nodiscard]] GenerationStatus ValidateInput(const ScatterGenerationInput& input, ScatterKeys& outKeys) noexcept
{
    if (input.Scopes.IsValid() == false || input.Scopes.IsEmpty() == true ||
        input.Scopes.GetSize() > MAX_SCATTER_GENERATION_SCOPES)
    {
        return GenerationStatus::InvalidInput;
    }
    for (usize i = 0; i < input.Scopes.GetSize(); ++i)
    {
        const ScatterScope& scope = input.Scopes[i];
        if (ValidateScatterRecipe(scope.Recipe).Status != ScatterStatus::Success ||
            (i > 0 && input.Scopes[i - 1].Recipe.Room >= scope.Recipe.Room) || scope.Locks.IsValid() == false ||
            scope.Locks.GetSize() > MAX_SCATTER_SCOPE_LOCKS)
        {
            return GenerationStatus::InvalidInput;
        }
        for (const ScatterPlacement& lock : scope.Locks)
        {
            if (lock.Identity.Room != scope.Recipe.Room)
            {
                return GenerationStatus::InvalidInput;
            }
        }
    }
    return TryMakeScatterKeys(input.WorldSeed, input.Domains, outKeys) == ScatterStatus::Success
               ? GenerationStatus::Success
               : GenerationStatus::InvalidInput;
}

void WriteRectangle(const ScatterRectangle& rectangle, GenerationByteWriter& outWriter) noexcept
{
    outWriter.WriteInt32(rectangle.MinimumX);
    outWriter.WriteInt32(rectangle.MinimumY);
    outWriter.WriteInt32(rectangle.MaximumX);
    outWriter.WriteInt32(rectangle.MaximumY);
}

[[nodiscard]] ScatterRectangle ReadRectangle(GenerationByteReader& inoutReader) noexcept
{
    ScatterRectangle rectangle;
    rectangle.MinimumX = inoutReader.ReadInt32();
    rectangle.MinimumY = inoutReader.ReadInt32();
    rectangle.MaximumX = inoutReader.ReadInt32();
    rectangle.MaximumY = inoutReader.ReadInt32();
    return rectangle;
}

/// A layer stage's parameters: every input the layer product depends on. Other layers and locks are excluded, so
/// editing them never invalidates it. The operator, its version and the schema are in the stage key already.
void WriteLayerParameters(const ScatterGenerationInput& input,
                          const ScatterRecipe& recipe,
                          const ScatterLayer& layer,
                          GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(SCATTER_LAYOUT_VERSION);
    outWriter.WriteUInt64(input.WorldSeed);
    outWriter.WriteUInt32(input.Domains.Placement);
    outWriter.WriteUInt32(input.Domains.Variant);
    outWriter.WriteUInt32(input.Domains.Transform);
    outWriter.WriteUInt16(recipe.Room);
    outWriter.WriteUInt8(layer.Id);
    outWriter.WriteUInt16(layer.CellSize);
    outWriter.WriteUInt16(layer.CandidatesPerCell);
    outWriter.WriteUInt16(layer.MinimumScale);
    outWriter.WriteUInt16(layer.MaximumScale);
    outWriter.WriteUInt32(static_cast<uint32>(layer.Variants.GetSize()));
    for (const ScatterVariant& variant : layer.Variants)
    {
        outWriter.WriteUInt32(variant.ExclusionRadius);
    }
    WriteRectangle(recipe.Region, outWriter);
    outWriter.WriteUInt32(static_cast<uint32>(recipe.KeepOuts.GetSize()));
    for (const ScatterRectangle& keepOut : recipe.KeepOuts)
    {
        WriteRectangle(keepOut, outWriter);
    }
}

void WriteLayerProduct(BorrowedView<const ScatterPlacement> placements, GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(static_cast<uint32>(placements.GetSize()));
    for (const ScatterPlacement& placement : placements)
    {
        outWriter.WriteUInt16(placement.Identity.Room);
        outWriter.WriteUInt8(placement.Identity.Layer);
        outWriter.WriteUInt8(placement.Identity.Candidate);
        outWriter.WriteInt16(placement.Identity.CellX);
        outWriter.WriteInt16(placement.Identity.CellY);
        outWriter.WriteInt32(placement.LatticeX);
        outWriter.WriteInt32(placement.LatticeY);
        outWriter.WriteUInt32(placement.Variant);
        outWriter.WriteUInt16(placement.Rotation);
        outWriter.WriteUInt16(placement.Scale);
    }
}

/// Decode a layer product's placements and append them: every record in the first record's room and layer, in
/// strictly canonical order. False leaves the array unchanged.
[[nodiscard]] bool TryAppendLayerPlacements(BorrowedView<const uint8> bytes,
                                            Array<ScatterPlacement>& inoutPlacements) noexcept
{
    GenerationByteReader reader(bytes);
    const uint32 count = reader.ReadUInt32();
    if (reader.HasFailed() == true || count > MAX_SCATTER_LAYER_CANDIDATES ||
        bytes.GetSize() !=
            SCATTER_LAYER_PRODUCT_HEADER_BYTES + (static_cast<usize>(count) * SCATTER_PLACEMENT_RECORD_BYTES) ||
        inoutPlacements.TryEnsureCapacity(inoutPlacements.GetSize() + count) == false)
    {
        return false;
    }
    const usize first = inoutPlacements.GetSize();
    for (uint32 i = 0; i < count; ++i)
    {
        ScatterPlacement placement;
        placement.Identity.Room = reader.ReadUInt16();
        placement.Identity.Layer = reader.ReadUInt8();
        placement.Identity.Candidate = reader.ReadUInt8();
        placement.Identity.CellX = reader.ReadInt16();
        placement.Identity.CellY = reader.ReadInt16();
        placement.LatticeX = reader.ReadInt32();
        placement.LatticeY = reader.ReadInt32();
        placement.Variant = reader.ReadUInt32();
        placement.Rotation = reader.ReadUInt16();
        placement.Scale = reader.ReadUInt16();
        const bool isOrdered = i == 0 || (placement.Identity.Room == inoutPlacements.GetLast().Identity.Room &&
                                          placement.Identity.Layer == inoutPlacements.GetLast().Identity.Layer &&
                                          IsCanonicallyBefore(inoutPlacements.GetLast().Identity, placement.Identity));
        if (isOrdered == false)
        {
            inoutPlacements.RemoveRange(first, inoutPlacements.GetSize());
            return false;
        }
        inoutPlacements.Add(placement);
    }
    if (reader.IsAtEnd() == false)
    {
        inoutPlacements.RemoveRange(first, inoutPlacements.GetSize());
        return false;
    }
    return true;
}

/// Generate one layer into the manifest's placements.
[[nodiscard]] GenerationStatus TryGenerateLayer(const ScatterRecipe& recipe,
                                                const ScatterKeys& keys,
                                                usize layerIndex,
                                                Array<ScatterPlacement>& inoutScratch,
                                                Array<ScatterPlacement>& inoutPlacements) noexcept
{
    ScatterCellRange cells;
    usize candidateCount = 0;
    ScatterStatus status = TryGetScatterCells(recipe, layerIndex, cells);
    if (status == ScatterStatus::Success)
    {
        status = TryCountScatterCandidates(recipe, layerIndex, cells, candidateCount);
    }
    if (status != ScatterStatus::Success)
    {
        return ToGenerationStatus(status);
    }
    if (candidateCount > MAX_SCATTER_LAYER_CANDIDATES)
    {
        return GenerationStatus::CapacityExceeded;
    }
    if (inoutScratch.TryResize(candidateCount) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    usize count = 0;
    status = TryGenerateScatter(recipe, keys, layerIndex, cells, {inoutScratch.GetData(), candidateCount}, count);
    if (status != ScatterStatus::Success)
    {
        return ToGenerationStatus(status);
    }
    return inoutPlacements.TryAddRange({inoutScratch.GetData(), count}) == true ? GenerationStatus::Success
                                                                                : GenerationStatus::OutOfMemory;
}

/// Explain why the input's candidate lacks an identity.
[[nodiscard]] GenerationStatus ExplainAbsence(const ScatterGenerationInput& input,
                                              const ScatterKeys& keys,
                                              ScatterIdentity identity,
                                              ScatterDecision& outDecision) noexcept
{
    ScatterDecision decision;
    decision.Stage = ScatterDecisionStage::Recipe;
    const ScatterScope* scopeOrNull = nullptr;
    for (const ScatterScope& scope : input.Scopes)
    {
        if (scope.Recipe.Room == identity.Room)
        {
            scopeOrNull = &scope;
        }
    }
    if (scopeOrNull == nullptr)
    {
        decision.Rule = ScatterDecisionRule::ScopeRemoved;
        outDecision = decision;
        return GenerationStatus::Success;
    }
    const ScatterRecipe& recipe = scopeOrNull->Recipe;
    for (usize layer = 0; layer < recipe.Layers.GetSize(); ++layer)
    {
        if (recipe.Layers[layer].Id == identity.Layer)
        {
            return ToGenerationStatus(TryExplainScatterCandidate(recipe, keys, layer, identity, outDecision));
        }
    }
    decision.Rule = ScatterDecisionRule::LayerRemoved;
    outDecision = decision;
    return GenerationStatus::Success;
}

/// Decode a fully staged generation candidate into a typed manifest.
[[nodiscard]] GenerationStatus TryBuildManifest(const GenerationCandidate& generation,
                                                ScatterManifest& outManifest) noexcept
{
    ScatterManifest manifest;
    if (manifest.Layers.TryEnsureCapacity(generation.Products.GetSize()) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    for (const GenerationProduct& product : generation.Products)
    {
        ScatterManifestLayer layer;
        layer.Room = static_cast<uint16>(product.Scope);
        layer.Layer = static_cast<uint8>(product.Stage);
        layer.Key = product.Key;
        layer.Output = product.Output;
        layer.FirstPlacement = manifest.Placements.GetSize();
        if (TryAppendLayerPlacements({product.Bytes.GetData(), product.Bytes.GetSize()}, manifest.Placements) == false)
        {
            // The kernel wrote or validated every product, so only allocation can fail here.
            return GenerationStatus::OutOfMemory;
        }
        layer.PlacementCount = manifest.Placements.GetSize() - layer.FirstPlacement;
        manifest.Layers.Add(layer);
    }
    if (TryComputeGenerationManifestDigest({generation.Products.GetData(), generation.Products.GetSize()},
                                           manifest.Digest) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    outManifest = foundation::Move(manifest);
    return GenerationStatus::Success;
}

[[nodiscard]] GenerationStatus
TryCheckLocks(const ScatterGenerationInput& input, const ScatterKeys& keys, ScatterCandidate& inoutCandidate) noexcept
{
    for (const ScatterScope& scope : input.Scopes)
    {
        for (const ScatterPlacement& lock : scope.Locks)
        {
            const ScatterPlacement* generatedOrNull = inoutCandidate.Manifest.FindPlacement(lock.Identity);
            if (generatedOrNull != nullptr && AreEqual(*generatedOrNull, lock) == true)
            {
                continue;
            }
            ScatterLockConflict conflict;
            conflict.Lock = lock;
            conflict.IsMissing = generatedOrNull == nullptr;
            if (generatedOrNull != nullptr)
            {
                conflict.Generated = *generatedOrNull;
            }
            else
            {
                const GenerationStatus status = ExplainAbsence(input, keys, lock.Identity, conflict.Decision);
                if (status != GenerationStatus::Success)
                {
                    return status;
                }
            }
            if (inoutCandidate.Conflicts.TryAdd(conflict) == false)
            {
                return GenerationStatus::OutOfMemory;
            }
        }
    }
    return inoutCandidate.Conflicts.IsEmpty() == true ? GenerationStatus::Success : GenerationStatus::ConstraintFailed;
}
} // namespace

const ScatterPlacement* ScatterManifest::FindPlacement(ScatterIdentity identity) const noexcept
{
    for (const ScatterManifestLayer& layer : Layers)
    {
        if (layer.Room != identity.Room || layer.Layer != identity.Layer)
        {
            continue;
        }
        usize low = layer.FirstPlacement;
        usize high = layer.FirstPlacement + layer.PlacementCount;
        while (low < high)
        {
            const usize middle = low + ((high - low) / 2);
            if (IsCanonicallyBefore(Placements[middle].Identity, identity) == true)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        const usize end = layer.FirstPlacement + layer.PlacementCount;
        return low < end && Placements[low].Identity == identity ? &Placements[low] : nullptr;
    }
    return nullptr;
}

bool ScatterLayerKernel::tryDecodeParameters(const GenerationStage& stage) noexcept
{
    GenerationByteReader reader({stage.Parameters.GetData(), stage.Parameters.GetSize()});
    const uint32 layout = reader.ReadUInt32();
    const uint64 worldSeed = reader.ReadUInt64();
    ScatterDomains domains;
    domains.Placement = reader.ReadUInt32();
    domains.Variant = reader.ReadUInt32();
    domains.Transform = reader.ReadUInt32();
    mRecipe.Room = reader.ReadUInt16();
    mLayer.Id = reader.ReadUInt8();
    mLayer.CellSize = reader.ReadUInt16();
    mLayer.CandidatesPerCell = reader.ReadUInt16();
    mLayer.MinimumScale = reader.ReadUInt16();
    mLayer.MaximumScale = reader.ReadUInt16();
    const uint32 variantCount = reader.ReadUInt32();
    if (reader.HasFailed() == true || layout != SCATTER_LAYOUT_VERSION || variantCount > MAX_SCATTER_VARIANTS ||
        mVariants.TryResize(variantCount) == false)
    {
        return false;
    }
    for (ScatterVariant& variant : mVariants)
    {
        variant.ExclusionRadius = reader.ReadUInt32();
    }
    mRecipe.Region = ReadRectangle(reader);
    const uint32 keepOutCount = reader.ReadUInt32();
    if (reader.HasFailed() == true || keepOutCount > MAX_SCATTER_KEEP_OUTS ||
        mKeepOuts.TryResize(keepOutCount) == false)
    {
        return false;
    }
    for (ScatterRectangle& keepOut : mKeepOuts)
    {
        keepOut = ReadRectangle(reader);
    }
    mLayer.Variants = {mVariants.GetData(), mVariants.GetSize()};
    mRecipe.KeepOuts = {mKeepOuts.GetData(), mKeepOuts.GetSize()};
    mRecipe.Layers = {&mLayer, 1};
    return reader.IsAtEnd() == true && mRecipe.Room == stage.Scope && mLayer.Id == stage.Stage &&
           ValidateScatterRecipe(mRecipe).Status == ScatterStatus::Success &&
           TryMakeScatterKeys(worldSeed, domains, mKeys) == ScatterStatus::Success;
}

GenerationStatus ScatterLayerKernel::TryExecute(const GenerationStage& stage,
                                                GenerationInputProducts inputs,
                                                GenerationByteWriter& outProduct) noexcept
{
    if (inputs.IsEmpty() == false || tryDecodeParameters(stage) == false)
    {
        return GenerationStatus::InvalidInput;
    }
    mPlacements.Clear();
    const GenerationStatus status = TryGenerateLayer(mRecipe, mKeys, 0, mScratch, mPlacements);
    if (status != GenerationStatus::Success)
    {
        return status;
    }
    WriteLayerProduct({mPlacements.GetData(), mPlacements.GetSize()}, outProduct);
    return outProduct.HasFailed() == true ? GenerationStatus::OutOfMemory : GenerationStatus::Success;
}

bool ScatterLayerKernel::IsValidProduct(const GenerationStage& stage,
                                        GenerationInputProducts inputs,
                                        BorrowedView<const uint8> product) noexcept
{
    mPlacements.Clear();
    if (inputs.IsEmpty() == false || tryDecodeParameters(stage) == false ||
        TryAppendLayerPlacements(product, mPlacements) == false)
    {
        return false;
    }
    for (const ScatterPlacement& placement : mPlacements)
    {
        if (placement.Identity.Room != mRecipe.Room || placement.Identity.Layer != mLayer.Id ||
            placement.Identity.Candidate >= mLayer.CandidatesPerCell || placement.Variant >= mVariants.GetSize() ||
            placement.Scale < mLayer.MinimumScale || placement.Scale > mLayer.MaximumScale)
        {
            return false;
        }
    }
    return true;
}

GenerationStatus TryAddScatterStages(const ScatterGenerationInput& input,
                                     ScatterLayerKernel& inoutKernel,
                                     Array<GenerationStage>& inoutPlan) noexcept
{
    ScatterKeys keys;
    const GenerationStatus inputStatus = ValidateInput(input, keys);
    if (inputStatus != GenerationStatus::Success)
    {
        return inputStatus;
    }
    const usize first = inoutPlan.GetSize();
    GenerationByteWriter writer;
    for (const ScatterScope& scope : input.Scopes)
    {
        for (const ScatterLayer& layer : scope.Recipe.Layers)
        {
            WriteLayerParameters(input, scope.Recipe, layer, writer);
            GenerationStage stage;
            stage.Scope = scope.Recipe.Room;
            stage.Stage = layer.Id;
            stage.Operator = SCATTER_LAYER_OPERATOR;
            stage.OperatorVersion = SCATTER_GENERATION_VERSION;
            stage.Schema = SCATTER_LAYER_PRODUCT_SCHEMA;
            stage.KernelOrNull = &inoutKernel;
            if (writer.HasFailed() == true || stage.Parameters.TryAddRange(writer.GetBytes()) == false ||
                inoutPlan.TryAdd(foundation::Move(stage)) == false)
            {
                inoutPlan.RemoveRange(first, inoutPlan.GetSize());
                return GenerationStatus::OutOfMemory;
            }
        }
    }
    return GenerationStatus::Success;
}

bool TryReadScatterLayerProduct(BorrowedView<const uint8> product, Array<ScatterPlacement>& outPlacements) noexcept
{
    Array<ScatterPlacement> placements;
    if (product.IsValid() == false || TryAppendLayerPlacements(product, placements) == false)
    {
        return false;
    }
    outPlacements = foundation::Move(placements);
    return true;
}

ScatterGenerationCoordinator::ScatterGenerationCoordinator(GenerationCacheStore& inoutCacheStore) noexcept
    : mGeneration(inoutCacheStore)
{
}

GenerationStatus ScatterGenerationCoordinator::TryStage(const ScatterGenerationInput& input,
                                                        GenerationTrace& inoutTrace,
                                                        ScatterCandidate& outCandidate) noexcept
{
    ScatterCandidate candidate;
    candidate.BaseRevision = mGeneration.GetAcceptedRevision();
    ScatterKeys keys;
    candidate.Status = ValidateInput(input, keys);
    Array<GenerationStage> plan;
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = TryAddScatterStages(input, mKernel, plan);
    }
    const BorrowedView<const GenerationStage> planView{plan.GetData(), plan.GetSize()};
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = mGeneration.TryBeginCandidate(planView, candidate.Generation);
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = mGeneration.TryRequestAll(planView, inoutTrace, candidate.Generation);
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = TryBuildManifest(candidate.Generation, candidate.Manifest);
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = TryCheckLocks(input, keys, candidate);
    }
    if (candidate.Status != GenerationStatus::Success && candidate.Status != GenerationStatus::ConstraintFailed)
    {
        candidate.Manifest = ScatterManifest{};
        candidate.Conflicts.Clear();
        mGeneration.Reject(candidate.Generation);
    }
    outCandidate = foundation::Move(candidate);
    return outCandidate.Status;
}

GenerationStatus ScatterGenerationCoordinator::TryPublish(ScatterCandidate& inoutCandidate) noexcept
{
    if (inoutCandidate.Status != GenerationStatus::Success)
    {
        return GenerationStatus::NotReady;
    }
    const GenerationStatus status = mGeneration.TryPublish(inoutCandidate.Generation);
    if (status != GenerationStatus::Success)
    {
        return status;
    }
    mAccepted = foundation::Move(inoutCandidate.Manifest);
    Reject(inoutCandidate);
    return GenerationStatus::Success;
}

void ScatterGenerationCoordinator::Reject(ScatterCandidate& inoutCandidate) noexcept
{
    inoutCandidate = ScatterCandidate{};
}

GenerationStatus TryCompareScatterManifests(const ScatterManifest& accepted,
                                            const ScatterManifest& candidate,
                                            const ScatterGenerationInput& candidateInput,
                                            Array<ScatterChange>& outChanges) noexcept
{
    ScatterKeys keys;
    const GenerationStatus inputStatus = ValidateInput(candidateInput, keys);
    if (inputStatus != GenerationStatus::Success)
    {
        return inputStatus;
    }
    Array<ScatterChange> changes;
    for (const ScatterPlacement& placement : accepted.Placements)
    {
        const ScatterPlacement* generatedOrNull = candidate.FindPlacement(placement.Identity);
        if (generatedOrNull != nullptr && AreEqual(*generatedOrNull, placement) == true)
        {
            continue;
        }
        ScatterChange change;
        change.Identity = placement.Identity;
        change.Kind = generatedOrNull == nullptr ? ScatterChangeKind::Removed : ScatterChangeKind::Changed;
        if (generatedOrNull == nullptr)
        {
            const GenerationStatus status = ExplainAbsence(candidateInput, keys, placement.Identity, change.Decision);
            if (status != GenerationStatus::Success)
            {
                return status;
            }
        }
        if (changes.TryAdd(change) == false)
        {
            return GenerationStatus::OutOfMemory;
        }
    }
    for (const ScatterPlacement& placement : candidate.Placements)
    {
        if (accepted.FindPlacement(placement.Identity) != nullptr)
        {
            continue;
        }
        ScatterChange change;
        change.Kind = ScatterChangeKind::Added;
        change.Identity = placement.Identity;
        if (changes.TryAdd(change) == false)
        {
            return GenerationStatus::OutOfMemory;
        }
    }
    outChanges = foundation::Move(changes);
    return GenerationStatus::Success;
}
} // namespace ludus::content::procedural
