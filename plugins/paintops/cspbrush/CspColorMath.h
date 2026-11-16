/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_COLOR_MATH_H
#define CSP_COLOR_MATH_H

#include <QtGlobal>

#include <array>

#include "kritacspbrushpaintop_export.h"

class KoColor;

/**
 * Color mixing backends of CSP's "Mixing mode":
 *  - Standard: plain interpolation of gamma-encoded sRGB (CSP Ver. 1)
 *  - Perceptual: pigment-like spectral mixing, the model of libmypaint's
 *    mix_colors() (Krita's MyPaint engine): each color is upsampled to
 *    10 reflectance bands and the bands are mixed by weighted geometric
 *    mean, so blue and yellow give green. Reflectance is linear, so colors
 *    are linearized first. Unlike mix_colors(), the weights are normalized
 *    (t = 0.5 is symmetric). Without libmypaint the build falls back to
 *    interpolation in OkLab.
 *  - Oklab: interpolation in OkLab, for the main/sub gradient of gray tips
 *    in Perceptual mode (spectral mixing with black darkens mid grays a lot)
 */
namespace CspColorMath
{
enum class Mode { Standard = 0, Perceptual = 1, Oklab = 2 };

using Rgb = std::array<float, 3>;       ///< gamma-encoded sRGB, 0..1
using Spectrum = std::array<float, 10>; ///< natural log of the reflectance per band

/// True when Perceptual mixing is spectral (built with libmypaint)
KRITACSPBRUSHPAINTOP_EXPORT bool hasSpectral();

KRITACSPBRUSHPAINTOP_EXPORT Rgb srgbToOklab(const Rgb &srgb);
KRITACSPBRUSHPAINTOP_EXPORT Rgb oklabToSrgb(const Rgb &lab);

/// Spectral round trip; only valid when hasSpectral()
KRITACSPBRUSHPAINTOP_EXPORT Spectrum toLogSpectrum(const Rgb &srgb);
KRITACSPBRUSHPAINTOP_EXPORT Rgb fromLogSpectrum(const Spectrum &logSpectrum);

/// toLogSpectrum() of a 16-bit sRGB pixel; the same result as toLogSpectrum({r/65535.f, g/65535.f, b/65535.f})
KRITACSPBRUSHPAINTOP_EXPORT Spectrum toLogSpectrum16(quint16 r, quint16 g, quint16 b);
/// mix(a, b, t, Perceptual) with both log-spectra given: the same result. Only valid when hasSpectral()
KRITACSPBRUSHPAINTOP_EXPORT Rgb mixSpectral(const Rgb &a, const Spectrum &logA, const Rgb &b, const Spectrum &logB, float t);

/// t = 0 gives a, t = 1 gives b
KRITACSPBRUSHPAINTOP_EXPORT Rgb mix(const Rgb &a, const Rgb &b, float t, Mode mode);
KRITACSPBRUSHPAINTOP_EXPORT KoColor mix(const KoColor &a, const KoColor &b, float t, Mode mode);

/// Like mix(), but each color counts by its alpha (a transparent color
/// brings no hue); the alpha itself is interpolated linearly
KRITACSPBRUSHPAINTOP_EXPORT void mixWithAlpha(const Rgb &a, float alphaA, const Rgb &b, float alphaB, float t, Mode mode,
                                              Rgb *color, float *alpha);
KRITACSPBRUSHPAINTOP_EXPORT KoColor mixWithAlpha(const KoColor &a, const KoColor &b, float t, Mode mode);
}

#endif // CSP_COLOR_MATH_H
