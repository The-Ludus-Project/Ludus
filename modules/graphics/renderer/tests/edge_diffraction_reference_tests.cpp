#include "edge_diffraction_reference.h"
#include "edge_diffraction_scene_loading.h"

#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <string_view>

// Reference images and capture comparison for the diffraction test scenes. The visible cases check that a reference
// survives LUHDR01 encoding, passes against itself and that the comparison catches a wrong brightness, camera,
// wavelength or missing diffraction. The hidden cases are the workflow of docs/architecture/scene-source-format.md
// (#diffraction-references): write a scene's reference image, and compare a renderer's capture with it.
namespace
{
using namespace ludus::foundation;
namespace testing = ludus::graphics::renderer::testing;

constexpr float64 DESIGN_WAVELENGTH = 0.005;
/// A small extent for the visible cases.
constexpr uint32 SMALL_WIDTH = 48;
constexpr uint32 SMALL_HEIGHT = 32;
constexpr usize SMALL_LIGHT_NODES = 3;
constexpr uint32 TEST_WORKERS = 2;
/// Defaults of the hidden workflow: the editor's 480-pixel captures at the scenes' aspect, 6 light nodes, 8 workers.
constexpr uint32 DEFAULT_WIDTH = 480;
constexpr uint32 DEFAULT_HEIGHT = 320;
constexpr usize DEFAULT_LIGHT_NODES = 6;
constexpr uint32 DEFAULT_WORKERS = 8;

[[nodiscard]] testing::EdgeDiffractionScene LoadScene(std::string_view path)
{
    testing::EdgeDiffractionScene scene;
    INFO(path);
    REQUIRE(testing::TryLoadDiffractionScene(path, scene) == true);
    return scene;
}

[[nodiscard]] std::string GetTestScenePath(std::string_view name)
{
    return std::string(LUDUS_TEST_SCENES_DIR) + "/diffraction/" + std::string(name);
}

[[nodiscard]] testing::DiffractionCapture RenderSmallReference(const testing::EdgeDiffractionScene& scene,
                                                               testing::DiffractionModel model)
{
    testing::DiffractionSettings settings;
    settings.Model = model;
    settings.WavelengthWorld = DESIGN_WAVELENGTH;
    settings.LightQuadratureNodes = SMALL_LIGHT_NODES;
    testing::DiffractionCapture capture;
    REQUIRE(
        testing::TryRenderDiffractionReference(scene, settings, {SMALL_WIDTH, SMALL_HEIGHT}, TEST_WORKERS, capture) ==
        true);
    return capture;
}

[[nodiscard]] testing::DiffractionCapture CopyCapture(const testing::DiffractionCapture& capture)
{
    Array<uint8> bytes;
    REQUIRE(testing::TryEncodeLuhdr(capture, bytes) == true);
    testing::DiffractionCapture copy;
    REQUIRE(testing::TryDecodeLuhdr(bytes.AsSpan(), copy) == true);
    return copy;
}

/// An environment variable's value, empty when it is unset.
[[nodiscard]] std::string_view GetEnvironment(const char* name)
{
    const char* valueOrNull = std::getenv(name);
    return valueOrNull == nullptr ? std::string_view{} : std::string_view{valueOrNull};
}

[[nodiscard]] float64 ReadEnvironmentNumber(const char* name, float64 fallback)
{
    const std::string_view text = GetEnvironment(name);
    if (text.empty() == true)
    {
        return fallback;
    }
    float32 value = 0;
    INFO(name << " = " << text);
    REQUIRE(ParseFloat32(text.data(), text.size(), value) == NumberParseStatus::Success);
    return static_cast<float64>(value);
}

[[nodiscard]] uint64 ReadEnvironmentCount(const char* name, uint64 fallback)
{
    const std::string_view text = GetEnvironment(name);
    if (text.empty() == true)
    {
        return fallback;
    }
    uint64 value = 0;
    INFO(name << " = " << text);
    REQUIRE(ParseUint64(text.data(), text.size(), value) == NumberParseStatus::Success);
    return value;
}

/// LUDUS_DIFFRACTION_EXTENT=<width>x<height>, or the fallback.
void ReadEnvironmentExtent(testing::DiffractionExtent& inoutExtent)
{
    const std::string_view text = GetEnvironment("LUDUS_DIFFRACTION_EXTENT");
    if (text.empty() == true)
    {
        return;
    }
    const usize separator = text.find('x');
    INFO("LUDUS_DIFFRACTION_EXTENT = " << text);
    REQUIRE(separator != std::string_view::npos);
    uint64 width = 0;
    uint64 height = 0;
    const std::string_view widthText = text.substr(0, separator);
    const std::string_view heightText = text.substr(separator + 1);
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the parser takes the view's size with its data.
    REQUIRE(ParseUint64(widthText.data(), widthText.size(), width) == NumberParseStatus::Success);
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the parser takes the view's size with its data.
    REQUIRE(ParseUint64(heightText.data(), heightText.size(), height) == NumberParseStatus::Success);
    constexpr uint64 MAXIMUM_EXTENT = 4096;
    REQUIRE((width > 0 && height > 0 && width <= MAXIMUM_EXTENT && height <= MAXIMUM_EXTENT));
    inoutExtent = {static_cast<uint32>(width), static_cast<uint32>(height)};
}

struct NamedModel final
{
    std::string_view Name;
    testing::DiffractionModel Model = testing::DiffractionModel::UtdAbsorbing;
};

constexpr usize MODEL_COUNT = 6;
constexpr StaticArray<NamedModel, MODEL_COUNT> MODELS{{
    { .Name = "geometric", .Model = testing::DiffractionModel::Geometric },
    { .Name = "kirchhoff", .Model = testing::DiffractionModel::Kirchhoff },
    { .Name = "utd-absorbing", .Model = testing::DiffractionModel::UtdAbsorbing },
    { .Name = "utd-soft", .Model = testing::DiffractionModel::UtdSoft },
    { .Name = "utd-hard", .Model = testing::DiffractionModel::UtdHard },
    { .Name = "utd-unpolarized", .Model = testing::DiffractionModel::UtdUnpolarized },
}};

/// LUDUS_DIFFRACTION_MODEL, or the fallback.
[[nodiscard]] NamedModel ReadEnvironmentModel(std::string_view fallback)
{
    std::string_view name = GetEnvironment("LUDUS_DIFFRACTION_MODEL");
    if (name.empty() == true)
    {
        name = fallback;
    }
    for (const NamedModel& model : MODELS)
    {
        if (model.Name == name)
        {
            return model;
        }
    }
    INFO("LUDUS_DIFFRACTION_MODEL = "
         << name << "; use geometric, kirchhoff, utd-absorbing, utd-soft, utd-hard or utd-unpolarized");
    FAIL("Unknown diffraction model");
    return {};
}

void WriteOutput(std::string_view directory, const std::string& name, std::span<const uint8> bytes)
{
    INFO(directory << "/" << name);
    REQUIRE(testing::TryWriteDiffractionFile(directory, name, bytes) == true);
}
} // namespace

