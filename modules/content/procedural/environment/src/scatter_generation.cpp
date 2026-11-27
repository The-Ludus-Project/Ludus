#include <ludus/content/procedural/environment/scatter_generation.h>

#include <ludus/foundation/containers/sort.hpp>

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

/// Lock order across a request: room, layer, then canonical identity order.
struct LockOrder final
{
    [[nodiscard]] bool operator()(const ScatterPlacement& a, const ScatterPlacement& b) const noexcept
    {
        if (a.Identity.Room != b.Identity.Room)
        {
            return a.Identity.Room < b.Identity.Room;
        }
        if (a.Identity.Layer != b.Identity.Layer)
        {
            return a.Identity.Layer < b.Identity.Layer;
        }
        return IsCanonicallyBefore(a.Identity, b.Identity);
    }
};

/// The contiguous locks of one room and layer within locks in LockOrder.
[[nodiscard]] BorrowedView<const ScatterPlacement>
GetLayerLocks(BorrowedView<const ScatterPlacement> locks, uint16 room, uint8 layer) noexcept
{
    usize first = 0;
    while (first < locks.GetSize() && (locks[first].Identity.Room < room ||
                                       (locks[first].Identity.Room == room && locks[first].Identity.Layer < layer)))
    {
        ++first;
    }
    usize end = first;
    while (end < locks.GetSize() && locks[end].Identity.Room == room && locks[end].Identity.Layer == layer)
    {
        ++end;
    }
    // An empty share has no element to address.
    return first == end ? BorrowedView<const ScatterPlacement>{}
                        : BorrowedView<const ScatterPlacement>{&locks[first], end - first};
}

