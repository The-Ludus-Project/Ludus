// ludus_ltc_fit: fits the renderer's linearly transformed cosine table. It
// writes ltc_table_layout.h into a layout directory, normally
// modules/graphics/renderer/shaders, and ltc_table.bin and
// ltc_table_provenance.json into a data directory, normally
// modules/graphics/renderer/data. See docs/development/ltc-table.md.

#include "ltc_fit.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <bit>
#include <cstdio>
#include <span>
#include <string_view>

namespace
{
using namespace ludus::tools::ltc_fit;
namespace math = ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::core::float32;
using ludus::foundation::core::uint8;
using ludus::foundation::core::usize;

// float32 needs nine significant decimal digits to round-trip.
constexpr uint32 FLOAT32_SIGNIFICANT_DIGITS = 9;
// Diagnostics are recorded to four significant digits.
constexpr uint32 DIAGNOSTIC_SIGNIFICANT_DIGITS = 4;
constexpr float64 DECIMAL_BASE = 10.0;
constexpr uint32 DECIMAL_DIGIT_BASE = 10;
// Version of the generated table's layout, independent of the fitter version.
constexpr uint32 TABLE_LAYOUT_VERSION = 2;
// Each table entry is four little-endian float32 values.
constexpr usize ENTRY_FLOATS = 4;
// Percentiles summarized in the provenance file.
constexpr float64 MEDIAN = 0.5;
constexpr float64 PERCENTILE_95 = 0.95;
// A fast, low-resolution configuration for checking the tool itself.
constexpr uint32 QUICK_TABLE_SIZE = 8;
constexpr uint32 QUICK_FRESNEL_COUNT = 3;
constexpr uint32 QUICK_OBJECTIVE_STRATA = 16;
constexpr uint32 QUICK_MOMENT_STRATA = 64;
constexpr uint32 QUICK_RESIDUAL_STRATA = 32;
constexpr uint32 QUICK_MAX_ITERATIONS = 400;
// The job system admits at most 64 workers; the default leaves the machine responsive.
constexpr uint32 MAX_WORKER_COUNT = 64;
constexpr uint32 DEFAULT_WORKER_COUNT = 8;
constexpr std::string_view LAYOUT_FILE = "ltc_table_layout.h";
constexpr std::string_view TABLE_FILE = "ltc_table.bin";
constexpr std::string_view PROVENANCE_FILE = "ltc_table_provenance.json";

/// Text accumulated for one output file; IsOk turns false on allocation failure.
struct TextBuilder final
{
    Array<uint8> Bytes;
    bool IsOk = true;

    void Append(std::string_view text) noexcept
    {
        for (const char character : text)
        {
            IsOk = IsOk == true && Bytes.TryAdd(static_cast<uint8>(character)) == true;
        }
    }
    void AppendUnsigned(uint64 value) noexcept
    {
        char digits[DECIMAL_DIGIT_BASE * 2]{};
        usize count = 0;
        do
        {
            digits[count] = static_cast<char>('0' + value % DECIMAL_DIGIT_BASE);
            ++count;
            value /= DECIMAL_DIGIT_BASE;
        } while (value != 0);
        while (count > 0)
        {
            --count;
            Append(std::string_view(&digits[count], 1));
        }
    }
    /// Scientific notation with significantDigits digits, for example -1.25e-3.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): value then digit count, like printf's precision.
    void AppendScientific(float64 value, uint32 significantDigits, bool isFloat32) noexcept
    {
        if (value == 0.0)
        {
            Append("0.0");
            return;
        }
        if (value < 0.0)
        {
            Append("-");
            value = -value;
        }
        ludus::foundation::core::int32 exponent =
            static_cast<ludus::foundation::core::int32>(math::Floor(math::Log(value) / math::Log(DECIMAL_BASE)));
        const float64 digitScale = math::Pow(DECIMAL_BASE, static_cast<float64>(significantDigits - 1));
        const float64 limit = digitScale * DECIMAL_BASE;
        uint64 digits = static_cast<uint64>(
            math::Floor(value / math::Pow(DECIMAL_BASE, static_cast<float64>(exponent)) * digitScale + 0.5));
        while (static_cast<float64>(digits) >= limit)
        {
            digits = (digits + DECIMAL_DIGIT_BASE / 2) / DECIMAL_DIGIT_BASE;
            ++exponent;
        }
        while (static_cast<float64>(digits) < digitScale)
        {
            digits = static_cast<uint64>(
                math::Floor(value / math::Pow(DECIMAL_BASE, static_cast<float64>(exponent - 1)) * digitScale + 0.5));
            --exponent;
        }
        if (isFloat32 == true)
        {
            // Nine digits identify a float32; correct a last digit that double rounding moved off it.
            const float32 expected = static_cast<float32>(value);
            const float64 unit = math::Pow(DECIMAL_BASE, static_cast<float64>(exponent) - (significantDigits - 1));
            for (const uint64 candidate : {digits, digits - 1, digits + 1})
            {
                if (static_cast<float32>(static_cast<float64>(candidate) * unit) == expected)
                {
                    digits = candidate;
                    break;
                }
            }
        }
        char text[DECIMAL_DIGIT_BASE * 2]{};
        for (uint32 i = significantDigits; i-- > 0;)
        {
            text[i] = static_cast<char>('0' + digits % DECIMAL_DIGIT_BASE);
            digits /= DECIMAL_DIGIT_BASE;
        }
        Append(std::string_view(text, 1));
        Append(".");
        Append(significantDigits > 1 ? std::string_view(text + 1, significantDigits - 1) : std::string_view("0"));
        Append(exponent < 0 ? "e-" : "e+");
        AppendUnsigned(static_cast<uint64>(exponent < 0 ? -exponent : exponent));
        if (isFloat32 == true)
        {
            Append("f");
        }
    }
    void AppendFloat32(float64 value) noexcept
    {
        AppendScientific(static_cast<float64>(static_cast<float32>(value)), FLOAT32_SIGNIFICANT_DIGITS, true);
    }
    void AppendDiagnostic(float64 value) noexcept
    {
        AppendScientific(value, DIAGNOSTIC_SIGNIFICANT_DIGITS, false);
    }
};

void WriteProgress(std::string_view text) noexcept
{
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), stderr));
}