TEST_CASE("Diffraction reference maps pixels to the wall through the scene's camera", "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene(GetTestScenePath("single-slit.luscene.json"));
    // The camera 0.9 m in front of the wall sees +-0.4 m vertically; at 3:2 it sees +-0.6 m horizontally.
    math::Vector3d topLeft{};
    math::Vector3d bottomRight{};
    REQUIRE(testing::TryFindDiffractionWallPoint(scene, 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT, topLeft) == true);
    REQUIRE(testing::TryFindDiffractionWallPoint(scene,
                                                 DEFAULT_WIDTH - 1,
                                                 DEFAULT_HEIGHT - 1,
                                                 DEFAULT_WIDTH,
                                                 DEFAULT_HEIGHT,
                                                 bottomRight) == true);
    // Pixels are 2.5 mm square on the wall; the first pixel's centre lies half a pixel inside each edge.
    const float64 halfPixel = 0.8 / DEFAULT_HEIGHT / 2;
    CHECK(math::Abs(topLeft.X - (-0.6 + halfPixel)) < 1e-9);
    CHECK(math::Abs(topLeft.Y - (0.4 - halfPixel)) < 1e-9);
    CHECK(math::Abs(bottomRight.X + topLeft.X) < 1e-9);
    CHECK(math::Abs(bottomRight.Y + topLeft.Y) < 1e-9);
    CHECK(topLeft.Z == scene.WallHeightWorld);
}

