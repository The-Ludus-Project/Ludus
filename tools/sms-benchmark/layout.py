"""Single owner of research CPU/GPU buffer layout and sampler policy constants."""
import struct

REFLECTOR_CENTER_RECORD = 14
REFLECTOR_DERIVATIVE_U_RECORD = 15
REFLECTOR_DERIVATIVE_V_RECORD = 16
LIGHT_CENTER_RECORD = 17
LIGHT_DERIVATIVE_U_RECORD = 18
LIGHT_DERIVATIVE_V_RECORD = 19
LIGHT_NORMAL_AREA_RECORD = 20
FLOOR_NORMAL_RECORD = 21
AREA_RADIANCE_RECORD = 22
SMOOTH_SLOPE_RECORD = 23
SMOOTH_COVARIANCE_RECORD = 24
SOLVER_ITERATIONS = 32
SOLVER_TOLERANCE = 2e-06
MINIMUM_MAP_DETERMINANT = 1e-08
REHIT_DISTANCE_UV = 0.0001
MAXIMUM_BERNOULLI_TRIALS = 16384
DEFENSIVE_FRACTION = 0.1
MINIMUM_TRIANGLE_DETERMINANT = 1e-10
ACCELERATION_GRID_SIDE = 16
ACCELERATION_RECORD_COUNT = 2
ACCELERATION_DISABLED = 4294967295
INPUT_RECORD_COUNT = 5
OUTPUT_RECORD_COUNT = 3
LIGHT_INPUT_RECORD = 2
RAY_ORIGIN_INPUT_RECORD = 3
RAY_DIRECTION_INPUT_RECORD = 4
BASE_TANGENT_RECORD = 2
BASE_BITANGENT_RECORD = 3
BASE_NORMAL_RECORD = 4
INVERSE_TRANSFORM_RECORD = 5
INVERSE_TRANSFORM_STRIDE = 3
RECTANGLE_COUNT = 3
PARITY_GROUP_SIZE = 32
SCENE_RECORD_COUNT = 25
RECORD_COMPONENT_COUNT = 4

IOR_ETA_RECORD = 0
IOR_EXTINCTION_RECORD = 1
GPU_SCALAR_BYTES = 4
METHOD_CONNECTION_MIS = 0
METHOD_SLICE = 1
METHOD_SMS = 2
METHOD_REFERENCE = 3

PARAMETER_SCHEMA = (
    ("PixelCount", "uint"), ("PixelOffset", "uint"), ("Batch", "uint"), ("Seed", "uint"),
    ("Method", "uint"), ("Resolution", "uint"), ("Alpha", "float"), ("MinimumGuideAlpha", "float"),
)
PARAMETER_CODEC = struct.Struct("<"+"".join("I" if kind == "uint" else "f" for _, kind in PARAMETER_SCHEMA))
RENDER_PARAMETER_BYTES = PARAMETER_CODEC.size
QMC_PARAMETER_SCHEMA = (
    ("PixelOffset", "uint"), ("PixelCount", "uint"), ("SampleCount", "uint"), ("SamplesPerBlock", "uint"),
    ("Seed", "uint"), ("BlocksPerPixel", "uint"), ("Padding0", "uint"), ("Padding1", "uint"),
    ("Alpha", "float"), ("Padding2", "float"), ("Padding3", "float"), ("Padding4", "float"),
)
QMC_PARAMETER_CODEC = struct.Struct("<"+"".join("I" if kind == "uint" else "f" for _, kind in QMC_PARAMETER_SCHEMA))
MAXIMUM_QMC_SAMPLES = 2**22