float64 GetPercentile(Array<float64>& inoutValues, float64 fraction) noexcept
{
    ludus::foundation::Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float64 a, float64 b) noexcept {
        return a < b;
    });
    return inoutValues[static_cast<usize>(fraction * static_cast<float64>(inoutValues.GetSize() - 1))];
}

void AppendSettings(TextBuilder& inoutText, const FitSettings& settings) noexcept
{
    inoutText.Append("//   fitter version ");
    inoutText.AppendUnsigned(LTC_FITTER_VERSION);
    inoutText.Append(", table ");
    inoutText.AppendUnsigned(settings.RoughnessCount);
    inoutText.Append(" x ");
    inoutText.AppendUnsigned(settings.ViewCount);
    inoutText.Append(" x ");
    inoutText.AppendUnsigned(settings.FresnelCount);
    inoutText.Append(", minimum roughness ");
    inoutText.AppendDiagnostic(settings.MinimumRoughness);
    inoutText.Append(", minimum view cosine ");
    inoutText.AppendDiagnostic(settings.MinimumViewCosine);
    inoutText.Append(",\n//   strata per axis: objective ");
    inoutText.AppendUnsigned(settings.ObjectiveStrata);
    inoutText.Append(", moments ");
    inoutText.AppendUnsigned(settings.MomentStrata);
    inoutText.Append(", residual ");
    inoutText.AppendUnsigned(settings.ResidualStrata);
    inoutText.Append(";\n//   Nelder-Mead ");
    inoutText.AppendUnsigned(settings.StartCount);
    inoutText.Append(" starts of at most ");
    inoutText.AppendUnsigned(settings.MaxIterations);
    inoutText.Append(" iterations; seed ");
    inoutText.AppendUnsigned(settings.Seed);
    inoutText.Append(".\n");
}

