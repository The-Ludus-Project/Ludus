#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::scene_source::internal
{
/// Values an authored program may define, inline constants included. Each live value becomes one
/// instruction, so the instruction limit bounds them.
inline constexpr usize MAX_PROGRAM_VALUES = scene_format::MAX_MATERIAL_PROGRAM_INSTRUCTIONS;
/// Most operands of one value (Lerp).
inline constexpr usize MAX_PROGRAM_OPERANDS = 3;
/// Value index of an absent operand.
inline constexpr uint8 NO_PROGRAM_VALUE = 0xFF;

/// One authored value in definition order. Operands name earlier values, so the draft is acyclic.
struct ProgramValue final
{
    /// Constant, SurfaceInput or an arithmetic opcode.
    scene_format::MaterialProgramOpcode Opcode = scene_format::MaterialProgramOpcode::None;
    /// Value of a Constant; the fourth component is zero.
    foundation::StaticArray<float32, scene_format::MATERIAL_PROGRAM_COMPONENTS> Constant{};
    /// Input of a SurfaceInput.
    scene_format::MaterialProgramInput Input = scene_format::MaterialProgramInput::BaseColor;
    /// Earlier value indices read by an arithmetic opcode; unused operands are NO_PROGRAM_VALUE.
    foundation::StaticArray<uint8, MAX_PROGRAM_OPERANDS> Operands{
        {NO_PROGRAM_VALUE, NO_PROGRAM_VALUE, NO_PROGRAM_VALUE}};
};

/// One authored lobe that names its weight and roughness values.
struct ProgramDraftLobe final
{
    scene_format::MaterialLobeOperation Operation = scene_format::MaterialLobeOperation::None; ///< Leaf operation.
    uint8 WeightValue = NO_PROGRAM_VALUE;    ///< Value whose RGB weights the lobe.
    uint8 RoughnessValue = NO_PROGRAM_VALUE; ///< Roughness value, or NO_PROGRAM_VALUE for Diffuse.
    foundation::StaticArray<float32, scene_format::MATERIAL_RGB_CHANNELS> Eta{}; ///< Conductor real IOR.
    foundation::StaticArray<float32, scene_format::MATERIAL_RGB_CHANNELS> K{};   ///< Conductor extinction.
    float32 Power = 0;                                                           ///< CosinePower exponent.
};

/// A parsed program before register allocation.
struct ProgramDraft final
{
    foundation::StaticArray<ProgramValue, MAX_PROGRAM_VALUES> Values{}; ///< Values in definition order.
    usize ValueCount = 0;                                               ///< Used Values.
    foundation::StaticArray<ProgramDraftLobe, scene_format::MAX_MATERIAL_PROGRAM_LOBES> Lobes{}; ///< Lobes.
    usize LobeCount = 0;                                                                         ///< Used Lobes.
};

/// Outcome of CompileMaterialProgram.
enum class ProgramCompileStatus : uint8
{
    Success,          ///< outProgram holds a valid program.
    TooManyConstants, ///< More than MAX_MATERIAL_PROGRAM_CONSTANTS distinct live constants.
    TooManyRegisters, ///< More than MAX_MATERIAL_PROGRAM_REGISTERS values are live at once.
    Invalid,          ///< The compiled program failed IsMaterialProgramValid, for example an empty lobe list.
};

/// Compile a draft into a canonical program. Values no lobe depends on are dropped; equal constants
/// share one slot; registers are reused once their last reader has executed.
/// @param draft Values whose operands precede them, and at least one lobe.
/// @param outProgram Replaced on Success; unchanged otherwise.
[[nodiscard]] ProgramCompileStatus CompileMaterialProgram(const ProgramDraft& draft,
                                                          scene_format::MaterialProgram& outProgram) noexcept;
} // namespace ludus::content::scene_source::internal