def write_shader_layout(directory):
    """Emit matching constants and a uniform schema beside copied shader sources."""
    lines = ["#pragma once"]
    lines.append(f"static const uint IOR_ETA_RECORD = {IOR_ETA_RECORD}u;")
    lines.append(f"static const uint IOR_EXTINCTION_RECORD = {IOR_EXTINCTION_RECORD}u;")
    lines.append(f"static const uint GPU_SCALAR_BYTES = {GPU_SCALAR_BYTES}u;")
    lines.append(f"static const uint METHOD_CONNECTION_MIS = {METHOD_CONNECTION_MIS}u;")
    lines.append(f"static const uint METHOD_SLICE = {METHOD_SLICE}u;")
    lines.append(f"static const uint METHOD_SMS = {METHOD_SMS}u;")
    lines.append(f"static const uint METHOD_REFERENCE = {METHOD_REFERENCE}u;")
    lines.append(f"static const uint REFLECTOR_CENTER_RECORD = {REFLECTOR_CENTER_RECORD}u;")
    lines.append(f"static const uint REFLECTOR_DERIVATIVE_U_RECORD = {REFLECTOR_DERIVATIVE_U_RECORD}u;")
    lines.append(f"static const uint REFLECTOR_DERIVATIVE_V_RECORD = {REFLECTOR_DERIVATIVE_V_RECORD}u;")
    lines.append(f"static const uint LIGHT_CENTER_RECORD = {LIGHT_CENTER_RECORD}u;")
    lines.append(f"static const uint LIGHT_DERIVATIVE_U_RECORD = {LIGHT_DERIVATIVE_U_RECORD}u;")
    lines.append(f"static const uint LIGHT_DERIVATIVE_V_RECORD = {LIGHT_DERIVATIVE_V_RECORD}u;")
    lines.append(f"static const uint LIGHT_NORMAL_AREA_RECORD = {LIGHT_NORMAL_AREA_RECORD}u;")
    lines.append(f"static const uint FLOOR_NORMAL_RECORD = {FLOOR_NORMAL_RECORD}u;")
    lines.append(f"static const uint AREA_RADIANCE_RECORD = {AREA_RADIANCE_RECORD}u;")
    lines.append(f"static const uint SMOOTH_SLOPE_RECORD = {SMOOTH_SLOPE_RECORD}u;")
    lines.append(f"static const uint SMOOTH_COVARIANCE_RECORD = {SMOOTH_COVARIANCE_RECORD}u;")
    lines.append(f"static const uint SOLVER_ITERATIONS = {SOLVER_ITERATIONS}u;")
    lines.append(f"static const float SOLVER_TOLERANCE = {SOLVER_TOLERANCE};")
    lines.append(f"static const float MINIMUM_MAP_DETERMINANT = {MINIMUM_MAP_DETERMINANT};")
    lines.append(f"static const float REHIT_DISTANCE_UV = {REHIT_DISTANCE_UV};")
    lines.append(f"static const uint MAXIMUM_BERNOULLI_TRIALS = {MAXIMUM_BERNOULLI_TRIALS}u;")
    lines.append(f"static const float DEFENSIVE_FRACTION = {DEFENSIVE_FRACTION};")
    lines.append(f"static const float MINIMUM_TRIANGLE_DETERMINANT = {MINIMUM_TRIANGLE_DETERMINANT};")
    lines.append(f"static const uint ACCELERATION_GRID_SIDE = {ACCELERATION_GRID_SIDE}u;")
    lines.append(f"static const uint ACCELERATION_RECORD_COUNT = {ACCELERATION_RECORD_COUNT}u;")
    lines.append(f"static const uint ACCELERATION_DISABLED = {ACCELERATION_DISABLED}u;")
    lines.append(f"static const uint INPUT_RECORD_COUNT = {INPUT_RECORD_COUNT}u;")
    lines.append(f"static const uint OUTPUT_RECORD_COUNT = {OUTPUT_RECORD_COUNT}u;")
    lines.append(f"static const uint LIGHT_INPUT_RECORD = {LIGHT_INPUT_RECORD}u;")
    lines.append(f"static const uint RAY_ORIGIN_INPUT_RECORD = {RAY_ORIGIN_INPUT_RECORD}u;")
    lines.append(f"static const uint RAY_DIRECTION_INPUT_RECORD = {RAY_DIRECTION_INPUT_RECORD}u;")
    lines.append(f"static const uint BASE_TANGENT_RECORD = {BASE_TANGENT_RECORD}u;")
    lines.append(f"static const uint BASE_BITANGENT_RECORD = {BASE_BITANGENT_RECORD}u;")
    lines.append(f"static const uint BASE_NORMAL_RECORD = {BASE_NORMAL_RECORD}u;")
    lines.append(f"static const uint INVERSE_TRANSFORM_RECORD = {INVERSE_TRANSFORM_RECORD}u;")
    lines.append(f"static const uint INVERSE_TRANSFORM_STRIDE = {INVERSE_TRANSFORM_STRIDE}u;")
    lines.append(f"static const uint RECTANGLE_COUNT = {RECTANGLE_COUNT}u;")
    lines.append(f"static const uint PARITY_GROUP_SIZE = {PARITY_GROUP_SIZE}u;")
    lines.append(f"static const uint SCENE_RECORD_COUNT = {SCENE_RECORD_COUNT}u;")
    lines.append(f"static const uint RECORD_COMPONENT_COUNT = {RECORD_COMPONENT_COUNT}u;")
    lines.append("struct RenderParameters\n{")
    lines.extend(f"    {kind} {name};" for name, kind in PARAMETER_SCHEMA)
    lines.append("};")
    lines.append("struct QmcParameters\n{")
    lines.extend(f"    {kind} {name};" for name, kind in QMC_PARAMETER_SCHEMA)
    lines.append("};")
    (directory / "layout.slang").write_text("\n".join(lines)+"\n")