void BuildLayout(const FitSettings& settings, TextBuilder& outText) noexcept
{
    outText.Append(R"(#pragma once

// Layout of the linearly transformed cosine (LTC) table for the renderer's specular BSDF: GGX with alpha =
// roughness^2, the height-correlated Smith visibility and Schlick's Fresnel (reference_bsdf.cpp,
// renderer_gpu_preview.slang). Shared by the C++ lookup, the shader lookup and the GPU buffer that holds the table.
// GENERATED by tools/ltc-fit (ludus_ltc_fit) with data/ltc_table.bin; do not edit. docs/development/ltc-table.md
// describes the fit, how to regenerate both files and the table's accuracy; data/ltc_table_provenance.json records
// the residual error of every transform.
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, "Real-Time Polygonal-Light Shading with
// Linearly Transformed Cosines", ACM Transactions on Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016), section 4,
// https://doi.org/10.1145/2897824.2925895, for the fit, and to Christoph Peters, "BRDF Importance Sampling for
// Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, sections 4.1-4.2,
// https://doi.org/10.1145/3450626.3459672, for the F0 axis and for tabulating the specular albedo beside the
// transform.
// Fit parameters:
)");
    AppendSettings(outText, settings);
    outText.Append(R"(//
// The table is LUDUS_LTC_TABLE_ENTRY_COUNT entries of four float32 values. Transform (f, r, t), at entry
// (f * LUDUS_LTC_TABLE_ROUGHNESS_COUNT + r) * LUDUS_LTC_TABLE_VIEW_COUNT + t, covers Schlick F0 f / (F0 count - 1),
// perceptual roughness LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS + r * (1 - minimum) / (rows - 1) and view inclination
// theta = t * (pi / 2) / (columns - 1). It holds (M02, M11, M20, M22) of the inverse transform
// M^-1 = [[1, 0, M02], [0, M11, 0], [M20, 0, M22]] up to a positive scale, in the frame whose x-z plane contains the
// view direction (sin theta, 0, cos theta). Moment (r, t), at entry LUDUS_LTC_TABLE_MOMENT_OFFSET + r * columns + t,
// holds (norm, Fresnel norm, 0, 0): the specular albedo with Schlick's Fresnel is
// F0 * (norm - Fresnel norm) + Fresnel norm for every F0.

)");
    outText.Append("#define LUDUS_LTC_TABLE_VERSION ");
    outText.AppendUnsigned(TABLE_LAYOUT_VERSION);
    outText.Append("\n#define LUDUS_LTC_TABLE_FITTER_VERSION ");
    outText.AppendUnsigned(LTC_FITTER_VERSION);
    outText.Append("\n#define LUDUS_LTC_TABLE_ROUGHNESS_COUNT ");
    outText.AppendUnsigned(settings.RoughnessCount);
    outText.Append("\n#define LUDUS_LTC_TABLE_VIEW_COUNT ");
    outText.AppendUnsigned(settings.ViewCount);
    outText.Append("\n#define LUDUS_LTC_TABLE_FRESNEL_COUNT ");
    outText.AppendUnsigned(settings.FresnelCount);
    outText.Append(
        "\n#define LUDUS_LTC_TABLE_SLICE_ENTRY_COUNT (LUDUS_LTC_TABLE_ROUGHNESS_COUNT * LUDUS_LTC_TABLE_VIEW_COUNT)\n"
        "#define LUDUS_LTC_TABLE_MOMENT_OFFSET (LUDUS_LTC_TABLE_FRESNEL_COUNT * LUDUS_LTC_TABLE_SLICE_ENTRY_COUNT)\n"
        "#define LUDUS_LTC_TABLE_ENTRY_COUNT (LUDUS_LTC_TABLE_MOMENT_OFFSET + LUDUS_LTC_TABLE_SLICE_ENTRY_COUNT)\n"
        "#define LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS ");
    outText.AppendFloat32(settings.MinimumRoughness);
    outText.Append("\n");
}

void AppendFloat32Bytes(Array<uint8>& inoutBytes, bool& inoutIsOk, float64 value) noexcept
{
    ludus::foundation::StaticArray<uint8, sizeof(float32)> bytes{};
    const uint32 bits = std::bit_cast<uint32>(static_cast<float32>(value));
    inoutIsOk = inoutIsOk == true &&
                ludus::foundation::core::TryWriteLittleEndian(bits, std::span<uint8>(bytes.GetData(), bytes.GetSize()));
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        inoutIsOk = inoutIsOk == true && inoutBytes.TryAdd(bytes[i]) == true;
    }
}

/// Every transform, then every moment, as little-endian float32 (see BuildLayout).
bool BuildData(const FitSettings& settings, const Array<FittedCell>& cells, Array<uint8>& outBytes) noexcept
{
    const usize sliceSize = usize{settings.RoughnessCount} * settings.ViewCount;
    bool isOk = outBytes.TryEnsureCapacity((cells.GetSize() + sliceSize) * ENTRY_FLOATS * sizeof(float32));
    for (usize i = 0; i < cells.GetSize(); ++i)
    {
        const StoredCell stored = MakeStoredCell(cells[i]);
        AppendFloat32Bytes(outBytes, isOk, stored.M02);
        AppendFloat32Bytes(outBytes, isOk, stored.M11);
        AppendFloat32Bytes(outBytes, isOk, stored.M20);
        AppendFloat32Bytes(outBytes, isOk, stored.M22);
    }
    // The moments do not depend on F0; every slice computes them from the same samples.
    for (usize i = 0; i < sliceSize; ++i)
    {
        const StoredCell stored = MakeStoredCell(cells[i]);
        AppendFloat32Bytes(outBytes, isOk, stored.Norm);
        AppendFloat32Bytes(outBytes, isOk, stored.FresnelNorm);
        AppendFloat32Bytes(outBytes, isOk, 0.0);
        AppendFloat32Bytes(outBytes, isOk, 0.0);
    }
    return isOk;
}

