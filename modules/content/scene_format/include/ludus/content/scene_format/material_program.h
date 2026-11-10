#pragma once

#ifdef __cplusplus
#    include <ludus/foundation/base/types.h>

#    include <ludus/content/scene_format/material.h>
#    include <ludus/foundation/containers/static_array.hpp>

#    include <cstddef>
#    include <type_traits>
#else
#    include "material.h"
#endif

/// @file
/// Bounded material closure programs (Ludus::SceneFormat), material model tag
/// MaterialModel::Program. The tags and limits are shared with the Slang interpreter.
///
/// A program is a straight-line list of parameter instructions over a small
/// register file followed by a weighted mixture of leaf lobes. The surface value
/// is the weighted sum of the lobe BSDFs, f = sum(w_i f_i). A sampler selects one
/// lobe with probability proportional to its scalar selection weight and samples
/// that lobe's direction, so a proposal consumes exactly three uniform values;
/// evaluation and continuation use the full mixture density
/// p = sum(s_i p_i) / sum(s_i), where s_i is the mean of the RGB weight w_i.
/// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, fourth
/// edition, section 2.2.3 (https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency),
/// for this one-sample mixture-density construction, which generalizes the
/// built-in diffuse/conductor Mix. The register program, its limits and the
/// lobe vocabulary are Ludus choices; see
/// the "Material closure programs" section of docs/architecture/renderer-systems.md.
///
/// Evaluators interpret registers as RGB: the fourth component of a constant is
/// zero and is never read. Each instruction clamps its result to
/// [-LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT, LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT], so
/// every value stays finite; lobes clamp negative weights to zero and roughness to
/// [MIN_ROUGHNESS, 1].
///
/// The same struct documents the LUSCN version-11 wire layout. A version-11
/// bundle appends one MaterialProgram record after the MaterialExtensionV7 of
/// every material whose model is MaterialModel::Program. Serialize it field by
/// field in little-endian order; unused constants, instructions and lobes are
/// all zero, so equal programs have equal bytes.
#ifdef __cplusplus
namespace ludus::content::scene_format
{
/// Integer representation of program tags and limits, also consumed by Slang.
using MaterialProgramTag = foundation::uint32;
#else
typedef uint MaterialProgramTag;
#endif
/// Opcode tag of MaterialProgramOpcode::Constant.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_CONSTANT = 1;
/// Opcode tag of MaterialProgramOpcode::SurfaceInput.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_SURFACE_INPUT = 2;
/// Opcode tag of MaterialProgramOpcode::Add.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_ADD = 3;
/// Opcode tag of MaterialProgramOpcode::Multiply.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_MULTIPLY = 4;
/// Opcode tag of MaterialProgramOpcode::Lerp.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_LERP = 5;
/// Opcode tag of MaterialProgramOpcode::OneMinus.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_ONE_MINUS = 6;
/// Opcode tag of MaterialProgramOpcode::Clamp.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_OPCODE_CLAMP = 7;
/// Input tag of MaterialProgramInput::BaseColor.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_INPUT_BASE_COLOR = 0;
/// Input tag of MaterialProgramInput::Roughness.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_INPUT_ROUGHNESS = 1;
/// Input tag of MaterialProgramInput::Metallic.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_INPUT_METALLIC = 2;
/// Operation tag of MaterialLobeOperation::Diffuse.
static const MaterialProgramTag LUDUS_MATERIAL_LOBE_DIFFUSE = 1;
/// Operation tag of MaterialLobeOperation::Conductor.
static const MaterialProgramTag LUDUS_MATERIAL_LOBE_CONDUCTOR = 2;
/// Operation tag of MaterialLobeOperation::CosinePower.
static const MaterialProgramTag LUDUS_MATERIAL_LOBE_COSINE_POWER = 3;
/// Registers an instruction may write. Bounds the evaluator's per-thread state.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS = 8;
/// Constants an instruction may load.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_MAX_CONSTANTS = 8;
/// Straight-line instructions evaluated per surface point.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_MAX_INSTRUCTIONS = 32;
/// Leaf lobes in the flattened weighted mixture.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_MAX_LOBES = 8;
/// Register operand of a lobe that reads no roughness register.
static const MaterialProgramTag LUDUS_MATERIAL_PROGRAM_NO_REGISTER = 255;
/// Largest constant and register magnitude; keeps every value of a bounded program finite in binary32.
#ifdef __cplusplus
inline constexpr MaterialScalar LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT = 10000;
#else
static const MaterialScalar LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT = 10000;
#endif

#ifdef __cplusplus
/// First and only supported program version.
inline constexpr foundation::uint32 MATERIAL_PROGRAM_VERSION_1 = 1;
/// Float4 registers an instruction may write.
inline constexpr foundation::usize MAX_MATERIAL_PROGRAM_REGISTERS = LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS;
/// Float4 constants an instruction may load.
inline constexpr foundation::usize MAX_MATERIAL_PROGRAM_CONSTANTS = LUDUS_MATERIAL_PROGRAM_MAX_CONSTANTS;
/// Straight-line instructions evaluated per surface point.
inline constexpr foundation::usize MAX_MATERIAL_PROGRAM_INSTRUCTIONS = LUDUS_MATERIAL_PROGRAM_MAX_INSTRUCTIONS;
/// Leaf lobes in the flattened weighted mixture.
inline constexpr foundation::usize MAX_MATERIAL_PROGRAM_LOBES = LUDUS_MATERIAL_PROGRAM_MAX_LOBES;
/// Components of one register or constant: RGB and a fourth component that loads zero and is never read.
inline constexpr foundation::usize MATERIAL_PROGRAM_COMPONENTS = 4;
/// Largest constant magnitude, LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT.
inline constexpr foundation::float32 MAX_MATERIAL_PROGRAM_CONSTANT = LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT;
/// Register operand value of a lobe that reads no roughness register.
inline constexpr foundation::uint8 NO_MATERIAL_PROGRAM_REGISTER = LUDUS_MATERIAL_PROGRAM_NO_REGISTER;
/// One-byte fields of an instruction: opcode, destination and three operands.
inline constexpr foundation::usize MATERIAL_PROGRAM_INSTRUCTION_FIELDS = 5;
/// One-byte fields of a lobe before its padding: operation, weight and roughness registers.
inline constexpr foundation::usize MATERIAL_PROGRAM_LOBE_BYTE_FIELDS = 3;
/// Scalar float32 lobe fields: Eta, K and Power.
inline constexpr foundation::usize MATERIAL_PROGRAM_LOBE_FLOATS = MATERIAL_RGB_CHANNELS + MATERIAL_RGB_CHANNELS + 1;
/// Leading uint32 program fields: version and the three counts.
inline constexpr foundation::usize MATERIAL_PROGRAM_HEADER_FIELDS = 4;
/// Padding bytes completing one eight-byte instruction.
inline constexpr foundation::usize MATERIAL_PROGRAM_INSTRUCTION_PADDING = 3;
/// Padding bytes after a lobe's four one-byte fields; keeps its floats four-byte aligned.
inline constexpr foundation::usize MATERIAL_PROGRAM_LOBE_PADDING = 1;

/// Version-1 instruction. Each instruction reads every operand before it writes its destination.
enum class MaterialProgramOpcode : foundation::uint8
{
    None = 0, ///< Unused instruction slot; never valid inside InstructionCount.
    Constant = LUDUS_MATERIAL_PROGRAM_OPCODE_CONSTANT, ///< Destination = Constants[OperandA].
    SurfaceInput =
        LUDUS_MATERIAL_PROGRAM_OPCODE_SURFACE_INPUT,    ///< Destination = the MaterialProgramInput named by OperandA.
    Add = LUDUS_MATERIAL_PROGRAM_OPCODE_ADD,            ///< Destination = registers OperandA + OperandB.
    Multiply = LUDUS_MATERIAL_PROGRAM_OPCODE_MULTIPLY,  ///< Destination = registers OperandA * OperandB.
    Lerp = LUDUS_MATERIAL_PROGRAM_OPCODE_LERP,          ///< Destination = A + (B - A) * C for registers A, B, C.
    OneMinus = LUDUS_MATERIAL_PROGRAM_OPCODE_ONE_MINUS, ///< Destination = 1 - register OperandA.
    Clamp = LUDUS_MATERIAL_PROGRAM_OPCODE_CLAMP,        ///< Destination = register OperandA clamped to [0,1].
};
/// Last valid version-1 opcode.
inline constexpr MaterialProgramOpcode LAST_MATERIAL_PROGRAM_OPCODE = MaterialProgramOpcode::Clamp;

/// Shading-point values a SurfaceInput instruction loads, after the material's textures are applied.
enum class MaterialProgramInput : foundation::uint8
{
    BaseColor = LUDUS_MATERIAL_PROGRAM_INPUT_BASE_COLOR, ///< Linear RGB base colour (reflectance times base texture).
    Roughness = LUDUS_MATERIAL_PROGRAM_INPUT_ROUGHNESS,  ///< Perceptual roughness in each RGB component.
    Metallic = LUDUS_MATERIAL_PROGRAM_INPUT_METALLIC,    ///< Metallic fraction in each RGB component.
};
/// Last valid surface input.
inline constexpr MaterialProgramInput LAST_MATERIAL_PROGRAM_INPUT = MaterialProgramInput::Metallic;

/// Leaf lobe operation. Every version-1 operation is a continuous reflection BSDF whose sampler
/// consumes two uniform values.
enum class MaterialLobeOperation : foundation::uint8
{
    None = 0,                                       ///< Unused lobe slot; never valid inside LobeCount.
    Diffuse = LUDUS_MATERIAL_LOBE_DIFFUSE,          ///< Lambertian reflection, 1/pi before the weight.
    Conductor = LUDUS_MATERIAL_LOBE_CONDUCTOR,      ///< Isotropic GGX, complex-IOR Fresnel; alpha = roughness^2.
    CosinePower = LUDUS_MATERIAL_LOBE_COSINE_POWER, ///< Reciprocal cosine power, exponent (1 - roughness) * Power.
};
/// Last valid lobe operation.
inline constexpr MaterialLobeOperation LAST_MATERIAL_LOBE_OPERATION = MaterialLobeOperation::CosinePower;

/// One straight-line instruction; eight bytes on the wire. Unused operands are zero.
struct MaterialProgramInstruction final
{
    MaterialProgramOpcode Opcode = MaterialProgramOpcode::None; ///< Operation; None only in unused slots.
    foundation::uint8 Destination = 0; ///< Register written, below MAX_MATERIAL_PROGRAM_REGISTERS.
    foundation::uint8 OperandA = 0;    ///< Constant index, MaterialProgramInput or first source register.
    foundation::uint8 OperandB = 0;    ///< Second source register for Add, Multiply and Lerp.
    foundation::uint8 OperandC = 0;    ///< Interpolation register for Lerp.
    foundation::StaticArray<foundation::uint8, MATERIAL_PROGRAM_INSTRUCTION_PADDING> Reserved{}; ///< Zero.
};

/// One leaf lobe of the weighted mixture; 32 bytes on the wire. Parameters an operation does not
/// use are zero.
struct MaterialProgramLobe final
{
    MaterialLobeOperation Operation = MaterialLobeOperation::None; ///< Operation; None only in unused slots.
    /// Register whose RGB components weight the lobe. Evaluators clamp negative weights to zero.
    foundation::uint8 WeightRegister = 0;
    /// Register whose first component is perceptual roughness for Conductor and CosinePower,
    /// NO_MATERIAL_PROGRAM_REGISTER for Diffuse, and zero in unused slots. Evaluators clamp it to [MIN_ROUGHNESS,1].
    foundation::uint8 RoughnessRegister = 0;
    foundation::StaticArray<foundation::uint8, MATERIAL_PROGRAM_LOBE_PADDING> Reserved{}; ///< Zero.
    /// Conductor complex-IOR real part, RGB proxy, in (0,MAX_MATERIAL_OPTICAL_CONSTANT].
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS> Eta{};
    /// Conductor complex-IOR imaginary part, RGB proxy, in [0,MAX_MATERIAL_OPTICAL_CONSTANT].
    foundation::StaticArray<foundation::float32, MATERIAL_RGB_CHANNELS> K{};
    /// CosinePower exponent at zero roughness, in [0,LUDUS_PROJECT_BRDF_MAX_POWER].
    foundation::float32 Power = 0;
};

/// A complete bounded closure program, also the LUSCN version-11 program record.
struct MaterialProgram final
{
    foundation::uint32 Version = MATERIAL_PROGRAM_VERSION_1; ///< MATERIAL_PROGRAM_VERSION_1.
    foundation::uint32 ConstantCount = 0;    ///< Used Constants, at most MAX_MATERIAL_PROGRAM_CONSTANTS.
    foundation::uint32 InstructionCount = 0; ///< Used Instructions, 1..MAX_MATERIAL_PROGRAM_INSTRUCTIONS.
    foundation::uint32 LobeCount = 0;        ///< Used Lobes, 1..MAX_MATERIAL_PROGRAM_LOBES.
    /// Finite constants with magnitude at most MAX_MATERIAL_PROGRAM_CONSTANT.
    foundation::StaticArray<foundation::StaticArray<foundation::float32, MATERIAL_PROGRAM_COMPONENTS>,
                            MAX_MATERIAL_PROGRAM_CONSTANTS>
        Constants{};
    foundation::StaticArray<MaterialProgramInstruction, MAX_MATERIAL_PROGRAM_INSTRUCTIONS>
        Instructions{};                                                               ///< Straight-line parameter code.
    foundation::StaticArray<MaterialProgramLobe, MAX_MATERIAL_PROGRAM_LOBES> Lobes{}; ///< Weighted leaf lobes.
};

/// Exact serialized instruction width, derived from its field schema.
inline constexpr foundation::usize MATERIAL_PROGRAM_INSTRUCTION_BYTES =
    MATERIAL_PROGRAM_INSTRUCTION_FIELDS * sizeof(foundation::uint8) + MATERIAL_PROGRAM_INSTRUCTION_PADDING;
/// Exact serialized lobe width, derived from its field schema.
inline constexpr foundation::usize MATERIAL_PROGRAM_LOBE_BYTES =
    MATERIAL_PROGRAM_LOBE_BYTE_FIELDS * sizeof(foundation::uint8) + MATERIAL_PROGRAM_LOBE_PADDING +
    MATERIAL_PROGRAM_LOBE_FLOATS * sizeof(foundation::float32);
/// Exact serialized program record width, derived from its field schema.
inline constexpr foundation::usize MATERIAL_PROGRAM_BYTES =
    MATERIAL_PROGRAM_HEADER_FIELDS * sizeof(foundation::uint32) +
    MAX_MATERIAL_PROGRAM_CONSTANTS * MATERIAL_PROGRAM_COMPONENTS * sizeof(foundation::float32) +
    MAX_MATERIAL_PROGRAM_INSTRUCTIONS * MATERIAL_PROGRAM_INSTRUCTION_BYTES +
    MAX_MATERIAL_PROGRAM_LOBES * MATERIAL_PROGRAM_LOBE_BYTES;
static_assert(std::is_trivially_copyable_v<MaterialProgram> && std::is_standard_layout_v<MaterialProgram>);
static_assert(sizeof(MaterialProgramInstruction) == MATERIAL_PROGRAM_INSTRUCTION_BYTES &&
              offsetof(MaterialProgramInstruction, Reserved) == MATERIAL_PROGRAM_INSTRUCTION_FIELDS);
static_assert(sizeof(MaterialProgramLobe) == MATERIAL_PROGRAM_LOBE_BYTES &&
              offsetof(MaterialProgramLobe, Eta) == MATERIAL_PROGRAM_LOBE_BYTE_FIELDS + MATERIAL_PROGRAM_LOBE_PADDING);
static_assert(sizeof(MaterialProgram) == MATERIAL_PROGRAM_BYTES &&
              offsetof(MaterialProgram, Constants) == MATERIAL_PROGRAM_HEADER_FIELDS * sizeof(foundation::uint32));

/// Report the capabilities one lobe operation guarantees.
/// @return LUDUS_MATERIAL_CAPABILITY_* bits; zero for None or an unknown operation.
[[nodiscard]] constexpr MaterialCapabilityBits GetMaterialLobeCapabilities(MaterialLobeOperation operation) noexcept
{
    switch (operation)
    {
        case MaterialLobeOperation::Diffuse:
        case MaterialLobeOperation::Conductor:
        case MaterialLobeOperation::CosinePower:
            return LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION | LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER;
        case MaterialLobeOperation::None:
            break;
    }
    return 0;
}

/// Combine the program's lobe capabilities. The mixture offers continuous reflection and a fixed
/// sampler only when every lobe does, and carries any lobe's transmission, internal walk or delta.
/// @param program A program that IsMaterialProgramValid accepts.
/// @return LUDUS_MATERIAL_CAPABILITY_* bits; zero for a program without lobes.
[[nodiscard]] constexpr MaterialCapabilityBits GetMaterialProgramCapabilities(const MaterialProgram& program) noexcept
{
    constexpr MaterialCapabilityBits SHARED_CAPABILITIES =
        LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION | LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER;
    if (program.LobeCount == 0 || program.LobeCount > MAX_MATERIAL_PROGRAM_LOBES)
    {
        return 0;
    }
    MaterialCapabilityBits shared = SHARED_CAPABILITIES;
    MaterialCapabilityBits carried = 0;
    for (foundation::usize lobe = 0; lobe < program.LobeCount; ++lobe)
    {
        const MaterialCapabilityBits capabilities = GetMaterialLobeCapabilities(program.Lobes[lobe].Operation);
        shared &= capabilities;
        carried |= capabilities & ~SHARED_CAPABILITIES;
    }
    return shared | carried;
}

/// Validate a program completely: version and counts, finite bounded constants, every operand
/// defined before it is read, operation parameters, and zero unused slots and padding.
/// Weights and roughness computed at a shading point are clamped by evaluators, not here.
/// @return True when an evaluator may execute the program without further checks.
[[nodiscard]] constexpr bool IsMaterialProgramValid(const MaterialProgram& program) noexcept
{
    using foundation::uint32;
    using foundation::usize;
    if (program.Version != MATERIAL_PROGRAM_VERSION_1 || program.ConstantCount > MAX_MATERIAL_PROGRAM_CONSTANTS ||
        program.InstructionCount == 0 || program.InstructionCount > MAX_MATERIAL_PROGRAM_INSTRUCTIONS ||
        program.LobeCount == 0 || program.LobeCount > MAX_MATERIAL_PROGRAM_LOBES)
    {
        return false;
    }
    for (usize constant = 0; constant < MAX_MATERIAL_PROGRAM_CONSTANTS; ++constant)
    {
        for (const foundation::float32 value : program.Constants[constant])
        {
            const bool isUsed = constant < program.ConstantCount;
            if ((isUsed == true &&
                 (value >= -MAX_MATERIAL_PROGRAM_CONSTANT && value <= MAX_MATERIAL_PROGRAM_CONSTANT) == false) ||
                (isUsed == false && value != 0))
            {
                return false;
            }
        }
    }
    uint32 written = 0;
    const auto isWritten = [&written](foundation::uint8 index) noexcept {
        return index < MAX_MATERIAL_PROGRAM_REGISTERS && (written & (1U << index)) != 0;
    };
    for (usize index = 0; index < MAX_MATERIAL_PROGRAM_INSTRUCTIONS; ++index)
    {
        const MaterialProgramInstruction& instruction = program.Instructions[index];
        for (const foundation::uint8 padding : instruction.Reserved)
        {
            if (padding != 0)
            {
                return false;
            }
        }
        if (index >= program.InstructionCount)
        {
            if (instruction.Opcode != MaterialProgramOpcode::None || instruction.Destination != 0 ||
                instruction.OperandA != 0 || instruction.OperandB != 0 || instruction.OperandC != 0)
            {
                return false;
            }
            continue;
        }
        // Decoded bytes may hold opcodes outside the enumeration; the switch below covers only known ones.
        bool isValid = instruction.Destination < MAX_MATERIAL_PROGRAM_REGISTERS &&
                       instruction.Opcode <= LAST_MATERIAL_PROGRAM_OPCODE;
        switch (instruction.Opcode)
        {
            case MaterialProgramOpcode::Constant:
                isValid = isValid == true && instruction.OperandA < program.ConstantCount &&
                          instruction.OperandB == 0 && instruction.OperandC == 0;
                break;
            case MaterialProgramOpcode::SurfaceInput:
                isValid = isValid == true &&
                          instruction.OperandA <= static_cast<foundation::uint8>(LAST_MATERIAL_PROGRAM_INPUT) &&
                          instruction.OperandB == 0 && instruction.OperandC == 0;
                break;
            case MaterialProgramOpcode::Add:
            case MaterialProgramOpcode::Multiply:
                isValid = isValid == true && isWritten(instruction.OperandA) == true &&
                          isWritten(instruction.OperandB) == true && instruction.OperandC == 0;
                break;
            case MaterialProgramOpcode::Lerp:
                isValid = isValid == true && isWritten(instruction.OperandA) == true &&
                          isWritten(instruction.OperandB) == true && isWritten(instruction.OperandC) == true;
                break;
            case MaterialProgramOpcode::OneMinus:
            case MaterialProgramOpcode::Clamp:
                isValid = isValid == true && isWritten(instruction.OperandA) == true && instruction.OperandB == 0 &&
                          instruction.OperandC == 0;
                break;
            case MaterialProgramOpcode::None:
                isValid = false;
                break;
        }
        if (isValid == false)
        {
            return false;
        }
        written |= 1U << instruction.Destination;
    }
    for (usize index = 0; index < MAX_MATERIAL_PROGRAM_LOBES; ++index)
    {
        const MaterialProgramLobe& lobe = program.Lobes[index];
        if (lobe.Reserved[0] != 0)
        {
            return false;
        }
        const bool isConductor = lobe.Operation == MaterialLobeOperation::Conductor;
        const bool isCosinePower = lobe.Operation == MaterialLobeOperation::CosinePower;
        for (usize channel = 0; channel < MATERIAL_RGB_CHANNELS; ++channel)
        {
            const bool isEtaValid = isConductor == true
                                        ? lobe.Eta[channel] > 0 && lobe.Eta[channel] <= MAX_MATERIAL_OPTICAL_CONSTANT
                                        : lobe.Eta[channel] == 0;
            const bool isKValid = isConductor == true
                                      ? lobe.K[channel] >= 0 && lobe.K[channel] <= MAX_MATERIAL_OPTICAL_CONSTANT
                                      : lobe.K[channel] == 0;
            if (isEtaValid == false || isKValid == false)
            {
                return false;
            }
        }
        const bool isPowerValid =
            isCosinePower == true ? lobe.Power >= 0 && lobe.Power <= LUDUS_PROJECT_BRDF_MAX_POWER : lobe.Power == 0;
        if (index >= program.LobeCount)
        {
            if (lobe.Operation != MaterialLobeOperation::None || lobe.WeightRegister != 0 ||
                lobe.RoughnessRegister != 0 || isPowerValid == false)
            {
                return false;
            }
            continue;
        }
        const bool hasRoughness = isConductor == true || isCosinePower == true;
        const bool isRoughnessValid = hasRoughness == true ? isWritten(lobe.RoughnessRegister)
                                                           : lobe.RoughnessRegister == NO_MATERIAL_PROGRAM_REGISTER;
        if (GetMaterialLobeCapabilities(lobe.Operation) == 0 || isWritten(lobe.WeightRegister) == false ||
            isRoughnessValid == false || isPowerValid == false)
        {
            return false;
        }
    }
    return true;
}
/// Capabilities of a material, taking a closure program's from the program.
/// @param scattering The material's typed scattering.
/// @param program The material's program; read only when scattering.Model is Program.
/// @return LUDUS_MATERIAL_CAPABILITY_* bits.
[[nodiscard]] constexpr MaterialCapabilityBits GetMaterialCapabilities(const MaterialScattering& scattering,
                                                                       const MaterialProgram& program) noexcept
{
    return scattering.Model == MaterialModel::Program ? GetMaterialProgramCapabilities(program)
                                                      : GetMaterialCapabilities(scattering);
}
/// Test the fixed-dimension reflection guarantee of a material and its program.
/// @param scattering The material's typed scattering.
/// @param program The material's program; read only when scattering.Model is Program.
/// @return True when the material satisfies FIXED_DIMENSION_REFLECTION_REQUIREMENT.
[[nodiscard]] constexpr bool SupportsFixedDimensionReflection(const MaterialScattering& scattering,
                                                              const MaterialProgram& program) noexcept
{
    return SatisfiesMaterialRequirement(GetMaterialCapabilities(scattering, program),
                                        FIXED_DIMENSION_REFLECTION_REQUIREMENT);
}
} // namespace ludus::content::scene_format
#else
/// Slang spelling of a program instruction's opcode; convert stored words explicitly.
enum MaterialProgramOpcode : MaterialProgramTag
{
    Constant = LUDUS_MATERIAL_PROGRAM_OPCODE_CONSTANT,          ///< Load a constant.
    SurfaceInput = LUDUS_MATERIAL_PROGRAM_OPCODE_SURFACE_INPUT, ///< Load a shading-point input.
    Add = LUDUS_MATERIAL_PROGRAM_OPCODE_ADD,                    ///< Sum two registers.
    Multiply = LUDUS_MATERIAL_PROGRAM_OPCODE_MULTIPLY,          ///< Multiply two registers.
    Lerp = LUDUS_MATERIAL_PROGRAM_OPCODE_LERP,                  ///< Interpolate two registers by a third.
    OneMinus = LUDUS_MATERIAL_PROGRAM_OPCODE_ONE_MINUS,         ///< Complement a register.
    Clamp = LUDUS_MATERIAL_PROGRAM_OPCODE_CLAMP,                ///< Clamp a register to [0,1].
};
/// Slang spelling of a SurfaceInput operand.
enum MaterialProgramInput : MaterialProgramTag
{
    BaseColor = LUDUS_MATERIAL_PROGRAM_INPUT_BASE_COLOR, ///< Textured base colour.
    Roughness = LUDUS_MATERIAL_PROGRAM_INPUT_ROUGHNESS,  ///< Perceptual roughness.
    Metallic = LUDUS_MATERIAL_PROGRAM_INPUT_METALLIC,    ///< Metallic fraction.
};
/// Slang spelling of a leaf lobe operation.
enum MaterialLobeOperation : MaterialProgramTag
{
    Diffuse = LUDUS_MATERIAL_LOBE_DIFFUSE,          ///< Lambertian reflection.
    Conductor = LUDUS_MATERIAL_LOBE_CONDUCTOR,      ///< Isotropic GGX conductor.
    CosinePower = LUDUS_MATERIAL_LOBE_COSINE_POWER, ///< Reciprocal cosine-power term.
};
#endif // __cplusplus
