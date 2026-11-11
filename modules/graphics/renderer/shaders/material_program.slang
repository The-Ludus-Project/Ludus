#pragma once
#include "material_program.h"
#include "material_scattering.slang"
#include "math.slang"
#include "renderer_gpu_preview_layout.h"

// Closure-program interpreter shared by the GPU preview, the GPU reference and the render-graph scene interface,
// paired with the CPU oracle in reference_bsdf.cpp (EvaluateMaterialProgram/SampleMaterialProgram); keep the
// arithmetic in the same order. The includer defines SceneValue(uint), which reads the packed scene words that
// PackGpuScene writes (LUDUS_GPU_PREVIEW_PROGRAM_HEADER_WORDS). Thanks to Pharr, Jakob and Humphreys, Physically
// Based Rendering, 4th ed., section 2.2.3, https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency:
// the weighted mixture f = sum(w_i f_i) samples one lobe in proportion to its selection weight and returns the full
// mixture density sum(s_i p_i) / sum(s_i). See docs/architecture/renderer-systems.md#material-closure-programs.

/// Smallest lobe roughness, scene_format::MIN_ROUGHNESS.
static const float MATERIAL_PROGRAM_MIN_ROUGHNESS = .045;
/// Lower bound of the height-correlated Smith visibility denominator, as reference_bsdf.cpp.
static const float MATERIAL_PROGRAM_SMITH_FLOOR = 1e-6;
/// Basis pole threshold of reference_bsdf.cpp's Basis.
static const float MATERIAL_PROGRAM_BASIS_POLE_COSINE = .999;
/// Channels averaged into a lobe's selection weight.
static const float MATERIAL_PROGRAM_CHANNELS = 3;

/// Shading-point inputs of a program: textured base colour, perceptual roughness and metallic fraction.
struct MaterialProgramInputs
{
    float3 BaseColor;
    float Roughness;
    float Metallic;
};

/// One lobe after its weight and roughness registers were read.
struct MaterialProgramLobe
{
    MaterialLobeOperation Operation;
    float3 Weight;
    float Selection;
    float Roughness;
    float Power;
    float3 Eta;
    float3 K;
};

uint GetMaterialProgramInstructionCount(uint offset)
{
    return asuint(SceneValue(offset).x);
}
uint GetMaterialProgramLobeCount(uint offset)
{
    return asuint(SceneValue(offset).y);
}
uint GetMaterialProgramLobeWord(uint offset, uint lobe)
{
    return offset + LUDUS_GPU_PREVIEW_PROGRAM_HEADER_WORDS + asuint(SceneValue(offset).z) +
           GetMaterialProgramInstructionCount(offset) + lobe * LUDUS_GPU_PREVIEW_PROGRAM_LOBE_WORDS;
}

/// The parameter registers of one shading point. Metal has lost stores to large thread arrays in merged graph
/// kernels, so the interpreter keeps this one small array, resets every field and streams the lobes from scene words
/// instead of holding them in a second array.
struct MaterialProgramRegisters
{
    float3 Values[LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS];
};

/// Run the parameter instructions; the validator guarantees each register is written before it is read.
MaterialProgramRegisters RunMaterialProgram(uint offset, MaterialProgramInputs inputs)
{
    MaterialProgramRegisters registers;
    [unroll] for (uint index = 0; index < LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS; ++index) registers.Values[index] =
        float3(0);
    uint instructionCount = GetMaterialProgramInstructionCount(offset);
    uint constants = offset + LUDUS_GPU_PREVIEW_PROGRAM_HEADER_WORDS;
    uint instructions = constants + asuint(SceneValue(offset).z);
    [loop] for (uint index = 0; index < instructionCount; ++index)
    {
        uint4 word = asuint(SceneValue(instructions + index));
        MaterialProgramOpcode opcode = MaterialProgramOpcode(word.x);
        uint operandB = word.w & ((1u << LUDUS_GPU_PREVIEW_PROGRAM_OPERAND_SHIFT) - 1u);
        uint operandC = word.w >> LUDUS_GPU_PREVIEW_PROGRAM_OPERAND_SHIFT;
        float3 first = registers.Values[word.z % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS];
        float3 second = registers.Values[operandB % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS];
        float3 third = registers.Values[operandC % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS];
        float3 result = float3(0);
        if (opcode == MaterialProgramOpcode::Constant)
            result = SceneValue(constants + word.z).xyz;
        else if (opcode == MaterialProgramOpcode::SurfaceInput)
            result = MaterialProgramInput(word.z) == MaterialProgramInput::BaseColor   ? inputs.BaseColor
                     : MaterialProgramInput(word.z) == MaterialProgramInput::Roughness ? float3(inputs.Roughness)
                                                                                       : float3(inputs.Metallic);
        else if (opcode == MaterialProgramOpcode::Add)
            result = first + second;
        else if (opcode == MaterialProgramOpcode::Multiply)
            result = first * second;
        else if (opcode == MaterialProgramOpcode::Lerp)
            result = first + (second - first) * third;
        else if (opcode == MaterialProgramOpcode::OneMinus)
            result = float3(1) - first;
        else if (opcode == MaterialProgramOpcode::Clamp)
            result = clamp(first, float3(0), float3(1));
        registers.Values[word.y % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS] =
            clamp(result, float3(-LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT), float3(LUDUS_MATERIAL_PROGRAM_MAX_CONSTANT));
    }
    return registers;
}

