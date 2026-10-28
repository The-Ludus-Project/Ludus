// ludus_ltc_fit: fits the renderer's linearly transformed cosine table and
// writes ltc_table_data.inc and ltc_table_provenance.json into a directory,
// normally modules/graphics/renderer/shaders. See docs/development/ltc-table.md.

#include "ltc_fit.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/scalar.hpp>

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
constexpr uint32 TABLE_LAYOUT_VERSION = 1;
// Percentiles summarized in the provenance file.
constexpr float64 MEDIAN = 0.5;
constexpr float64 PERCENTILE_95 = 0.95;
// A fast, low-resolution configuration for checking the tool itself.
constexpr uint32 QUICK_TABLE_SIZE = 8;
constexpr uint32 QUICK_OBJECTIVE_STRATA = 16;
constexpr uint32 QUICK_MOMENT_STRATA = 64;
constexpr uint32 QUICK_RESIDUAL_STRATA = 32;
constexpr uint32 QUICK_MAX_ITERATIONS = 400;
constexpr std::string_view TABLE_FILE = "ltc_table_data.inc";
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

void BuildTable(const FitSettings& settings, const Array<FittedCell>& cells, TextBuilder& outText) noexcept
{
    outText.Append(R"(#pragma once

// Linearly transformed cosine (LTC) table for the renderer's specular BSDF: GGX with alpha = roughness^2, the
// height-correlated Smith visibility and Schlick's Fresnel (reference_bsdf.cpp, renderer_gpu_preview.slang).
// GENERATED by tools/ltc-fit (ludus_ltc_fit); do not edit. docs/development/ltc-table.md describes the fit, how to
// regenerate this file and its accuracy; ltc_table_provenance.json records the residual error of every cell.
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, "Real-Time Polygonal-Light Shading with
// Linearly Transformed Cosines", ACM Transactions on Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016), section 4,
// https://doi.org/10.1145/2897824.2925895, for the fit, and to Christoph Peters, "BRDF Importance Sampling for
// Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021, sections 4.1-4.2,
// https://doi.org/10.1145/3450626.3459672, for tabulating the specular albedo beside the transform.
// Fit parameters:
)");
    AppendSettings(outText, settings);
    outText.Append(R"(//
// Cell (r, t) covers perceptual roughness LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS + r * (1 - minimum) / (rows - 1) and
// view inclination theta = t * (pi / 2) / (columns - 1); cells are roughness-major. Entry 2 * cell holds
// (M02, M11, M20, M22) of the inverse transform M^-1 = [[1, 0, M02], [0, M11, 0], [M20, 0, M22]] up to a positive
// scale, in the frame whose x-z plane contains the view direction (sin theta, 0, cos theta). Entry 2 * cell + 1 holds
// (norm, Fresnel norm, 0, 0): the specular albedo with Schlick's Fresnel is F0 * (norm - Fresnel norm) + Fresnel norm.
// The table is declared when the includer defines LUDUS_LTC_TABLE_ENTRY as a four-float aggregate type (float4 in
// Slang); without it, only the layout constants are defined.

)");
    outText.Append("#define LUDUS_LTC_TABLE_VERSION ");
    outText.AppendUnsigned(TABLE_LAYOUT_VERSION);
    outText.Append("\n#define LUDUS_LTC_TABLE_FITTER_VERSION ");
    outText.AppendUnsigned(LTC_FITTER_VERSION);
    outText.Append("\n#define LUDUS_LTC_TABLE_ROUGHNESS_COUNT ");
    outText.AppendUnsigned(settings.RoughnessCount);
    outText.Append("\n#define LUDUS_LTC_TABLE_VIEW_COUNT ");
    outText.AppendUnsigned(settings.ViewCount);
    outText.Append("\n#define LUDUS_LTC_TABLE_ENTRIES_PER_CELL 2\n#define LUDUS_LTC_TABLE_ENTRY_COUNT "
                   "(LUDUS_LTC_TABLE_ROUGHNESS_COUNT * LUDUS_LTC_TABLE_VIEW_COUNT * LUDUS_LTC_TABLE_ENTRIES_PER_CELL)\n"
                   "#define LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS ");
    outText.AppendFloat32(settings.MinimumRoughness);
    outText.Append("\n\n#ifdef LUDUS_LTC_TABLE_ENTRY\nstatic const LUDUS_LTC_TABLE_ENTRY "
                   "LUDUS_LTC_TABLE[LUDUS_LTC_TABLE_ENTRY_COUNT] =\n{\n");
    for (uint32 row = 0; row < settings.RoughnessCount; ++row)
    {
        outText.Append("    // Roughness row ");
        outText.AppendUnsigned(row);
        outText.Append(".\n");
        for (uint32 column = 0; column < settings.ViewCount; ++column)
        {
            const StoredCell stored = MakeStoredCell(cells[usize{row} * settings.ViewCount + column]);
            outText.Append("    {");
            outText.AppendFloat32(stored.M02);
            outText.Append(", ");
            outText.AppendFloat32(stored.M11);
            outText.Append(", ");
            outText.AppendFloat32(stored.M20);
            outText.Append(", ");
            outText.AppendFloat32(stored.M22);
            outText.Append("},\n    {");
            outText.AppendFloat32(stored.Norm);
            outText.Append(", ");
            outText.AppendFloat32(stored.FresnelNorm);
            outText.Append(", 0.0f, 0.0f},\n");
        }
    }
    outText.Append("};\n#endif\n");
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
    for (usize i = 0; i < cells.GetSize(); ++i)
    {
        if (all.TryAdd(cells[i].RelativeL1Error) == false ||
            (i < settings.ViewCount && minimumRow.TryAdd(cells[i].RelativeL1Error) == false))
        {
            return false;
        }
    }
    outText.Append("{\n  \"generator\": \"tools/ltc-fit (ludus_ltc_fit)\",\n  \"fitterVersion\": ");
    outText.AppendUnsigned(LTC_FITTER_VERSION);
    outText.Append(",\n  \"tableVersion\": ");
    outText.AppendUnsigned(TABLE_LAYOUT_VERSION);
    outText.Append(",\n  \"brdf\": \"Ludus specular lobe: GGX, alpha = roughness^2, height-correlated Smith, Fresnel 1 "
                   "(Schlick handled by the Fresnel norm)\",\n  \"objective\": \"L3 error of the normalized "
                   "cosine-weighted BRDF (Heitz et al. 2016, section 4)\",\n  \"residual\": \"relative L1 error: "
                   "integral of |LTC - rho cos / norm| on independent samples, in [0, 2]\",\n  \"settings\": {"
                   "\"roughnessCount\": ");
    outText.AppendUnsigned(settings.RoughnessCount);
    outText.Append(", \"viewCount\": ");
    outText.AppendUnsigned(settings.ViewCount);
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
    outText.Append("},\n  \"relativeL1Error\": [\n");
    for (uint32 row = 0; row < settings.RoughnessCount; ++row)
    {
        outText.Append("    [");
        for (uint32 column = 0; column < settings.ViewCount; ++column)
        {
            outText.Append(column == 0 ? "" : ", ");
            outText.AppendDiagnostic(cells[usize{row} * settings.ViewCount + column].RelativeL1Error);
        }
        outText.Append(row + 1 == settings.RoughnessCount ? "]\n" : "],\n");
    }
    outText.Append("  ]\n}\n");
    return true;
}

