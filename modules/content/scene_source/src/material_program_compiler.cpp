#include "internal/material_program_compiler.h"

#include <initializer_list>

namespace ludus::content::scene_source::internal
{
namespace
{
namespace format = scene_format;
using foundation::StaticArray;

/// Last-use marker of a value that a lobe reads, so it stays live to the end.
constexpr usize LOBE_USE = MAX_PROGRAM_VALUES;
/// Last-use marker of a value nothing live reads.
constexpr usize NO_USE = MAX_PROGRAM_VALUES + 1;

[[nodiscard]] bool IsArithmetic(format::MaterialProgramOpcode opcode) noexcept
{
    return opcode != format::MaterialProgramOpcode::Constant && opcode != format::MaterialProgramOpcode::SurfaceInput;
}

/// Find or add a constant slot, comparing every component exactly.
[[nodiscard]] bool TryGetConstantSlot(const StaticArray<float32, format::MATERIAL_PROGRAM_COMPONENTS>& value,
                                      format::MaterialProgram& inoutProgram,
                                      uint8& outSlot) noexcept
{
    for (uint32 slot = 0; slot < inoutProgram.ConstantCount; ++slot)
    {
        bool isEqual = true;
        for (usize component = 0; component < format::MATERIAL_PROGRAM_COMPONENTS; ++component)
        {
            isEqual = isEqual == true && inoutProgram.Constants[slot][component] == value[component];
        }
        if (isEqual == true)
        {
            outSlot = static_cast<uint8>(slot);
            return true;
        }
    }
    if (inoutProgram.ConstantCount == format::MAX_MATERIAL_PROGRAM_CONSTANTS)
    {
        return false;
    }
    inoutProgram.Constants[inoutProgram.ConstantCount] = value;
    outSlot = static_cast<uint8>(inoutProgram.ConstantCount);
    ++inoutProgram.ConstantCount;
    return true;
}
} // namespace

ProgramCompileStatus CompileMaterialProgram(const ProgramDraft& draft, format::MaterialProgram& outProgram) noexcept
{
    LUDUS_ASSERT(draft.ValueCount <= MAX_PROGRAM_VALUES && draft.LobeCount <= format::MAX_MATERIAL_PROGRAM_LOBES);
    // Liveness: a value is live when a lobe or a live value reads it.
    StaticArray<bool, MAX_PROGRAM_VALUES> isLive{};
    for (usize lobe = 0; lobe < draft.LobeCount; ++lobe)
    {
        for (const uint8 value : {draft.Lobes[lobe].WeightValue, draft.Lobes[lobe].RoughnessValue})
        {
            if (value != NO_PROGRAM_VALUE)
            {
                isLive[value] = true;
            }
        }
    }
    for (usize value = draft.ValueCount; value != 0; --value)
    {
        const ProgramValue& item = draft.Values[value - 1];
        if (isLive[value - 1] == true && IsArithmetic(item.Opcode) == true)
        {
            for (const uint8 operand : item.Operands)
            {
                if (operand != NO_PROGRAM_VALUE)
                {
                    isLive[operand] = true;
                }
            }
        }
    }
    // The last live reader of each value decides when its register becomes free.
    StaticArray<usize, MAX_PROGRAM_VALUES> lastUse{};
    for (usize& use : lastUse)
    {
        use = NO_USE;
    }
    for (usize value = 0; value < draft.ValueCount; ++value)
    {
        if (isLive[value] == true && IsArithmetic(draft.Values[value].Opcode) == true)
        {
            for (const uint8 operand : draft.Values[value].Operands)
            {
                if (operand != NO_PROGRAM_VALUE)
                {
                    lastUse[operand] = value;
                }
            }
        }
    }
    for (usize lobe = 0; lobe < draft.LobeCount; ++lobe)
    {
        for (const uint8 value : {draft.Lobes[lobe].WeightValue, draft.Lobes[lobe].RoughnessValue})
        {
            if (value != NO_PROGRAM_VALUE)
            {
                lastUse[value] = LOBE_USE;
            }
        }
    }
    format::MaterialProgram program;
    StaticArray<uint8, MAX_PROGRAM_VALUES> registers{};
    StaticArray<bool, format::MAX_MATERIAL_PROGRAM_REGISTERS> isOccupied{};
    for (usize value = 0; value < draft.ValueCount; ++value)
    {
        if (isLive[value] == false)
        {
            continue;
        }
        const ProgramValue& item = draft.Values[value];
        format::MaterialProgramInstruction instruction;
        instruction.Opcode = item.Opcode;
        if (item.Opcode == format::MaterialProgramOpcode::Constant)
        {
            if (TryGetConstantSlot(item.Constant, program, instruction.OperandA) == false)
            {
                return ProgramCompileStatus::TooManyConstants;
            }
        }
        else if (item.Opcode == format::MaterialProgramOpcode::SurfaceInput)
        {
            instruction.OperandA = static_cast<uint8>(item.Input);
        }
        else
        {
            StaticArray<uint8*, MAX_PROGRAM_OPERANDS> slots{
                {&instruction.OperandA, &instruction.OperandB, &instruction.OperandC}};
            for (usize operand = 0; operand < MAX_PROGRAM_OPERANDS; ++operand)
            {
                const uint8 source = item.Operands[operand];
                if (source != NO_PROGRAM_VALUE)
                {
                    *slots[operand] = registers[source];
                }
            }
            // Operands are read before the destination is written, so a register whose last
            // reader is this instruction can hold its result.
            for (const uint8 source : item.Operands)
            {
                if (source != NO_PROGRAM_VALUE && lastUse[source] == value)
                {
                    isOccupied[registers[source]] = false;
                }
            }
        }
        usize destination = 0;
        while (destination < format::MAX_MATERIAL_PROGRAM_REGISTERS && isOccupied[destination] == true)
        {
            ++destination;
        }
        if (destination == format::MAX_MATERIAL_PROGRAM_REGISTERS)
        {
            return ProgramCompileStatus::TooManyRegisters;
        }
        isOccupied[destination] = true;
        registers[value] = static_cast<uint8>(destination);
        instruction.Destination = static_cast<uint8>(destination);
        program.Instructions[program.InstructionCount] = instruction;
        ++program.InstructionCount;
    }
    for (usize lobe = 0; lobe < draft.LobeCount; ++lobe)
    {
        const ProgramDraftLobe& source = draft.Lobes[lobe];
        format::MaterialProgramLobe& compiled = program.Lobes[lobe];
        compiled.Operation = source.Operation;
        compiled.WeightRegister = source.WeightValue == NO_PROGRAM_VALUE ? format::NO_MATERIAL_PROGRAM_REGISTER
                                                                         : registers[source.WeightValue];
        compiled.RoughnessRegister = source.RoughnessValue == NO_PROGRAM_VALUE ? format::NO_MATERIAL_PROGRAM_REGISTER
                                                                               : registers[source.RoughnessValue];
        compiled.Eta = source.Eta;
        compiled.K = source.K;
        compiled.Power = source.Power;
    }
    program.LobeCount = static_cast<uint32>(draft.LobeCount);
    if (format::IsMaterialProgramValid(program) == false)
    {
        return ProgramCompileStatus::Invalid;
    }
    outProgram = program;
    return ProgramCompileStatus::Success;
}
} // namespace ludus::content::scene_source::internal