/// Read one lobe's parameters and its weight and roughness registers.
MaterialProgramLobe ReadMaterialProgramLobe(uint offset, uint lobe, MaterialProgramRegisters registers)
{
    uint first = GetMaterialProgramLobeWord(offset, lobe);
    float4 operation = SceneValue(first);
    MaterialProgramLobe result;
    result.Operation = MaterialLobeOperation(asuint(operation.x));
    float3 weight = max(registers.Values[asuint(operation.y) % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS], float3(0));
    result.Weight = weight;
    result.Selection = (weight.x + weight.y + weight.z) / MATERIAL_PROGRAM_CHANNELS;
    uint roughnessRegister = asuint(operation.z);
    result.Roughness = roughnessRegister == LUDUS_MATERIAL_PROGRAM_NO_REGISTER
                           ? 1
                           : clamp(registers.Values[roughnessRegister % LUDUS_MATERIAL_PROGRAM_MAX_REGISTERS].x,
                                   MATERIAL_PROGRAM_MIN_ROUGHNESS,
                                   1);
    result.Power = operation.w;
    result.Eta = SceneValue(first + 1).xyz;
    result.K = SceneValue(first + 2).xyz;
    return result;
}

/// Sum of the lobes' selection weights.
float GetMaterialProgramSelectionTotal(uint offset, MaterialProgramRegisters registers)
{
    float total = 0;
    uint lobeCount = GetMaterialProgramLobeCount(offset);
    [loop] for (uint lobe = 0; lobe < lobeCount; ++lobe) total +=
        ReadMaterialProgramLobe(offset, lobe, registers).Selection;
    return total;
}

/// One lobe's value without its weight, and its own solid-angle density.
float4 EvaluateMaterialProgramLobe(MaterialProgramLobe lobe,
                                   float3 normalWorld,
                                   float3 viewWorld,
                                   float3 lightWorld,
                                   float nv,
                                   float nl)
{
    if (lobe.Operation == MaterialLobeOperation::Diffuse)
        return float4(float3(1 / PI), nl / PI);
    if (lobe.Operation == MaterialLobeOperation::Conductor)
    {
        float3 halfWorld = NormalizeOrZero(viewWorld + lightWorld);
        float vh = clamp(dot(viewWorld, halfWorld), 0, 1);
        float alpha = lobe.Roughness * lobe.Roughness;
        float alphaSquared = alpha * alpha;
        float distribution = ComputeGgxNormalDistribution(normalWorld, halfWorld, alphaSquared);
        float lambdaView = sqrt(nv * nv * (1 - alphaSquared) + alphaSquared);
        float lambdaLight = sqrt(nl * nl * (1 - alphaSquared) + alphaSquared);
        float visibility = .5 / max(nl * lambdaView + nv * lambdaLight, MATERIAL_PROGRAM_SMITH_FLOOR);
        float3 fresnel = ComputeConductorFresnelRgb(vh, lobe.Eta, lobe.K);
        return float4(fresnel * (distribution * visibility), distribution / (2 * (nv + lambdaView)));
    }
    float exponent = (1 - lobe.Roughness) * lobe.Power;
    float viewLightCosine = min(1, nv * nl);
    float lightCosine = min(1, nl);
    float scale = (exponent + 2) / (2 * PI);
    float value = exponent == 0 ? scale : scale * pow(viewLightCosine, exponent);
    return float4(float3(value), scale * pow(lightCosine, exponent + 1));
}

