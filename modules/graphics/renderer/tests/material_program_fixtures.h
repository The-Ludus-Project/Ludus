#pragma once

// Closure programs shared by the renderer's CPU/GPU parity and render-graph tests.

#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::renderer_tests
{
/// Isotropic GGX roughness of the grain mix's glossy gold lobe; its alpha is the square, 0.09.
inline constexpr ludus::foundation::float32 GRAIN_MIX_ROUGHNESS = 0.3F;
/// Gold's complex index of refraction, RGB proxy, shared by the grain mix and the parity program.
inline constexpr ludus::foundation::StaticArray<ludus::foundation::float32,
                                                ludus::content::scene_format::MATERIAL_RGB_CHANNELS>
    GOLD_ETA{{0.143F, 0.374F, 1.442F}};
/// Imaginary part of gold's index of refraction, RGB proxy.
inline constexpr ludus::foundation::StaticArray<ludus::foundation::float32,
                                                ludus::content::scene_format::MATERIAL_RGB_CHANNELS>
    GOLD_K{{3.983F, 2.386F, 1.603F}};

/// A three-lobe closure program for CPU/GPU parity and graph tests: a rust diffuse weighted by (1 - metallic) times the
/// base colour, a gold conductor weighted by metallic, and a cosine-power sheen whose weight interpolates a constant
/// tint towards the diffuse weight.
inline ludus::content::scene_format::MaterialProgram MakeParityProgram()
{
    namespace format = ludus::content::scene_format;
    using format::MaterialProgramOpcode;
    format::MaterialProgram program;
    program.ConstantCount = 2;
    program.Constants[0] = {{0.5F, 0.5F, 0.5F, 0}};
    program.Constants[1] = {{0.3F, 0.2F, 0.1F, 0}};
    constexpr ludus::foundation::usize INSTRUCTION_COUNT = 8;
    const ludus::foundation::StaticArray<format::MaterialProgramInstruction, INSTRUCTION_COUNT> instructions{{
        {
            .Opcode = MaterialProgramOpcode::SurfaceInput,
            .Destination = 0,
            .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::Metallic),
        },
        { .Opcode = MaterialProgramOpcode::OneMinus, .Destination = 1, .OperandA = 0 },
        {
            .Opcode = MaterialProgramOpcode::SurfaceInput,
            .Destination = 2,
            .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::BaseColor),
        },
        { .Opcode = MaterialProgramOpcode::Multiply, .Destination = 3, .OperandA = 1, .OperandB = 2 },
        {
            .Opcode = MaterialProgramOpcode::SurfaceInput,
            .Destination = 4,
            .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::Roughness),
        },
        { .Opcode = MaterialProgramOpcode::Constant, .Destination = 5, .OperandA = 1 },
        { .Opcode = MaterialProgramOpcode::Constant, .Destination = 6, .OperandA = 0 },
        { .Opcode = MaterialProgramOpcode::Lerp, .Destination = 7, .OperandA = 5, .OperandB = 3, .OperandC = 6 },
    }};
    for (const format::MaterialProgramInstruction& instruction : instructions)
    {
        program.Instructions[program.InstructionCount] = instruction;
        ++program.InstructionCount;
    }
    program.LobeCount = 3;
    program.Lobes[0] =
    {
        .Operation = format::MaterialLobeOperation::Diffuse,
        .WeightRegister = 3,
        .RoughnessRegister = format::NO_MATERIAL_PROGRAM_REGISTER,
    };
    program.Lobes[1] =
    {
        .Operation = format::MaterialLobeOperation::Conductor,
        .WeightRegister = 0,
        .RoughnessRegister = 4,
        .Eta = GOLD_ETA,
        .K = GOLD_K,
    };
    program.Lobes[2] =
    {
        .Operation = format::MaterialLobeOperation::CosinePower,
        .WeightRegister = 7,
        .RoughnessRegister = 4,
        .Power = 12,
    };
    return program;
}

/// A texture-weighted mix of two distinct lobes after Robert L. Cook, "Shade Trees", SIGGRAPH '84, pp. 223–231,
/// Fig. 1b (https://graphics.pixar.com/library/ShadeTrees/paper.pdf): a scalar grain mask m arrives through the base
/// colour input, a white Lambertian lobe takes the complement 1 - m and a glossy gold conductor takes m. Cook's wood
/// mixes two separately shaded responses rather than blending their parameters, so each lobe keeps its own
/// directional behaviour; here the mix is a flat closure program whose surface value is (1 - m) f_d + m f_c and whose
/// density is (1 - m) p_d + m p_c. The record is exactly what tests/scenes/materials/grain-mix-board.luscene.json
/// compiles to; the `[scene-source]` check of ludus_render_graph_ray_tests requires the two to stay equal.
inline ludus::content::scene_format::MaterialProgram MakeGrainMixProgram()
{
    namespace format = ludus::content::scene_format;
    using format::MaterialProgramOpcode;
    format::MaterialProgram program;
    program.InstructionCount = 3;
    program.Instructions[0] =
    {
        .Opcode = MaterialProgramOpcode::SurfaceInput,
        .Destination = 0,
        .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::BaseColor),
    };
    program.Instructions[1] = { .Opcode = MaterialProgramOpcode::OneMinus, .Destination = 1, .OperandA = 0 };
    program.Instructions[2] =
    {
        .Opcode = MaterialProgramOpcode::SurfaceInput,
        .Destination = 2,
        .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::Roughness),
    };
    program.LobeCount = 2;
    program.Lobes[0] =
    {
        .Operation = format::MaterialLobeOperation::Diffuse,
        .WeightRegister = 1,
        .RoughnessRegister = format::NO_MATERIAL_PROGRAM_REGISTER,
    };
    program.Lobes[1] =
    {
        .Operation = format::MaterialLobeOperation::Conductor,
        .WeightRegister = 0,
        .RoughnessRegister = 2,
        .Eta = GOLD_ETA,
        .K = GOLD_K,
    };
    return program;
}
} // namespace ludus::renderer_tests