/// Validate a request, derive its keys and gather every scope's locks in LockOrder. Outputs are unchanged on failure.
[[nodiscard]] GenerationStatus
ValidateInput(const ScatterGenerationInput& input, ScatterKeys& outKeys, Array<ScatterPlacement>& outLocks) noexcept
{
    if (input.Scopes.IsValid() == false || input.Scopes.IsEmpty() == true ||
        input.Scopes.GetSize() > MAX_SCATTER_GENERATION_SCOPES)
    {
        return GenerationStatus::InvalidInput;
    }
    Array<ScatterPlacement> locks;
    for (usize i = 0; i < input.Scopes.GetSize(); ++i)
    {
        const ScatterScope& scope = input.Scopes[i];
        if (ValidateScatterRecipe(scope.Recipe).Status != ScatterStatus::Success ||
            (i > 0 && input.Scopes[i - 1].Recipe.Room >= scope.Recipe.Room) || scope.Locks.IsValid() == false ||
            scope.Locks.GetSize() > MAX_SCATTER_SCOPE_LOCKS)
        {
            return GenerationStatus::InvalidInput;
        }
        for (const ScatterLayer& layer : scope.Recipe.Layers)
        {
            if (layer.Locks.IsEmpty() == false)
            {
                return GenerationStatus::InvalidInput;
            }
        }
        for (const ScatterPlacement& lock : scope.Locks)
        {
            if (lock.Identity.Room != scope.Recipe.Room)
            {
                return GenerationStatus::InvalidInput;
            }
        }
        // Scopes ascend by room, so sorting each scope's share leaves the whole array in LockOrder.
        const usize first = locks.GetSize();
        if (locks.TryAddRange(scope.Locks) == false)
        {
            return GenerationStatus::OutOfMemory;
        }
        foundation::Sort(locks.GetData() + first, scope.Locks.GetSize(), LockOrder{});
        for (usize lock = first + 1; lock < locks.GetSize(); ++lock)
        {
            if (locks[lock - 1].Identity == locks[lock].Identity)
            {
                return GenerationStatus::InvalidInput;
            }
        }
    }
    ScatterKeys keys;
    if (TryMakeScatterKeys(input.WorldSeed, input.Domains, keys) != ScatterStatus::Success)
    {
        return GenerationStatus::InvalidInput;
    }
    outKeys = keys;
    outLocks = foundation::Move(locks);
    return GenerationStatus::Success;
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

/// One placement record: identity 8, lattice 8, variant 4, rotation 2, scale 2 bytes.
void WritePlacement(const ScatterPlacement& placement, GenerationByteWriter& outWriter) noexcept
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

[[nodiscard]] ScatterPlacement ReadPlacement(GenerationByteReader& inoutReader) noexcept
{
    ScatterPlacement placement;
    placement.Identity.Room = inoutReader.ReadUInt16();
    placement.Identity.Layer = inoutReader.ReadUInt8();
    placement.Identity.Candidate = inoutReader.ReadUInt8();
    placement.Identity.CellX = inoutReader.ReadInt16();
    placement.Identity.CellY = inoutReader.ReadInt16();
    placement.LatticeX = inoutReader.ReadInt32();
    placement.LatticeY = inoutReader.ReadInt32();
    placement.Variant = inoutReader.ReadUInt32();
    placement.Rotation = inoutReader.ReadUInt16();
    placement.Scale = inoutReader.ReadUInt16();
    return placement;
}

/// A layer's own fields and variants.
void WriteLayerFields(const ScatterLayer& layer, GenerationByteWriter& outWriter) noexcept
{
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
}

void WritePlacements(BorrowedView<const ScatterPlacement> placements, GenerationByteWriter& outWriter) noexcept
{
    outWriter.WriteUInt32(static_cast<uint32>(placements.GetSize()));
    for (const ScatterPlacement& placement : placements)
    {
        WritePlacement(placement, outWriter);
    }
}

void WriteRegionAndKeepOuts(const ScatterRecipe& recipe, GenerationByteWriter& outWriter) noexcept
{
    WriteRectangle(recipe.Region, outWriter);
    outWriter.WriteUInt32(static_cast<uint32>(recipe.KeepOuts.GetSize()));
    for (const ScatterRectangle& keepOut : recipe.KeepOuts)
    {
        WriteRectangle(keepOut, outWriter);
    }
}

/// A layer stage's parameters: every input the layer product depends on, its locks in canonical order included.
/// Other layers and their locks are excluded, so editing them never invalidates it. The operator, its version and
/// the schema are in the stage key already.
void WriteLayerParameters(const ScatterGenerationInput& input,
                          const ScatterRecipe& recipe,
                          const ScatterLayer& layer,
                          BorrowedView<const ScatterPlacement> layerLocks,
                          GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(SCATTER_LAYOUT_VERSION);
    outWriter.WriteUInt64(input.WorldSeed);
    outWriter.WriteUInt32(input.Domains.Placement);
    outWriter.WriteUInt32(input.Domains.Variant);
    outWriter.WriteUInt32(input.Domains.Transform);
    outWriter.WriteUInt16(recipe.Room);
    WriteLayerFields(layer, outWriter);
    WritePlacements(layerLocks, outWriter);
    WriteRegionAndKeepOuts(recipe, outWriter);
}

void WriteLayerProduct(BorrowedView<const ScatterPlacement> placements, GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    WritePlacements(placements, outWriter);
}

/// The request's canonical encoding: every authored input of every scope, then every lock in LockOrder.
void WriteInput(const ScatterGenerationInput& input,
                BorrowedView<const ScatterPlacement> locks,
                GenerationByteWriter& outWriter) noexcept
{
    outWriter.Clear();
    outWriter.WriteUInt32(SCATTER_INPUT_DIGEST_VERSION);
    outWriter.WriteUInt32(SCATTER_LAYOUT_VERSION);
    outWriter.WriteUInt64(input.WorldSeed);
    outWriter.WriteUInt32(input.Domains.Placement);
    outWriter.WriteUInt32(input.Domains.Variant);
    outWriter.WriteUInt32(input.Domains.Transform);
    outWriter.WriteUInt32(static_cast<uint32>(input.Scopes.GetSize()));
    for (const ScatterScope& scope : input.Scopes)
    {
        outWriter.WriteUInt16(scope.Recipe.Room);
        WriteRegionAndKeepOuts(scope.Recipe, outWriter);
        outWriter.WriteUInt32(static_cast<uint32>(scope.Recipe.Layers.GetSize()));
        for (const ScatterLayer& layer : scope.Recipe.Layers)
        {
            WriteLayerFields(layer, outWriter);
        }
    }
    WritePlacements(locks, outWriter);
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
        const ScatterPlacement placement = ReadPlacement(reader);
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

/// Explain why the input's candidate lacks an identity, evaluating its layer with the request's locks.
[[nodiscard]] GenerationStatus ExplainAbsence(const ScatterGenerationInput& input,
                                              const ScatterKeys& keys,
                                              BorrowedView<const ScatterPlacement> locks,
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
            ScatterLayer lockedLayer = recipe.Layers[layer];
            lockedLayer.Locks = GetLayerLocks(locks, recipe.Room, lockedLayer.Id);
            ScatterRecipe lockedRecipe = recipe;
            lockedRecipe.Layers = {&lockedLayer, 1};
            return ToGenerationStatus(TryExplainScatterCandidate(lockedRecipe, keys, 0, identity, outDecision));
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

/// Record every lock the candidate's manifest does not satisfy, in LockOrder.
[[nodiscard]] GenerationStatus
TryCheckLocks(const ScatterGenerationInput& input, const ScatterKeys& keys, ScatterCandidate& inoutCandidate) noexcept
{
    const BorrowedView<const ScatterPlacement> locks{inoutCandidate.Locks.GetData(), inoutCandidate.Locks.GetSize()};
    for (const ScatterPlacement& lock : locks)
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
            const GenerationStatus status = ExplainAbsence(input, keys, locks, lock.Identity, conflict.Decision);
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
    return inoutCandidate.Conflicts.IsEmpty() == true ? GenerationStatus::Success : GenerationStatus::ConstraintFailed;
}

/// Append a conflict on one property of an authored lock and the candidate's placement with its identity.
[[nodiscard]] bool TryAddPropertyConflict(ScatterOverrideProperty property,
                                          const ScatterPlacement& authored,
                                          const ScatterPlacement& generated,
                                          Array<ScatterOverrideConflict>& inoutConflicts) noexcept
{
    ScatterOverrideConflict conflict;
    conflict.Identity = authored.Identity;
    conflict.Property = property;
    conflict.HasAuthored = true;
    conflict.Authored = authored;
    conflict.HasCandidate = true;
    conflict.Candidate = generated;
    return inoutConflicts.TryAdd(conflict);
}

/// Append one conflict per property on which the candidate's placement differs from the authored lock, in
/// ScatterOverrideProperty order.
[[nodiscard]] bool TryAddPropertyConflicts(const ScatterPlacement& authored,
                                           const ScatterPlacement& generated,
                                           Array<ScatterOverrideConflict>& inoutConflicts) noexcept
{
    const bool isPositionDifferent = authored.LatticeX != generated.LatticeX || authored.LatticeY != generated.LatticeY;
    return (isPositionDifferent == false ||
            TryAddPropertyConflict(ScatterOverrideProperty::Position, authored, generated, inoutConflicts) == true) &&
           (authored.Variant == generated.Variant ||
            TryAddPropertyConflict(ScatterOverrideProperty::Variant, authored, generated, inoutConflicts) == true) &&
           (authored.Rotation == generated.Rotation ||
            TryAddPropertyConflict(ScatterOverrideProperty::Rotation, authored, generated, inoutConflicts) == true) &&
           (authored.Scale == generated.Scale ||
            TryAddPropertyConflict(ScatterOverrideProperty::Scale, authored, generated, inoutConflicts) == true);
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
    const uint32 lockCount = reader.ReadUInt32();
    if (reader.HasFailed() == true || lockCount > MAX_SCATTER_LAYER_LOCKS || mLocks.TryResize(lockCount) == false)
    {
        return false;
    }
    for (ScatterPlacement& lock : mLocks)
    {
        lock = ReadPlacement(reader);
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
    mLayer.Locks = {mLocks.GetData(), mLocks.GetSize()};
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
    Array<ScatterPlacement> locks;
    const GenerationStatus inputStatus = ValidateInput(input, keys, locks);
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
            WriteLayerParameters(input,
                                 scope.Recipe,
                                 layer,
                                 GetLayerLocks({locks.GetData(), locks.GetSize()}, scope.Recipe.Room, layer.Id),
                                 writer);
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
    Array<ScatterPlacement> locks;
    candidate.Status = ValidateInput(input, keys, locks);
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
        GenerationByteWriter writer;
        WriteInput(input, {locks.GetData(), locks.GetSize()}, writer);
        candidate.Input = writer.ComputeDigest();
        candidate.Status = writer.HasFailed() == true ? GenerationStatus::OutOfMemory : GenerationStatus::Success;
        candidate.Locks = foundation::Move(locks);
    }
    if (candidate.Status == GenerationStatus::Success)
    {
        candidate.Status = TryCheckLocks(input, keys, candidate);
    }
    if (candidate.Status != GenerationStatus::Success && candidate.Status != GenerationStatus::ConstraintFailed)
    {
        candidate.Manifest = ScatterManifest{};
        candidate.Input = GenerationDigest{};
        candidate.Locks.Clear();
        candidate.Conflicts.Clear();
        mGeneration.Reject(candidate.Generation);
    }
    outCandidate = foundation::Move(candidate);
    return outCandidate.Status;
}

GenerationStatus ScatterGenerationCoordinator::TryPublish(const ScatterGenerationInput& current,
                                                          ScatterCandidate& inoutCandidate) noexcept
{
    if (inoutCandidate.Status != GenerationStatus::Success)
    {
        return GenerationStatus::NotReady;
    }
    GenerationDigest currentInput;
    const GenerationStatus inputStatus = TryComputeScatterInputDigest(current, currentInput);
    if (inputStatus != GenerationStatus::Success)
    {
        return inputStatus;
    }
    if (currentInput != inoutCandidate.Input)
    {
        return GenerationStatus::StaleCandidate;
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
    Array<ScatterPlacement> locks;
    const GenerationStatus inputStatus = ValidateInput(candidateInput, keys, locks);
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
            const GenerationStatus status = ExplainAbsence(candidateInput,
                                                           keys,
                                                           {locks.GetData(), locks.GetSize()},
                                                           placement.Identity,
                                                           change.Decision);
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

GenerationStatus TryComputeScatterInputDigest(const ScatterGenerationInput& input, GenerationDigest& outDigest) noexcept
{
    ScatterKeys keys;
    Array<ScatterPlacement> locks;
    const GenerationStatus status = ValidateInput(input, keys, locks);
    if (status != GenerationStatus::Success)
    {
        return status;
    }
    GenerationByteWriter writer;
    WriteInput(input, {locks.GetData(), locks.GetSize()}, writer);
    if (writer.HasFailed() == true)
    {
        return GenerationStatus::OutOfMemory;
    }
    outDigest = writer.ComputeDigest();
    return GenerationStatus::Success;
}

GenerationStatus TryFindScatterOverrideConflicts(const ScatterCandidate& candidate,
                                                 const ScatterGenerationInput& current,
                                                 Array<ScatterOverrideConflict>& outConflicts) noexcept
{
    if (candidate.Status != GenerationStatus::Success && candidate.Status != GenerationStatus::ConstraintFailed)
    {
        return GenerationStatus::NotReady;
    }
    ScatterKeys keys;
    Array<ScatterPlacement> authored;
    const GenerationStatus inputStatus = ValidateInput(current, keys, authored);
    if (inputStatus != GenerationStatus::Success)
    {
        return inputStatus;
    }
    // Walk the authored and staged locks together; both are in LockOrder.
    const LockOrder isBefore;
    Array<ScatterOverrideConflict> conflicts;
    usize authoredIndex = 0;
    usize stagedIndex = 0;
    while (authoredIndex < authored.GetSize() || stagedIndex < candidate.Locks.GetSize())
    {
        const bool isAuthoredNext = stagedIndex == candidate.Locks.GetSize() ||
                                    (authoredIndex < authored.GetSize() &&
                                     isBefore(candidate.Locks[stagedIndex], authored[authoredIndex]) == false);
        const ScatterPlacement& lock = isAuthoredNext == true ? authored[authoredIndex] : candidate.Locks[stagedIndex];
        const bool isStaged =
            stagedIndex < candidate.Locks.GetSize() && candidate.Locks[stagedIndex].Identity == lock.Identity;
        const ScatterPlacement* generatedOrNull = candidate.Manifest.FindPlacement(lock.Identity);
        bool isAdded = true;
        if (isAuthoredNext == true && generatedOrNull != nullptr)
        {
            isAdded = TryAddPropertyConflicts(lock, *generatedOrNull, conflicts);
        }
        else
        {
            ScatterOverrideConflict conflict;
            conflict.Identity = lock.Identity;
            conflict.Property =
                isAuthoredNext == true ? ScatterOverrideProperty::Presence : ScatterOverrideProperty::Lock;
            conflict.HasAuthored = isAuthoredNext;
            conflict.Authored = isAuthoredNext == true ? lock : ScatterPlacement{};
            conflict.HasCandidate = generatedOrNull != nullptr;
            conflict.Candidate = generatedOrNull != nullptr ? *generatedOrNull : ScatterPlacement{};
            isAdded = conflicts.TryAdd(conflict);
        }
        if (isAdded == false)
        {
            return GenerationStatus::OutOfMemory;
        }
        authoredIndex += isAuthoredNext == true ? 1 : 0;
        stagedIndex += isStaged == true ? 1 : 0;
    }
    outConflicts = foundation::Move(conflicts);
    return GenerationStatus::Success;
}
} // namespace ludus::content::procedural