TEST_CASE("Diffraction reference survives LUHDR01 and passes against itself", "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene(GetTestScenePath("single-slit.luscene.json"));
    const testing::DiffractionCapture reference = RenderSmallReference(scene, testing::DiffractionModel::UtdAbsorbing);
    CHECK(reference.Flags == (testing::LUHDR_CPU_ORACLE_FLAG | testing::LUHDR_CENTER_RAYS_FLAG |
                              testing::LUHDR_REVERSE_Z_FLAG | testing::LUHDR_OPTICAL_FLAG));
    CHECK(reference.OpticalWavelengthWorld == static_cast<float32>(DESIGN_WAVELENGTH));
    const testing::DiffractionCapture copy = CopyCapture(reference);
    REQUIRE(copy.Radiance.GetSize() == reference.Radiance.GetSize());
    for (usize i = 0; i < copy.Radiance.GetSize(); ++i)
    {
        CHECK(copy.Radiance[i] == reference.Radiance[i]);
    }
    CHECK(copy.Flags == reference.Flags);
    CHECK(copy.OpticalWavelengthWorld == reference.OpticalWavelengthWorld);

    testing::DiffractionComparison comparison;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, copy, {}, comparison) == true);
    CHECK(comparison.IsPassing == true);
    CHECK(comparison.WorstColumnRatio == 1);
    CHECK(comparison.RelativeL1 == 0);
    CHECK(comparison.ComparedPixels == SMALL_WIDTH * SMALL_HEIGHT);
    CHECK(comparison.SignificantColumns > 0);

    Array<uint8> profile;
    REQUIRE(testing::TryWriteDiffractionProfile(scene, reference, &copy, profile) == true);
    const std::string_view text{reinterpret_cast<const char*>(profile.GetData()), profile.GetSize()};
    CHECK(text.starts_with("column,wall_x_m,reference_centre_row,reference_column_mean,capture_centre_row,capture_"
                           "column_mean,ratio\n") == true);
    usize lines = 0;
    for (const char character : text)
    {
        lines += character == '\n' ? 1 : 0;
    }
    CHECK(lines == SMALL_WIDTH + 1);
}

TEST_CASE("Diffraction comparison catches wrong brightness, camera, wavelength and missing diffraction",
          "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene(GetTestScenePath("single-slit.luscene.json"));
    const testing::DiffractionCapture reference = RenderSmallReference(scene, testing::DiffractionModel::UtdAbsorbing);

    testing::DiffractionCapture brighter = CopyCapture(reference);
    for (usize i = 0; i < brighter.Radiance.GetSize(); ++i)
    {
        brighter.Radiance[i] *= 1.1F;
    }
    testing::DiffractionComparison comparison;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, brighter, {}, comparison) == true);
    CHECK(comparison.IsPassing == false);
    CHECK(math::Abs(comparison.TotalRatio - 1.1) < 1e-6);

    testing::DiffractionCapture moved = CopyCapture(reference);
    moved.CameraPositionWorld.X += 0.01F;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, moved, {}, comparison) == true);
    CHECK(comparison.HasMatchingCamera == false);
    CHECK(comparison.IsPassing == false);

    // The editor's view of the same camera: conventional depth (z' = w - z) and its own near plane. The same points
    // land on the same pixels, so the camera matches.
    testing::DiffractionCapture conventional = CopyCapture(reference);
    for (math::Vector4& column : conventional.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z * 0.5F;
    }
    REQUIRE(testing::TryCompareDiffractionCapture(reference, conventional, {}, comparison) == true);
    CHECK(comparison.HasMatchingCamera == true);
    CHECK(comparison.IsPassing == true);

    testing::DiffractionCapture otherWavelength = CopyCapture(reference);
    otherWavelength.OpticalWavelengthWorld = 0.004F;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, otherWavelength, {}, comparison) == true);
    CHECK(comparison.HasMatchingWavelength == false);
    CHECK(comparison.IsPassing == false);

    // A renderer without diffraction: the geometric image, labelled with the reference's wavelength. Its flat strip
    // misses the bright centre and the light in the geometric shadow.
    testing::DiffractionCapture geometric = RenderSmallReference(scene, testing::DiffractionModel::Geometric);
    geometric.Flags = reference.Flags;
    geometric.OpticalWavelengthWorld = reference.OpticalWavelengthWorld;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, geometric, {}, comparison) == true);
    INFO("worst column " << comparison.WorstColumn << " ratio " << comparison.WorstColumnRatio);
    CHECK(comparison.IsPassing == false);
    CHECK(math::Abs(comparison.WorstColumnRatio - 1) > 0.5);
}

