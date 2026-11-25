#include <ludus/content/procedural/architecture/modular_assembly.h>

#include <ludus/foundation/containers/sort.hpp>

namespace ludus::content::procedural
{
namespace
{
using foundation::Array;
using foundation::BorrowedView;

/// Bytes of the record count that starts every parameter block and product.
constexpr usize COUNT_BYTES = 4;
/// Bytes of one module record: ID 2, width 2, role 1.
constexpr usize MODULE_RECORD_BYTES = 5;
/// Bytes of one instance record: span 2, slot 2, module 2, offset 4.
constexpr usize INSTANCE_RECORD_BYTES = 10;
/// Most instances of one assembly product: every span at its longest, filled with unit-width modules.
constexpr usize MAX_ASSEMBLY_INSTANCES = MAX_ASSEMBLY_SPANS * MAX_ASSEMBLY_SPAN_LENGTH;

void WriteModule(const AssemblyModule& module, GenerationByteWriter& outWriter) noexcept
{
    outWriter.WriteUInt16(module.Id);
    outWriter.WriteUInt16(module.Width);
    outWriter.WriteUInt8(static_cast<uint8>(module.Role));
}

/// Decode a count and module records; false on malformed bytes, an unknown role or allocation failure.
[[nodiscard]] bool TryReadModules(GenerationByteReader& inoutReader, Array<AssemblyModule>& outModules) noexcept
{
    const uint32 count = inoutReader.ReadUInt32();
    if (inoutReader.HasFailed() == true || count > MAX_ASSEMBLY_MODULES ||
        inoutReader.GetRemainingSize() < static_cast<usize>(count) * MODULE_RECORD_BYTES ||
        outModules.TryResize(count) == false)
    {
        return false;
    }
    for (AssemblyModule& module : outModules)
    {
        module.Id = inoutReader.ReadUInt16();
        module.Width = inoutReader.ReadUInt16();
        const uint8 role = inoutReader.ReadUInt8();
        if (role > static_cast<uint8>(AssemblyModuleRole::Adapter))
        {
            return false;
        }
        module.Role = static_cast<AssemblyModuleRole>(role);
    }
    return inoutReader.HasFailed() == false;
}

/// A library assembly can use: 1 through MAX_ASSEMBLY_MODULES modules with unique IDs, widths in range and at
/// least one Repeat module.
[[nodiscard]] bool IsValidLibrary(BorrowedView<const AssemblyModule> modules) noexcept
{
    if (modules.IsValid() == false || modules.IsEmpty() == true || modules.GetSize() > MAX_ASSEMBLY_MODULES)
    {
        return false;
    }
    bool hasRepeat = false;
    for (usize i = 0; i < modules.GetSize(); ++i)
    {
        const AssemblyModule& module = modules[i];
        if (module.Width == 0 || module.Width > MAX_ASSEMBLY_MODULE_WIDTH || module.Role > AssemblyModuleRole::Adapter)
        {
            return false;
        }
        for (usize earlier = 0; earlier < i; ++earlier)
        {
            if (modules[earlier].Id == module.Id)
            {
                return false;
            }
        }
        hasRepeat = hasRepeat == true || module.Role == AssemblyModuleRole::Repeat;
    }
    return hasRepeat;
}

[[nodiscard]] bool IsValidSpans(BorrowedView<const AssemblySpan> spans) noexcept
{
    if (spans.IsValid() == false || spans.IsEmpty() == true || spans.GetSize() > MAX_ASSEMBLY_SPANS)
    {
        return false;
    }
    for (usize i = 0; i < spans.GetSize(); ++i)
    {
        if (spans[i].Length == 0 || spans[i].Length > MAX_ASSEMBLY_SPAN_LENGTH ||
            (i > 0 && spans[i - 1].Id >= spans[i].Id))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const AssemblyModule* FindModule(BorrowedView<const AssemblyModule> modules, uint16 id) noexcept
{
    for (const AssemblyModule& module : modules)
    {
        if (module.Id == id)
        {
            return &module;
        }
    }
    return nullptr;
}
} // namespace

GenerationStatus ModuleCatalogKernel::TryExecute(const GenerationStage& stage,
                                                 GenerationInputProducts inputs,
                                                 GenerationByteWriter& outProduct) noexcept
{
    GenerationByteReader reader({stage.Parameters.GetData(), stage.Parameters.GetSize()});
    if (inputs.IsEmpty() == false || TryReadModules(reader, mModules) == false || reader.IsAtEnd() == false ||
        IsValidLibrary({mModules.GetData(), mModules.GetSize()}) == false)
    {
        return GenerationStatus::InvalidInput;
    }
    foundation::Sort(
        mModules.GetData(),
        mModules.GetSize(),
        [](const AssemblyModule& left, const AssemblyModule& right) noexcept { return left.Id < right.Id; });
    outProduct.WriteUInt32(static_cast<uint32>(mModules.GetSize()));
    for (const AssemblyModule& module : mModules)
    {
        WriteModule(module, outProduct);
    }
    return outProduct.HasFailed() == true ? GenerationStatus::OutOfMemory : GenerationStatus::Success;
}

bool ModuleCatalogKernel::IsValidProduct(const GenerationStage& /*stage*/,
                                         GenerationInputProducts inputs,
                                         BorrowedView<const uint8> product) noexcept
{
    return inputs.IsEmpty() == true && TryReadModuleCatalog(product, mModules) == true;
}

bool SpanAssemblyKernel::tryDecode(const GenerationStage& stage, GenerationInputProducts inputs) noexcept
{
    if (inputs.GetSize() != 1 || TryReadModuleCatalog(inputs[0], mModules) == false)
    {
        return false;
    }
    GenerationByteReader reader({stage.Parameters.GetData(), stage.Parameters.GetSize()});
    const uint32 count = reader.ReadUInt32();
    if (reader.HasFailed() == true || count > MAX_ASSEMBLY_SPANS || mSpans.TryResize(count) == false)
    {
        return false;
    }
    for (AssemblySpan& span : mSpans)
    {
        span.Id = reader.ReadUInt16();
        span.Length = reader.ReadUInt32();
    }
    return reader.IsAtEnd() == true && IsValidSpans({mSpans.GetData(), mSpans.GetSize()}) == true;
}

GenerationStatus SpanAssemblyKernel::tryCloseSpan(const AssemblySpan& span) noexcept
{
    mClosures.Clear();
    if (mClosures.TryResize(static_cast<usize>(span.Length) + 1, Closure{}) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    mClosures[0].IsReachable = true;
    for (usize length = 1; length <= span.Length; ++length)
    {
        Closure best;
        for (const AssemblyModule& module : mModules)
        {
            if (module.Width > length || mClosures[length - module.Width].IsReachable == false)
            {
                continue;
            }
            const Closure& rest = mClosures[length - module.Width];
            Closure closure;
            closure.IsReachable = true;
            closure.Adapters =
                static_cast<uint16>(rest.Adapters + (module.Role == AssemblyModuleRole::Adapter ? 1 : 0));
            closure.Modules = static_cast<uint16>(rest.Modules + 1);
            if (best.IsReachable == false || closure.Adapters < best.Adapters ||
                (closure.Adapters == best.Adapters && closure.Modules < best.Modules))
            {
                best = closure;
            }
        }
        mClosures[length] = best;
    }
    if (mClosures[span.Length].IsReachable == false)
    {
        return GenerationStatus::ConstraintFailed;
    }
    // Lay out from the start: the lowest-ID module whose remainder still closes optimally.
    usize remaining = span.Length;
    AssemblyInstance instance;
    instance.Identity.Span = span.Id;
    while (remaining > 0)
    {
        const Closure& target = mClosures[remaining];
        const AssemblyModule* chosenOrNull = nullptr;
        for (const AssemblyModule& module : mModules)
        {
            if (module.Width > remaining)
            {
                continue;
            }
            const Closure& rest = mClosures[remaining - module.Width];
            const uint16 adapters = module.Role == AssemblyModuleRole::Adapter ? 1 : 0;
            if (rest.IsReachable == true && rest.Adapters + adapters == target.Adapters &&
                rest.Modules + 1 == target.Modules)
            {
                chosenOrNull = &module;
                break;
            }
        }
        // An optimal closure of `remaining` always has a first module; its absence would be a closure table defect.
        LUDUS_ASSERT(chosenOrNull != nullptr);
        if (chosenOrNull == nullptr || mInstances.TryAdd(instance) == false)
        {
            return chosenOrNull == nullptr ? GenerationStatus::ConstraintFailed : GenerationStatus::OutOfMemory;
        }
        mInstances.GetLast().Module = chosenOrNull->Id;
        remaining -= chosenOrNull->Width;
        instance.Offset += chosenOrNull->Width;
        ++instance.Identity.Slot;
    }
    return GenerationStatus::Success;
}

GenerationStatus SpanAssemblyKernel::TryExecute(const GenerationStage& stage,
                                                GenerationInputProducts inputs,
                                                GenerationByteWriter& outProduct) noexcept
{
    if (tryDecode(stage, inputs) == false)
    {
        return GenerationStatus::InvalidInput;
    }
    mInstances.Clear();
    for (const AssemblySpan& span : mSpans)
    {
        const GenerationStatus status = tryCloseSpan(span);
        if (status != GenerationStatus::Success)
        {
            return status;
        }
    }
    outProduct.WriteUInt32(static_cast<uint32>(mInstances.GetSize()));
    for (const AssemblyInstance& instance : mInstances)
    {
        outProduct.WriteUInt16(instance.Identity.Span);
        outProduct.WriteUInt16(instance.Identity.Slot);
        outProduct.WriteUInt16(instance.Module);
        outProduct.WriteUInt32(instance.Offset);
    }
    return outProduct.HasFailed() == true ? GenerationStatus::OutOfMemory : GenerationStatus::Success;
}

bool SpanAssemblyKernel::IsValidProduct(const GenerationStage& stage,
                                        GenerationInputProducts inputs,
                                        BorrowedView<const uint8> product) noexcept
{
    if (tryDecode(stage, inputs) == false || TryReadSpanAssembly(product, stage.Scope, mInstances) == false)
    {
        return false;
    }
    usize next = 0;
    for (const AssemblySpan& span : mSpans)
    {
        uint32 offset = 0;
        for (uint16 slot = 0; offset < span.Length; ++slot)
        {
            if (next >= mInstances.GetSize())
            {
                return false;
            }
            const AssemblyInstance& instance = mInstances[next];
            const AssemblyModule* moduleOrNull = FindModule({mModules.GetData(), mModules.GetSize()}, instance.Module);
            if (instance.Identity.Span != span.Id || instance.Identity.Slot != slot || instance.Offset != offset ||
                moduleOrNull == nullptr)
            {
                return false;
            }
            offset += moduleOrNull->Width;
            ++next;
        }
        if (offset != span.Length)
        {
            return false;
        }
    }
    return next == mInstances.GetSize();
}

GenerationStatus TryAddModuleCatalogStage(const ModuleCatalogInput& input,
                                          ModuleCatalogKernel& inoutKernel,
                                          Array<GenerationStage>& inoutPlan,
                                          usize& outIndex) noexcept
{
    if (IsValidLibrary(input.Modules) == false)
    {
        return GenerationStatus::InvalidInput;
    }
    GenerationByteWriter writer;
    writer.WriteUInt32(static_cast<uint32>(input.Modules.GetSize()));
    for (const AssemblyModule& module : input.Modules)
    {
        WriteModule(module, writer);
    }
    GenerationStage stage;
    stage.Scope = input.Scope;
    stage.Stage = input.Stage;
    stage.Operator = MODULE_CATALOG_OPERATOR;
    stage.OperatorVersion = MODULAR_ASSEMBLY_VERSION;
    stage.Schema = MODULE_CATALOG_PRODUCT_SCHEMA;
    stage.KernelOrNull = &inoutKernel;
    const usize index = inoutPlan.GetSize();
    if (writer.HasFailed() == true || stage.Parameters.TryAddRange(writer.GetBytes()) == false ||
        inoutPlan.TryAdd(foundation::Move(stage)) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    outIndex = index;
    return GenerationStatus::Success;
}

GenerationStatus TryAddSpanAssemblyStage(const SpanAssemblyInput& input,
                                         usize catalogIndex,
                                         SpanAssemblyKernel& inoutKernel,
                                         Array<GenerationStage>& inoutPlan) noexcept
{
    if (IsValidSpans(input.Spans) == false || catalogIndex >= inoutPlan.GetSize() ||
        inoutPlan[catalogIndex].Operator != MODULE_CATALOG_OPERATOR)
    {
        return GenerationStatus::InvalidInput;
    }
    GenerationByteWriter writer;
    writer.WriteUInt32(static_cast<uint32>(input.Spans.GetSize()));
    for (const AssemblySpan& span : input.Spans)
    {
        writer.WriteUInt16(span.Id);
        writer.WriteUInt32(span.Length);
    }
    GenerationStage stage;
    stage.Scope = input.Scope;
    stage.Stage = input.Stage;
    stage.Operator = SPAN_ASSEMBLY_OPERATOR;
    stage.OperatorVersion = MODULAR_ASSEMBLY_VERSION;
    stage.Schema = SPAN_ASSEMBLY_PRODUCT_SCHEMA;
    stage.KernelOrNull = &inoutKernel;
    if (writer.HasFailed() == true || stage.Parameters.TryAddRange(writer.GetBytes()) == false ||
        stage.Inputs.TryAdd(catalogIndex) == false || inoutPlan.TryAdd(foundation::Move(stage)) == false)
    {
        return GenerationStatus::OutOfMemory;
    }
    return GenerationStatus::Success;
}

bool TryReadModuleCatalog(BorrowedView<const uint8> product, Array<AssemblyModule>& outModules) noexcept
{
    Array<AssemblyModule> modules;
    GenerationByteReader reader(product);
    if (TryReadModules(reader, modules) == false || reader.IsAtEnd() == false ||
        IsValidLibrary({modules.GetData(), modules.GetSize()}) == false)
    {
        return false;
    }
    for (usize i = 1; i < modules.GetSize(); ++i)
    {
        if (modules[i - 1].Id >= modules[i].Id)
        {
            return false;
        }
    }
    outModules = foundation::Move(modules);
    return true;
}

bool TryReadSpanAssembly(BorrowedView<const uint8> product,
                         uint32 scope,
                         Array<AssemblyInstance>& outInstances) noexcept
{
    GenerationByteReader reader(product);
    const uint32 count = reader.ReadUInt32();
    Array<AssemblyInstance> instances;
    if (reader.HasFailed() == true || count > MAX_ASSEMBLY_INSTANCES ||
        product.GetSize() != COUNT_BYTES + (static_cast<usize>(count) * INSTANCE_RECORD_BYTES) ||
        instances.TryResize(count) == false)
    {
        return false;
    }
    for (usize i = 0; i < instances.GetSize(); ++i)
    {
        AssemblyInstance& instance = instances[i];
        instance.Identity.Scope = scope;
        instance.Identity.Span = reader.ReadUInt16();
        instance.Identity.Slot = reader.ReadUInt16();
        instance.Module = reader.ReadUInt16();
        instance.Offset = reader.ReadUInt32();
        const bool isFirstOfSpan =
            instance.Identity.Slot == 0 && (i == 0 || instances[i - 1].Identity.Span < instance.Identity.Span);
        const bool isNextOfSpan = i > 0 && instances[i - 1].Identity.Span == instance.Identity.Span &&
                                  instances[i - 1].Identity.Slot + 1 == instance.Identity.Slot;
        if (isFirstOfSpan == false && isNextOfSpan == false)
        {
            return false;
        }
    }
    if (reader.IsAtEnd() == false)
    {
        return false;
    }
    outInstances = foundation::Move(instances);
    return true;
}
} // namespace ludus::content::procedural