/// Value and full mixture density of the program at offset, without the cosine; zero outside the upper hemisphere
/// and for a program without positive lobe weight.
float4 EvaluateMaterialProgram(uint offset,
                               MaterialProgramInputs inputs,
                               float3 normalWorld,
                               float3 viewWorld,
                               float3 lightWorld)
{
    float nv = dot(normalWorld, viewWorld);
    float nl = dot(normalWorld, lightWorld);
    if (nv <= 0 || nl <= 0)
        return 0;
    MaterialProgramRegisters registers = RunMaterialProgram(offset, inputs);
    float total = GetMaterialProgramSelectionTotal(offset, registers);
    if (total <= 0)
        return 0;
    uint lobeCount = GetMaterialProgramLobeCount(offset);
    float3 value = float3(0);
    float pdf = 0;
    [loop] for (uint lobe = 0; lobe < lobeCount; ++lobe)
    {
        MaterialProgramLobe parameters = ReadMaterialProgramLobe(offset, lobe, registers);
        float4 evaluated = EvaluateMaterialProgramLobe(parameters, normalWorld, viewWorld, lightWorld, nv, nl);
        value = value + parameters.Weight * evaluated.xyz;
        pdf += parameters.Selection * evaluated.w;
    }
    return float4(value, pdf / total);
}

/// Select one lobe with samples.x in proportion to its selection weight and sample its direction with samples.yz.
/// Returns zero for a null proposal: below the surface, or a program without positive lobe weight.
float3 SampleMaterialProgramDirection(uint offset,
                                      MaterialProgramInputs inputs,
                                      float3 normalWorld,
                                      float3 viewWorld,
                                      float3 samples)
{
    if (dot(normalWorld, viewWorld) <= 0)
        return 0;
    MaterialProgramRegisters registers = RunMaterialProgram(offset, inputs);
    float total = GetMaterialProgramSelectionTotal(offset, registers);
    if (total <= 0)
        return 0;
    uint lobeCount = GetMaterialProgramLobeCount(offset);
    float selector = samples.x * total;
    float cumulative = 0;
    uint selected = 0;
    // Rounding can leave the selector at the total; the last positive lobe then takes it.
    [loop] for (uint lobe = 0; lobe < lobeCount; ++lobe)
    {
        float selection = ReadMaterialProgramLobe(offset, lobe, registers).Selection;
        if (selection > 0)
        {
            selected = lobe;
            cumulative += selection;
            if (selector < cumulative)
                break;
        }
    }
    MaterialProgramLobe chosen = ReadMaterialProgramLobe(offset, selected, registers);
    float3 axisXWorld = NormalizeOrZero(
        cross(abs(normalWorld.z) < MATERIAL_PROGRAM_BASIS_POLE_COSINE ? float3(0, 0, 1) : float3(0, 1, 0),
              normalWorld));
    float3 axisYWorld = cross(normalWorld, axisXWorld);
    float radius = sqrt(samples.y);
    float angle = 2 * PI * samples.z;
    float diskX = radius * cos(angle);
    float diskY = radius * sin(angle);
    float3 directionWorld;
    if (chosen.Operation == MaterialLobeOperation::Diffuse)
        directionWorld = axisXWorld * diskX + axisYWorld * diskY + normalWorld * sqrt(max(0, 1 - samples.y));
    else if (chosen.Operation == MaterialLobeOperation::Conductor)
    {
        float alpha = chosen.Roughness * chosen.Roughness;
        float3 viewTangent =
            float3(dot(viewWorld, axisXWorld), dot(viewWorld, axisYWorld), dot(viewWorld, normalWorld));
        float3 stretchedTangent = NormalizeOrZero(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));
        float tangentLength = stretchedTangent.x * stretchedTangent.x + stretchedTangent.y * stretchedTangent.y;
        float3 firstTangent = tangentLength > 0
                                  ? float3(-stretchedTangent.y, stretchedTangent.x, 0) / sqrt(tangentLength)
                                  : float3(1, 0, 0);
        float3 secondTangent = cross(stretchedTangent, firstTangent);
        float blend = .5 * (1 + stretchedTangent.z);
        float warpedY = (1 - blend) * sqrt(max(0, 1 - diskX * diskX)) + blend * diskY;
        float3 hemisphereTangent = firstTangent * diskX + secondTangent * warpedY +
                                   stretchedTangent * sqrt(max(0, 1 - diskX * diskX - warpedY * warpedY));
        float3 halfTangent = NormalizeOrZero(
            float3(alpha * hemisphereTangent.x, alpha * hemisphereTangent.y, max(0, hemisphereTangent.z)));
        float3 halfWorld = axisXWorld * halfTangent.x + axisYWorld * halfTangent.y + normalWorld * halfTangent.z;
        directionWorld = halfWorld * (2 * dot(viewWorld, halfWorld)) - viewWorld;
    }
    else
    {
        float exponent = (1 - chosen.Roughness) * chosen.Power;
        float cosine = pow(1 - samples.y, 1 / (exponent + 2));
        float ringRadius = sqrt(max(0, 1 - cosine * cosine));
        directionWorld = ringRadius * (cos(angle) * axisXWorld + sin(angle) * axisYWorld) + cosine * normalWorld;
    }
    directionWorld = NormalizeOrZero(directionWorld);
    return dot(normalWorld, directionWorld) > 0 ? directionWorld : float3(0);
}