TEST_CASE("Checked-in diffraction references match the oracle", "[edge-diffraction]")
{
    // tests/scenes/diffraction/references holds the oracle's references, so the editor can compare with them without
    // a terminal. Each must still be what the oracle renders: spot-check pixels across the image against it.
    struct CheckedInReference final
    {
        std::string_view Scene;
        std::string_view Model;
        testing::DiffractionModel Value = testing::DiffractionModel::UtdAbsorbing;
    };
    constexpr usize REFERENCE_COUNT = 4;
    constexpr StaticArray<CheckedInReference, REFERENCE_COUNT> REFERENCES{{
        { .Scene = "single-edge", .Model = "utd-absorbing", .Value = testing::DiffractionModel::UtdAbsorbing },
        { .Scene = "single-slit", .Model = "utd-absorbing", .Value = testing::DiffractionModel::UtdAbsorbing },
        {
            .Scene = "slit-partial-occluder",
            .Model = "utd-absorbing",
            .Value = testing::DiffractionModel::UtdAbsorbing,
        },
        { .Scene = "single-slit", .Model = "geometric", .Value = testing::DiffractionModel::Geometric },
    }};
    constexpr uint32 REFERENCE_WIDTH = 240;
    constexpr uint32 REFERENCE_HEIGHT = 160;
    // Pixels on the centre row across the beam, its fringes and the shadow, and two off the centre row.
    constexpr usize SAMPLE_COUNT = 7;
    constexpr StaticArray<uint32, SAMPLE_COUNT> SAMPLE_COLUMNS{{10, 60, 100, 118, 125, 170, 230}};
    constexpr StaticArray<uint32, SAMPLE_COUNT> SAMPLE_ROWS{{80, 80, 80, 80, 3, 150, 80}};
    for (const CheckedInReference& expected : REFERENCES)
    {
        const std::string path = std::string(LUDUS_TEST_SCENES_DIR) + "/diffraction/references/" +
                                 std::string(expected.Scene) + "-" + std::string(expected.Model) + ".luhdr";
        INFO(path);
        Array<uint8> bytes;
        REQUIRE(testing::TryReadDiffractionFile(path, bytes) == true);
        testing::DiffractionCapture reference;
        REQUIRE(testing::TryDecodeLuhdr(bytes.AsSpan(), reference) == true);
        CHECK(reference.Width == REFERENCE_WIDTH);
        CHECK(reference.Height == REFERENCE_HEIGHT);
        const bool isOptical = expected.Value != testing::DiffractionModel::Geometric;
        CHECK(reference.OpticalWavelengthWorld == (isOptical == true ? static_cast<float32>(DESIGN_WAVELENGTH) : 0.0F));
        const testing::EdgeDiffractionScene scene =
            LoadScene(GetTestScenePath(std::string(expected.Scene) + ".luscene.json"));
        testing::DiffractionSettings settings;
        settings.Model = expected.Value;
        for (usize i = 0; i < SAMPLE_COUNT; ++i)
        {
            math::Vector3d wallWorld{};
            REQUIRE(testing::TryFindDiffractionWallPoint(scene,
                                                         SAMPLE_COLUMNS[i],
                                                         SAMPLE_ROWS[i],
                                                         REFERENCE_WIDTH,
                                                         REFERENCE_HEIGHT,
                                                         wallWorld) == true);
            float64 radiance = 0;
            REQUIRE(testing::TryComputeDiffractionWallRadiance(scene, settings, wallWorld, radiance) == true);
            const float32 stored =
                reference.Radiance[(static_cast<usize>(SAMPLE_ROWS[i]) * REFERENCE_WIDTH + SAMPLE_COLUMNS[i]) * 3];
            INFO("column " << SAMPLE_COLUMNS[i] << " row " << SAMPLE_ROWS[i] << ": file " << stored << ", oracle "
                           << radiance);
            CHECK(stored == static_cast<float32>(radiance));
        }
    }
}