void AppendSummary(TextBuilder& inoutText, Array<float64>& inoutValues) noexcept
{
    inoutText.Append("{\"median\": ");
    inoutText.AppendDiagnostic(GetPercentile(inoutValues, MEDIAN));
    inoutText.Append(", \"p95\": ");
    inoutText.AppendDiagnostic(GetPercentile(inoutValues, PERCENTILE_95));
    inoutText.Append(", \"max\": ");
    inoutText.AppendDiagnostic(GetPercentile(inoutValues, 1.0));
    inoutText.Append("}");
}

bool BuildProvenance(const FitSettings& settings, const Array<FittedCell>& cells, TextBuilder& outText) noexcept
{
    Array<float64> all;
    Array<float64> minimumRow;
    Array<float64> unitFresnel;
    const usize sliceSize = usize{settings.RoughnessCount} * settings.ViewCount;
    for (usize i = 0; i < cells.GetSize(); ++i)
    {
        if (all.TryAdd(cells[i].RelativeL1Error) == false ||
            (i % sliceSize < settings.ViewCount && minimumRow.TryAdd(cells[i].RelativeL1Error) == false) ||
            (i / sliceSize + 1 == settings.FresnelCount && unitFresnel.TryAdd(cells[i].RelativeL1Error) == false))
        {
            return false;
        }
    }
    outText.Append("{\n  \"generator\": \"tools/ltc-fit (ludus_ltc_fit)\",\n  \"fitterVersion\": ");
    outText.AppendUnsigned(LTC_FITTER_VERSION);
    outText.Append(",\n  \"tableVersion\": ");
    outText.AppendUnsigned(TABLE_LAYOUT_VERSION);
    outText.Append(",\n  \"brdf\": \"Ludus specular lobe: GGX, alpha = roughness^2, height-correlated Smith, Schlick "
                   "Fresnel with the slice's F0\",\n  \"objective\": \"L3 error of the normalized "
                   "cosine-weighted BRDF (Heitz et al. 2016, section 4)\",\n  \"residual\": \"relative L1 error: "
                   "integral of |LTC - rho F cos / norm| on independent samples, in [0, 2]\",\n  \"settings\": {"
                   "\"roughnessCount\": ");
    outText.AppendUnsigned(settings.RoughnessCount);
    outText.Append(", \"viewCount\": ");
    outText.AppendUnsigned(settings.ViewCount);
    outText.Append(", \"fresnelCount\": ");
    outText.AppendUnsigned(settings.FresnelCount);
    outText.Append(", \"minimumRoughness\": ");
    outText.AppendDiagnostic(settings.MinimumRoughness);
    outText.Append(", \"minimumViewCosine\": ");
    outText.AppendDiagnostic(settings.MinimumViewCosine);
    outText.Append(", \"objectiveStrata\": ");
    outText.AppendUnsigned(settings.ObjectiveStrata);
    outText.Append(", \"momentStrata\": ");
    outText.AppendUnsigned(settings.MomentStrata);
    outText.Append(", \"residualStrata\": ");
    outText.AppendUnsigned(settings.ResidualStrata);
    outText.Append(", \"maxIterations\": ");
    outText.AppendUnsigned(settings.MaxIterations);
    outText.Append(", \"startCount\": ");
    outText.AppendUnsigned(settings.StartCount);
    outText.Append(", \"seed\": ");
    outText.AppendUnsigned(settings.Seed);
    outText.Append("},\n  \"relativeL1ErrorSummary\": {\"all\": ");
    AppendSummary(outText, all);
    outText.Append(", \"minimumRoughness\": ");
    AppendSummary(outText, minimumRow);
    outText.Append(", \"unitFresnel\": ");
    AppendSummary(outText, unitFresnel);
    outText.Append("},\n  \"relativeL1Error\": [\n");
    for (uint32 fresnel = 0; fresnel < settings.FresnelCount; ++fresnel)
    {
        outText.Append("    [\n");
        for (uint32 row = 0; row < settings.RoughnessCount; ++row)
        {
            outText.Append("      [");
            for (uint32 column = 0; column < settings.ViewCount; ++column)
            {
                outText.Append(column == 0 ? "" : ", ");
                outText.AppendDiagnostic(
                    cells[sliceSize * fresnel + usize{row} * settings.ViewCount + column].RelativeL1Error);
            }
            outText.Append(row + 1 == settings.RoughnessCount ? "]\n" : "],\n");
        }
        outText.Append(fresnel + 1 == settings.FresnelCount ? "    ]\n" : "    ],\n");
    }
    outText.Append("  ]\n}\n");
    return true;
}

