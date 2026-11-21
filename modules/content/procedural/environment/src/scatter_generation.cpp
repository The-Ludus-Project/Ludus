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

/// Every input a layer product depends on: operator, versions, seed, domains, room, the layer and its variants,
/// the region and the keep-outs. Other layers and locks are excluded, so editing them never invalidates it.
void WriteLayerKey(const ScatterGenerationInput& input,
                   const ScatterRecipe& recipe,
                   const ScatterLayer& layer,
                   GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(SCATTER_GENERATION_VERSION);
    outWriter.WriteUInt32(SCATTER_LAYER_OPERATOR);
    outWriter.WriteUInt32(SCATTER_LAYOUT_VERSION);
    outWriter.WriteUInt32(SCATTER_LAYER_PRODUCT_SCHEMA);
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

/// Decode a cached layer product and check it against the layer: identities in this room and layer, strictly
/// canonical order, candidate, variant and scale in range. False rejects the entry as corrupt.
[[nodiscard]] bool TryReadLayerProduct(BorrowedView<const uint8> bytes,
                                       const ScatterRecipe& recipe,
                                       const ScatterLayer& layer,
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
        const bool isOrdered = i == 0 || IsCanonicallyBefore(inoutPlacements.GetLast().Identity, placement.Identity);
        if (placement.Identity.Room != recipe.Room || placement.Identity.Layer != layer.Id ||
            placement.Identity.Candidate >= layer.CandidatesPerCell || placement.Variant >= layer.Variants.GetSize() ||
            placement.Scale < layer.MinimumScale || placement.Scale > layer.MaximumScale || isOrdered == false)
        {
            inoutPlacements.RemoveRange(first, inoutPlacements.GetSize());
            return false;
        }
        inoutPlacements.Add(placement);
    }
    return reader.IsAtEnd() == true;
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

void WriteManifestDigest(BorrowedView<const ScatterManifestLayer> layers, GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(SCATTER_GENERATION_VERSION);
    outWriter.WriteUInt32(static_cast<uint32>(layers.GetSize()));
    for (const ScatterManifestLayer& layer : layers)
    {
        outWriter.WriteUInt16(layer.Room);
        outWriter.WriteUInt8(layer.Layer);
        outWriter.WriteDigest(layer.Key);
        outWriter.WriteDigest(layer.Output);
    }
}

/// Stage one layer: look up its product, else generate and store it; append it to the manifest.
[[nodiscard]] GenerationStatus TryStageLayer(GenerationCacheStore& inoutStore,
                                             const ScatterGenerationInput& input,
                                             const ScatterKeys& keys,
                                             const ScatterRecipe& recipe,
                                             usize layerIndex,
                                             GenerationTrace& inoutTrace,
                                             Array<ScatterPlacement>& inoutScratch,
                                             ScatterManifest& inoutManifest) noexcept
{
    const ScatterLayer& layer = recipe.Layers[layerIndex];
    GenerationByteWriter writer;
    WriteLayerKey(input, recipe, layer, writer);
    if (writer.HasFailed() == true)
    {
        return GenerationStatus::OutOfMemory;
    }
    ScatterManifestLayer product;
    product.Room = recipe.Room;
    product.Layer = layer.Id;
    product.Key = writer.ComputeDigest();
    product.FirstPlacement = inoutManifest.Placements.GetSize();

    GenerationStageRecord record;
    record.Scope = recipe.Room;
    record.Stage = layer.Id;
    record.Key = product.Key;
    Array<uint8> cached;
    record.Cache = TryFindGenerationProduct(inoutStore, product.Key, SCATTER_LAYER_PRODUCT_SCHEMA, cached);
    if (record.Cache == GenerationCacheOutcome::Hit &&
        TryReadLayerProduct({cached.GetData(), cached.GetSize()}, recipe, layer, inoutManifest.Placements) == false)
    {
        // The framing verified but the content does not fit the layer; discard it like any corrupt entry.
        inoutStore.Remove(product.Key);
        record.Cache = GenerationCacheOutcome::Corrupt;
    }
    if (record.Cache == GenerationCacheOutcome::Hit)
    {
        record.Outcome = GenerationStageOutcome::CacheHit;
        record.Output = ComputeGenerationDigest({cached.GetData(), cached.GetSize()});
    }
    else
    {
        const GenerationStatus status =
            TryGenerateLayer(recipe, keys, layerIndex, inoutScratch, inoutManifest.Placements);
        if (status != GenerationStatus::Success)
        {
            record.Outcome = GenerationStageOutcome::Failed;
            inoutTrace.Record(record);
            return status;
        }
        WriteLayerProduct({inoutManifest.Placements.GetData() + product.FirstPlacement,
                           inoutManifest.Placements.GetSize() - product.FirstPlacement},
                          writer);
        if (writer.HasFailed() == true)
        {
            return GenerationStatus::OutOfMemory;
        }
        record.Outcome = GenerationStageOutcome::Executed;
        record.Output = writer.ComputeDigest();
        record.IsStored =
            TryStoreGenerationProduct(inoutStore, product.Key, SCATTER_LAYER_PRODUCT_SCHEMA, writer.GetBytes());
    }
    product.Output = record.Output;
    product.PlacementCount = inoutManifest.Placements.GetSize() - product.FirstPlacement;
    inoutTrace.Record(record);
    return inoutManifest.Layers.TryAdd(product) == true ? GenerationStatus::Success : GenerationStatus::OutOfMemory;
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

ScatterGenerationCoordinator::ScatterGenerationCoordinator(GenerationCacheStore& inoutCacheStore) noexcept
    : mCacheStore(inoutCacheStore)
{
}

GenerationStatus ScatterGenerationCoordinator::TryStage(const ScatterGenerationInput& input,
                                                        GenerationTrace& inoutTrace,
                                                        ScatterCandidate& outCandidate) noexcept
{
    ScatterCandidate candidate;
    candidate.BaseRevision = mAcceptedRevision;
    ScatterKeys keys;
    candidate.Status = ValidateInput(input, keys);
    Array<ScatterPlacement> scratch;
    for (usize scope = 0; scope < input.Scopes.GetSize() && candidate.Status == GenerationStatus::Success; ++scope)
    {
        const ScatterRecipe& recipe = input.Scopes[scope].Recipe;
        for (usize layer = 0; layer < recipe.Layers.GetSize() && candidate.Status == GenerationStatus::Success; ++layer)
        {
            candidate.Status =
                TryStageLayer(mCacheStore, input, keys, recipe, layer, inoutTrace, scratch, candidate.Manifest);
        }
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        GenerationByteWriter writer;
        WriteManifestDigest({candidate.Manifest.Layers.GetData(), candidate.Manifest.Layers.GetSize()}, writer);
        candidate.Manifest.Digest = writer.ComputeDigest();
        candidate.Status =
            writer.HasFailed() == true ? GenerationStatus::OutOfMemory : TryCheckLocks(input, keys, candidate);
    }
    if (candidate.Status != GenerationStatus::Success && candidate.Status != GenerationStatus::ConstraintFailed)
    {
        candidate.Manifest = ScatterManifest{};
        candidate.Conflicts.Clear();
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
    if (inoutCandidate.BaseRevision != mAcceptedRevision)
    {
        return GenerationStatus::StaleCandidate;
    }
    mAccepted = foundation::Move(inoutCandidate.Manifest);
    ++mAcceptedRevision;
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