TEST_CASE("Write a diffraction scene's reference image", "[.diffraction-reference]")
{
    // LUDUS_DIFFRACTION_SCENE=<scene .luscene.json> LUDUS_DIFFRACTION_OUTPUT=<existing directory>, optionally
    // LUDUS_DIFFRACTION_MODEL (utd-absorbing), LUDUS_DIFFRACTION_WAVELENGTH (0.005 m), LUDUS_DIFFRACTION_EXTENT
    // (480x320), LUDUS_DIFFRACTION_LIGHT_NODES (6) and LUDUS_DIFFRACTION_WORKERS (8). Writes
    // <scene>-<model>.luhdr and <scene>-<model>-profile.csv.
    const std::string_view scenePath = GetEnvironment("LUDUS_DIFFRACTION_SCENE");
    const std::string_view output = GetEnvironment("LUDUS_DIFFRACTION_OUTPUT");
    if (scenePath.empty() == true || output.empty() == true)
    {
        SKIP("Set LUDUS_DIFFRACTION_SCENE to a diffraction scene file and LUDUS_DIFFRACTION_OUTPUT to a directory.");
    }
    const testing::EdgeDiffractionScene scene = LoadScene(scenePath);
    const NamedModel model = ReadEnvironmentModel("utd-absorbing");
    testing::DiffractionSettings settings;
    settings.Model = model.Model;
    settings.WavelengthWorld = ReadEnvironmentNumber("LUDUS_DIFFRACTION_WAVELENGTH", DESIGN_WAVELENGTH);
    settings.LightQuadratureNodes =
        static_cast<usize>(ReadEnvironmentCount("LUDUS_DIFFRACTION_LIGHT_NODES", DEFAULT_LIGHT_NODES));
    testing::DiffractionExtent extent{DEFAULT_WIDTH, DEFAULT_HEIGHT};
    ReadEnvironmentExtent(extent);
    const uint32 workers = static_cast<uint32>(ReadEnvironmentCount("LUDUS_DIFFRACTION_WORKERS", DEFAULT_WORKERS));
    testing::DiffractionCapture reference;
    INFO("model " << model.Name << " does not apply to the scene, or the settings are invalid");
    REQUIRE(testing::TryRenderDiffractionReference(scene, settings, extent, workers, reference) == true);
    Array<uint8> bytes;
    REQUIRE(testing::TryEncodeLuhdr(reference, bytes) == true);
    // <scene>-<model>, so that a reference written into tests/scenes/diffraction/references keeps its scene's name.
    const std::string_view sceneName = scenePath.substr(scenePath.rfind('/') + 1);
    constexpr std::string_view SCENE_EXTENSION = ".luscene.json";
    const std::string_view sceneStem = sceneName.ends_with(SCENE_EXTENSION) == true
                                           ? sceneName.substr(0, sceneName.size() - SCENE_EXTENSION.size())
                                           : sceneName;
    const std::string stem = std::string(sceneStem) + "-" + std::string(model.Name);
    WriteOutput(output, stem + ".luhdr", bytes.AsSpan());
    Array<uint8> profile;
    REQUIRE(testing::TryWriteDiffractionProfile(scene, reference, nullptr, profile) == true);
    WriteOutput(output, stem + "-profile.csv", profile.AsSpan());
}