bool Publish(const ludus::foundation::filesystem::WriteDirectory& directory,
             std::string_view file,
             const Array<uint8>& bytes) noexcept
{
    const ludus::foundation::filesystem::PublicationResult result =
        directory.Publish(file, std::span<const uint8>(bytes.GetData(), bytes.GetSize()));
    return result.Published == true;
}

/// A decimal worker count of at most MAX_WORKER_COUNT.
bool TryParseWorkerCount(std::string_view text, uint32& outCount) noexcept
{
    uint32 value = 0;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        value = value * DECIMAL_DIGIT_BASE + static_cast<uint32>(character - '0');
        if (value > MAX_WORKER_COUNT)
        {
            return false;
        }
    }
    outCount = value;
    return text.empty() == false;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    constexpr int FIRST_OPTION = 3;
    FitSettings settings;
    uint32 workerCount = DEFAULT_WORKER_COUNT;
    bool areArgumentsValid = argumentCount >= FIRST_OPTION;
    for (int i = FIRST_OPTION; i < argumentCount && areArgumentsValid == true; ++i)
    {
        const std::string_view option(arguments[i]);
        if (option == "--quick")
        {
            settings.RoughnessCount = QUICK_TABLE_SIZE;
            settings.ViewCount = QUICK_TABLE_SIZE;
            settings.FresnelCount = QUICK_FRESNEL_COUNT;
            settings.ObjectiveStrata = QUICK_OBJECTIVE_STRATA;
            settings.MomentStrata = QUICK_MOMENT_STRATA;
            settings.ResidualStrata = QUICK_RESIDUAL_STRATA;
            settings.MaxIterations = QUICK_MAX_ITERATIONS;
        }
        else
        {
            areArgumentsValid = option == "--workers" && i + 1 < argumentCount &&
                                TryParseWorkerCount(std::string_view(arguments[i + 1]), workerCount) == true;
            ++i;
        }
    }
    if (areArgumentsValid == false)
    {
        WriteProgress("usage: ludus_ltc_fit <layout directory> <data directory> [--quick] [--workers <0-64>]\n");
        return 2;
    }
    ludus::foundation::filesystem::WriteDirectory layoutDirectory;
    ludus::foundation::filesystem::WriteDirectory dataDirectory;
    if (layoutDirectory.Open(arguments[1]).Succeeded() == false ||
        dataDirectory.Open(arguments[2]).Succeeded() == false)
    {
        WriteProgress("ludus_ltc_fit: cannot open an output directory\n");
        return 1;
    }
    WriteProgress("ludus_ltc_fit: fitting the LTC table\n");
    Array<FittedCell> cells;
    if (FitTable(settings, workerCount, cells) == false)
    {
        WriteProgress("ludus_ltc_fit: fitting failed\n");
        return 1;
    }
    TextBuilder layout;
    TextBuilder provenance;
    Array<uint8> data;
    BuildLayout(settings, layout);
    if (BuildData(settings, cells, data) == false || BuildProvenance(settings, cells, provenance) == false ||
        layout.IsOk == false || provenance.IsOk == false ||
        Publish(layoutDirectory, LAYOUT_FILE, layout.Bytes) == false ||
        Publish(dataDirectory, TABLE_FILE, data) == false ||
        Publish(dataDirectory, PROVENANCE_FILE, provenance.Bytes) == false)
    {
        WriteProgress("ludus_ltc_fit: writing the outputs failed\n");
        return 1;
    }
    WriteProgress("ludus_ltc_fit: wrote ltc_table_layout.h, ltc_table.bin and ltc_table_provenance.json\n");
    return 0;
}
