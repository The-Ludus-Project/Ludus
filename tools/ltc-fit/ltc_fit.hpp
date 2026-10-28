#pragma once

// Offline fitter for the renderer's linearly transformed cosine (LTC) table.
//
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM
// Transactions on Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016), section 4,
// https://doi.org/10.1145/2897824.2925895: for every view inclination and
// roughness, fit the cosine-weighted BRDF (Equation 5) with an LTC whose matrix
// has the planar form of Equation 6, minimizing the L3 error, and store the
// inverse matrix with the BRDF's norm over (theta, sqrt(alpha)).
// Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
// ACM Transactions on Graphics 40(4), Article 140, 2021, section 4.1, page 9,
// https://doi.org/10.1145/3450626.3459672, for the third table axis over
// Schlick's F0 (a 64 x 64 x 51 table of transforms), and section 4.2 for
// tabulating the specular albedo beside the transform.
// The objective's Monte Carlo estimator, the optimizer, its initialization and
// the Fresnel moment are Ludus choices described in
// docs/development/ltc-table.md. No code from either paper's GPL-licensed
// utilities is used.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::tools::ltc_fit
{
using foundation::core::float64;
using foundation::core::uint32;
using foundation::core::uint64;

/// @brief Version of the fitting procedure; a change that alters fitted values bumps it.
inline constexpr uint32 LTC_FITTER_VERSION = 2;

/// @brief Table resolution and fitting parameters, recorded in the generated table's provenance.
struct FitSettings final
{
    /// Rows over perceptual roughness, linearly spaced from MinimumRoughness to one.
    uint32 RoughnessCount = 64;
    /// Columns over the view inclination theta, linearly spaced over [0, pi/2] (Heitz et al. 2016, section 4).
    uint32 ViewCount = 64;
    /// Slices over Schlick's F0, linearly spaced over [0, 1]. Peters 2021 (section 4.1) uses 51 F0 values without
    /// stating their spacing; the linear spacing is a Ludus choice.
    uint32 FresnelCount = 51;
    /// The renderer's smallest admitted perceptual roughness; alpha is its square.
    float64 MinimumRoughness = 0.045;
    /// Smallest view cosine fitted; the grazing column theta = pi/2 is fitted at this cosine.
    float64 MinimumViewCosine = 1.0e-4;
    /// Strata per axis of each of the objective's two fixed sample sets.
    uint32 ObjectiveStrata = 32;
    /// Strata per axis of the BRDF moment integration.
    uint32 MomentStrata = 256;
    /// Strata per axis of each of the residual's two independent sample sets.
    uint32 ResidualStrata = 64;
    /// Nelder-Mead iterations per start.
    uint32 MaxIterations = 1500;
    /// Nelder-Mead starts per cell; each restart begins a fresh simplex at the best point found.
    uint32 StartCount = 3;
    /// Seed of the jitter of every fixed sample set.
    uint64 Seed = 0x17C0F17ULL;
};

/// @brief The fitted LTC of one table cell and its diagnostics.
///
/// The cell fits rho cos with Schlick's Fresnel for the cell's F0. The moments Norm and FresnelNorm do not depend on
/// F0; the fitted lobe's own norm is F0 * (Norm - FresnelNorm) + FresnelNorm. The matrix is M = [[A, 0, B], [0, C, 0],
/// [D, 0, 1]] (Heitz et al. 2016, Equation 6) in the frame whose x-z plane contains the view direction (sin theta, 0,
/// cos theta).
struct FittedCell final
{
    float64 A = 1.0; ///< M(0, 0).
    float64 B = 0.0; ///< M(0, 2).
    float64 C = 1.0; ///< M(1, 1).
    float64 D = 0.0; ///< M(2, 0).
    /// Directional albedo of the specular lobe with a Fresnel factor of one: the integral of rho * cos.
    float64 Norm = 0.0;
    /// Integral of (1 - v.h)^5 rho * cos; Schlick's albedo is F0 * (Norm - FresnelNorm) + FresnelNorm.
    float64 FresnelNorm = 0.0;
    /// Integral of |LTC - rho F cos / norm| over the sphere on independent samples, in [0, 2], for the cell's F0.
    float64 RelativeL1Error = 0.0;
    /// L3 objective at the solution, on the fitting samples.
    float64 Objective = 0.0;
    /// Objective evaluations spent on the cell.
    uint32 EvaluationCount = 0;
};

/// @brief The inverse transform stored in the table, normalized so that its (0, 0) entry is one.
///
/// M^-1 is proportional to [[1, 0, M02], [0, M11, 0], [M20, 0, M22]]; the LTC is invariant to that scale.
struct StoredCell final
{
    float64 M02 = 0.0;
    float64 M11 = 1.0;
    float64 M20 = 0.0;
    float64 M22 = 1.0;
    float64 Norm = 0.0;
    float64 FresnelNorm = 0.0;
};

/// @brief Perceptual roughness of table row index.
[[nodiscard]] float64 GetTableRoughness(const FitSettings& settings, uint32 index) noexcept;

/// @brief View cosine fitted for table column index, at least MinimumViewCosine.
[[nodiscard]] float64 GetTableViewCosine(const FitSettings& settings, uint32 index) noexcept;

/// @brief Schlick F0 of table slice index.
[[nodiscard]] float64 GetTableFresnel(const FitSettings& settings, uint32 index) noexcept;

/// @brief Fit one cell, starting from the best of the initial guesses.
/// @param settings Fitting parameters.
/// @param roughness Perceptual roughness; alpha is its square.
/// @param viewCosine Cosine of the view inclination, in (0, 1].
/// @param fresnel Schlick F0 of the fitted lobe, in [0, 1].
/// @param initialGuesses Candidate starting matrices; their moments and diagnostics are ignored. Empty starts from the
/// identity.
/// @param outCell The fitted cell; written only on success.
/// @return false when no candidate has a finite objective or allocation fails.
[[nodiscard]] bool FitCell(const FitSettings& settings,
                           float64 roughness,
                           float64 viewCosine,
                           float64 fresnel,
                           foundation::BorrowedView<const FittedCell> initialGuesses,
                           FittedCell& outCell) noexcept;

/// @brief Fit the whole table. Every F0 slice is an independent job; within a slice, rows run from rough to smooth and
/// each column from normal to grazing incidence, each cell started from its already fitted neighbors. The result does
/// not depend on the worker count.
/// @param settings Fitting parameters.
/// @param workerCount Job system worker threads; zero fits every slice on the calling thread.
/// @param outCells FresnelCount * RoughnessCount * ViewCount cells, index (fresnel * RoughnessCount + roughness) *
/// ViewCount + view; written only on success.
/// @return false when a cell cannot be fitted, the job system cannot start or allocation fails.
[[nodiscard]] bool
FitTable(const FitSettings& settings, uint32 workerCount, foundation::Array<FittedCell>& outCells) noexcept;

/// @brief The normalized inverse transform and moments of a fitted cell.
[[nodiscard]] StoredCell MakeStoredCell(const FittedCell& cell) noexcept;
} // namespace ludus::tools::ltc_fit