TEST_CASE("Compare a capture with a diffraction scene's reference", "[.diffraction-compare]")
{
    // LUDUS_DIFFRACTION_SCENE=<scene .luscene.json> LUDUS_DIFFRACTION_CAPTURE=<capture .luhdr>, optionally
    // LUDUS_DIFFRACTION_MODEL (utd-absorbing for an optical capture, geometric otherwise), LUDUS_DIFFRACTION_WAVELENGTH
    // (the capture's), LUDUS_DIFFRACTION_TOLERANCE (0.05), LUDUS_DIFFRACTION_LIGHT_NODES (6),
    // LUDUS_DIFFRACTION_WORKERS (8), LUDUS_DIFFRACTION_REFERENCE (a reference .luhdr to reuse) and
    // LUDUS_DIFFRACTION_OUTPUT (a directory for the reference and the column profile). Renders the reference at the
    // capture's extent unless one is given, and passes when every significant column agrees.
    const std::string_view scenePath = GetEnvironment("LUDUS_DIFFRACTION_SCENE");
    const std::string_view capturePath = GetEnvironment("LUDUS_DIFFRACTION_CAPTURE");
    if (scenePath.empty() == true || capturePath.empty() == true)
    {
        SKIP("Set LUDUS_DIFFRACTION_SCENE to a diffraction scene file and LUDUS_DIFFRACTION_CAPTURE to a capture.");
    }
    const testing::EdgeDiffractionScene scene = LoadScene(scenePath);
    Array<uint8> captureBytes;
    INFO(capturePath);
    REQUIRE(testing::TryReadDiffractionFile(capturePath, captureBytes) == true);
    testing::DiffractionCapture capture;
    REQUIRE(testing::TryDecodeLuhdr(captureBytes.AsSpan(), capture) == true);

    const bool isOptical = (capture.Flags & testing::LUHDR_OPTICAL_FLAG) != 0;
    const NamedModel model = ReadEnvironmentModel(isOptical == true ? "utd-absorbing" : "geometric");
    testing::DiffractionSettings settings;
    settings.Model = model.Model;
    settings.WavelengthWorld = ReadEnvironmentNumber(
        "LUDUS_DIFFRACTION_WAVELENGTH",
        isOptical == true ? static_cast<float64>(capture.OpticalWavelengthWorld) : DESIGN_WAVELENGTH);
    settings.LightQuadratureNodes =
        static_cast<usize>(ReadEnvironmentCount("LUDUS_DIFFRACTION_LIGHT_NODES", DEFAULT_LIGHT_NODES));
    const uint32 workers = static_cast<uint32>(ReadEnvironmentCount("LUDUS_DIFFRACTION_WORKERS", DEFAULT_WORKERS));
    // LUDUS_DIFFRACTION_REFERENCE=<reference .luhdr> reuses a reference written before instead of rendering it.
    testing::DiffractionCapture reference;
    const std::string_view referencePath = GetEnvironment("LUDUS_DIFFRACTION_REFERENCE");
    if (referencePath.empty() == false)
    {
        Array<uint8> referenceBytes;
        INFO(referencePath);
        REQUIRE(testing::TryReadDiffractionFile(referencePath, referenceBytes) == true);
        REQUIRE(testing::TryDecodeLuhdr(referenceBytes.AsSpan(), reference) == true);
    }
    else
    {
        REQUIRE(testing::TryRenderDiffractionReference(scene,
                                                       settings,
                                                       {capture.Width, capture.Height},
                                                       workers,
                                                       reference) == true);
    }
    testing::DiffractionTolerance tolerance;
    tolerance.ColumnRatio = ReadEnvironmentNumber("LUDUS_DIFFRACTION_TOLERANCE", tolerance.ColumnRatio);
    testing::DiffractionComparison comparison;
    REQUIRE(testing::TryCompareDiffractionCapture(reference, capture, tolerance, comparison) == true);

    const std::string_view output = GetEnvironment("LUDUS_DIFFRACTION_OUTPUT");
    if (output.empty() == false)
    {
        if (referencePath.empty() == true)
        {
            Array<uint8> bytes;
            REQUIRE(testing::TryEncodeLuhdr(reference, bytes) == true);
            WriteOutput(output, "reference-" + std::string(model.Name) + ".luhdr", bytes.AsSpan());
        }
        Array<uint8> profile;
        REQUIRE(testing::TryWriteDiffractionProfile(scene, reference, &capture, profile) == true);
        WriteOutput(output, "comparison-profile.csv", profile.AsSpan());
    }
    WARN("model " << model.Name << " at " << settings.WavelengthWorld << " m; camera "
                  << (comparison.HasMatchingCamera == true ? "matches" : "DIFFERS") << "; wavelength "
                  << (comparison.HasMatchingWavelength == true ? "matches" : "DIFFERS") << "; worst column "
                  << comparison.WorstColumn << " capture/reference " << comparison.WorstColumnRatio << " (tolerance +-"
                  << tolerance.ColumnRatio << " over " << comparison.SignificantColumns
                  << " significant columns); total ratio " << comparison.TotalRatio << "; per-pixel relative L1 "
                  << comparison.RelativeL1);
    CHECK(comparison.IsPassing == true);
}
