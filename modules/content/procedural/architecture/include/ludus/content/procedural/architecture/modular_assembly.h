#pragma once

// Modular assembly stages: the second consumer of the procedural generation
// executor of slice P1. A module catalog stage turns an authored module library
// into a canonical catalog product; a span assembly stage reads that product and
// closes each of a scope's spans exactly with repeat modules and authored adapters,
// giving every module instance a persistent (scope, span, slot) identity. Both are
// pure GenerationStageKernel functions of their parameters and input products.
// Contracts: docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation
// and #architecture-buildings-and-interiors.

// Thanks to Pascal Müller, Peter Wonka, Simon Haegler, Andreas Ulmer and Luc Van
// Gool, "Procedural Modeling of Buildings", ACM Transactions on Graphics 25(3),
// SIGGRAPH 2006, pp. 614-623, https://doi.org/10.1145/1179352.1141931. The repeat
// split of their shape grammar, which fills a scope with as many copies of a shape
// as fit, informs the span closure here. Ludus
// departs by requiring an exact closure in integer lattice units: repeats and
// authored adapters close the span with the fewest adapters, or the stage fails.

#include <ludus/foundation/base/core.h>

#include <ludus/content/procedural/core/generation_bytes.h>
#include <ludus/content/procedural/core/generation_executor.h>
#include <ludus/content/procedural/core/generation_trace.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::procedural
{
/// Persistent operator ID of the module catalog stage. Scatter layers use operator 1.
inline constexpr uint32 MODULE_CATALOG_OPERATOR = 2;
/// Persistent operator ID of the span assembly stage.
inline constexpr uint32 SPAN_ASSEMBLY_OPERATOR = 3;
/// Version of both assembly operators' algorithms and parameter encodings.
inline constexpr uint32 MODULAR_ASSEMBLY_VERSION = 1;
/// Schema of a module catalog product: a count, then 5-byte module records in ascending ID order.
inline constexpr uint32 MODULE_CATALOG_PRODUCT_SCHEMA = 1;
/// Schema of a span assembly product: a count, then 10-byte instance records in span then slot order.
inline constexpr uint32 SPAN_ASSEMBLY_PRODUCT_SCHEMA = 1;
/// Most modules in a catalog.
inline constexpr usize MAX_ASSEMBLY_MODULES = 64;
/// Widest module, in lattice units.
inline constexpr uint16 MAX_ASSEMBLY_MODULE_WIDTH = 1024;
/// Most spans in one assembly stage.
inline constexpr usize MAX_ASSEMBLY_SPANS = 64;
/// Longest span, in lattice units; bounds the closure search and the instances of one span.
inline constexpr uint32 MAX_ASSEMBLY_SPAN_LENGTH = 4096;

/// @brief How a span assembly may use a module.
enum class AssemblyModuleRole : uint8
{
    /// An ordinary module repeated along a span.
    Repeat,
    /// An authored filler that closes a residual span; assembly uses as few as possible.
    Adapter,
};

/// @brief One authored module of a library.
struct AssemblyModule final
{
    /// Persistent module ID, unique within the catalog.
    uint16 Id = 0;
    /// Width along a span, 1 through MAX_ASSEMBLY_MODULE_WIDTH lattice units.
    uint16 Width = 1;
    /// How assembly may use the module.
    AssemblyModuleRole Role = AssemblyModuleRole::Repeat;
};

/// @brief One straight span of a scope that assembly closes exactly with modules.
struct AssemblySpan final
{
    /// Persistent span ID, unique within the stage.
    uint16 Id = 0;
    /// Length, 1 through MAX_ASSEMBLY_SPAN_LENGTH lattice units.
    uint32 Length = 1;
};

/// @brief Persistent identity of a module instance.
struct AssemblyIdentity final
{
    /// The assembly stage's scope.
    uint32 Scope = 0;
    /// The span's authored ID.
    uint16 Span = 0;
    /// Position along the span, counted from zero at the span's start.
    uint16 Slot = 0;
    /// Field-wise equality.
    [[nodiscard]] bool operator==(const AssemblyIdentity&) const noexcept = default;
};

/// @brief One module placed along a span.
struct AssemblyInstance final
{
    /// Persistent identity.
    AssemblyIdentity Identity;
    /// The placed module's ID.
    uint16 Module = 0;
    /// Distance from the span's start to the module's start, in lattice units.
    uint32 Offset = 0;
};

/// @brief The shared upstream stage of one or more assemblies: an authored module library.
struct ModuleCatalogInput final
{
    /// Scope of the stage; often a scope shared by several assemblies.
    uint32 Scope = 0;
    /// Persistent stage ID within the scope.
    uint32 Stage = 0;
    /// The authored modules, 1 through MAX_ASSEMBLY_MODULES in any order, with unique IDs and at least one Repeat.
    /// Reordering them changes the stage's key but not its product. Borrowed for the call.
    foundation::BorrowedView<const AssemblyModule> Modules;
};

/// @brief One scope's span assembly: its local spans, closed with a catalog stage's modules.
struct SpanAssemblyInput final
{
    /// Scope of the stage.
    uint32 Scope = 0;
    /// Persistent stage ID within the scope.
    uint32 Stage = 0;
    /// The spans, 1 through MAX_ASSEMBLY_SPANS in strictly ascending ID order. Borrowed for the call.
    foundation::BorrowedView<const AssemblySpan> Spans;
};

/// @brief The kernel of module catalog stages: validates the authored modules and sorts them by ID.
///
/// Holds reusable scratch storage. Single-owner; not thread-safe.
class ModuleCatalogKernel final : public GenerationStageKernel
{
public:
    /// @copydoc GenerationStageKernel::TryExecute
    /// @return Success, InvalidInput for an invalid module library, or OutOfMemory.
    [[nodiscard]] GenerationStatus TryExecute(const GenerationStage& stage,
                                              GenerationInputProducts inputs,
                                              GenerationByteWriter& outProduct) noexcept override;
    /// @brief Check a cached catalog: well-formed, valid modules in strictly ascending ID order.
    [[nodiscard]] bool IsValidProduct(const GenerationStage& stage,
                                      GenerationInputProducts inputs,
                                      foundation::BorrowedView<const uint8> product) noexcept override;

private:
    foundation::Array<AssemblyModule> mModules;
};

/// @brief The kernel of span assembly stages.
///
/// Each span is closed exactly with the fewest adapters, then the fewest modules. Modules are laid out from the
/// span's start; at each slot the lowest-ID module that still allows an optimal closure of the remainder is chosen,
/// so the layout is a deterministic function of the catalog and the span. Holds reusable scratch storage.
/// Single-owner; not thread-safe.
class SpanAssemblyKernel final : public GenerationStageKernel
{
public:
    /// @copydoc GenerationStageKernel::TryExecute
    /// @return Success; InvalidInput for malformed parameters or catalog; ConstraintFailed when a span has no exact
    ///         closure with the catalog's modules; or OutOfMemory.
    [[nodiscard]] GenerationStatus TryExecute(const GenerationStage& stage,
                                              GenerationInputProducts inputs,
                                              GenerationByteWriter& outProduct) noexcept override;
    /// @brief Check a cached assembly: every span in order, slots counted from zero, catalog modules only, and
    ///        offsets contiguous so each span closes exactly at its length.
    [[nodiscard]] bool IsValidProduct(const GenerationStage& stage,
                                      GenerationInputProducts inputs,
                                      foundation::BorrowedView<const uint8> product) noexcept override;

private:
    /// Closure cost of a remaining length: adapters first, then modules.
    struct Closure final
    {
        /// Whether any module sequence closes the length exactly.
        bool IsReachable = false;
        /// Fewest adapters of an exact closure.
        uint16 Adapters = 0;
        /// Fewest modules of an exact closure with that many adapters.
        uint16 Modules = 0;
    };

    [[nodiscard]] bool tryDecode(const GenerationStage& stage, GenerationInputProducts inputs) noexcept;
    [[nodiscard]] GenerationStatus tryCloseSpan(const AssemblySpan& span) noexcept;

    foundation::Array<AssemblyModule> mModules;
    foundation::Array<AssemblySpan> mSpans;
    foundation::Array<Closure> mClosures;
    foundation::Array<AssemblyInstance> mInstances;
};

/// @brief Append a module catalog stage to a plan.
/// @param input The authored library; validated by this call.
/// @param inoutKernel The stage's kernel; borrowed and must outlive every request using the plan.
/// @param inoutPlan Receives the stage; unchanged on failure.
/// @param outIndex The stage's plan index, for TryAddSpanAssemblyStage; unchanged on failure.
/// @return Success, InvalidInput or OutOfMemory.
[[nodiscard]] GenerationStatus TryAddModuleCatalogStage(const ModuleCatalogInput& input,
                                                        ModuleCatalogKernel& inoutKernel,
                                                        foundation::Array<GenerationStage>& inoutPlan,
                                                        usize& outIndex) noexcept;

/// @brief Append a span assembly stage that reads a catalog stage's product to a plan.
/// @param input The scope's spans; validated by this call.
/// @param catalogIndex Plan index of a module catalog stage already in the plan.
/// @param inoutKernel The stage's kernel; borrowed and must outlive every request using the plan.
/// @param inoutPlan Receives the stage; unchanged on failure.
/// @return Success, InvalidInput or OutOfMemory.
[[nodiscard]] GenerationStatus TryAddSpanAssemblyStage(const SpanAssemblyInput& input,
                                                       usize catalogIndex,
                                                       SpanAssemblyKernel& inoutKernel,
                                                       foundation::Array<GenerationStage>& inoutPlan) noexcept;

/// @brief Decode a module catalog product.
/// @param product The product's canonical bytes.
/// @param outModules Replaced on success with modules in ascending ID order; unchanged on failure.
/// @return False when the bytes are not a valid catalog or allocation fails.
[[nodiscard]] bool TryReadModuleCatalog(foundation::BorrowedView<const uint8> product,
                                        foundation::Array<AssemblyModule>& outModules) noexcept;

/// @brief Decode a span assembly product.
/// @param product The product's canonical bytes.
/// @param scope The assembly stage's scope, which every identity carries.
/// @param outInstances Replaced on success in span then slot order; unchanged on failure.
/// @return False when the bytes are malformed or allocation fails.
[[nodiscard]] bool TryReadSpanAssembly(foundation::BorrowedView<const uint8> product,
                                       uint32 scope,
                                       foundation::Array<AssemblyInstance>& outInstances) noexcept;
} // namespace ludus::content::procedural
