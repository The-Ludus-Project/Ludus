#pragma once

// Closure programs shared by the renderer's CPU/GPU parity and render-graph tests.

#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::renderer_tests
{
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
        .Eta = {{0.143F, 0.374F, 1.442F}},
        .K = {{3.983F, 2.386F, 1.603F}},
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
} // namespace ludus::renderer_tests