bool Publish(const ludus::foundation::filesystem::WriteDirectory& directory,
             std::string_view file,
             const TextBuilder& text) noexcept
{
    const ludus::foundation::filesystem::PublicationResult result =
        directory.Publish(file, std::span<const uint8>(text.Bytes.GetData(), text.Bytes.GetSize()));
    return result.Published == true;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount < 2 || argumentCount > 3)
    {
        WriteProgress("usage: ludus_ltc_fit <output directory> [--quick]\n");
        return 2;
    }
    FitSettings settings;
    if (argumentCount == 3)
    {
        if (std::string_view(arguments[2]) != "--quick")
        {
            WriteProgress("ludus_ltc_fit: unknown option\n");
            return 2;
        }
        settings.RoughnessCount = QUICK_TABLE_SIZE;
        settings.ViewCount = QUICK_TABLE_SIZE;
        settings.ObjectiveStrata = QUICK_OBJECTIVE_STRATA;
        settings.MomentStrata = QUICK_MOMENT_STRATA;
        settings.ResidualStrata = QUICK_RESIDUAL_STRATA;
        settings.MaxIterations = QUICK_MAX_ITERATIONS;
    }
    ludus::foundation::filesystem::WriteDirectory directory;
    if (directory.Open(arguments[1]).Succeeded() == false)
    {
        WriteProgress("ludus_ltc_fit: cannot open the output directory\n");
        return 1;
    }
    WriteProgress("ludus_ltc_fit: fitting the LTC table\n");
    Array<FittedCell> cells;
    if (FitTable(settings, cells) == false)
    {
        WriteProgress("ludus_ltc_fit: fitting failed\n");
        return 1;
    }
    TextBuilder table;
    TextBuilder provenance;
    BuildTable(settings, cells, table);
    if (BuildProvenance(settings, cells, provenance) == false || table.IsOk == false || provenance.IsOk == false ||
        Publish(directory, TABLE_FILE, table) == false || Publish(directory, PROVENANCE_FILE, provenance) == false)
    {
        WriteProgress("ludus_ltc_fit: writing the outputs failed\n");
        return 1;
    }
    WriteProgress("ludus_ltc_fit: wrote ltc_table_data.inc and ltc_table_provenance.json\n");
    return 0;
}
