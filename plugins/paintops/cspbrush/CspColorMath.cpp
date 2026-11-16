/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspColorMath.h"

#include <QColor>
#include <QtGlobal>
#include <cmath>
#include <vector>

#include <KoColor.h>
#include <KoColorSpace.h>

#include <config-mypaint.h>

#ifdef HAVE_LIBMYPAINT
// from libmypaint's helpers.h, which also defines MIN/MAX/CLAMP macros
extern "C" {
void rgb_to_spectral(float r, float g, float b, float *spectral_);
void spectral_to_rgb(float *spectral, float *rgb_);
}
#endif

namespace CspColorMath
{
namespace {
float toLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float toGamma(float c)
{
    c = qBound(0.0f, c, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Rgb lerp(const Rgb &a, const Rgb &b, float t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
}

bool hasSpectral()
{
#ifdef HAVE_LIBMYPAINT
    return true;
#else
    return false;
#endif
}

Rgb srgbToOklab(const Rgb &srgb)
{
    const float r = toLinear(srgb[0]), g = toLinear(srgb[1]), b = toLinear(srgb[2]);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

Rgb oklabToSrgb(const Rgb &lab)
{
    const float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    return {toGamma(4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3),
            toGamma(-1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3),
            toGamma(-0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3)};
}

Spectrum toLogSpectrum(const Rgb &srgb)
{
    Spectrum s {};
#ifdef HAVE_LIBMYPAINT
    // libmypaint offsets the bands by WGM_EPSILON, so they are never 0
    rgb_to_spectral(toLinear(qBound(0.0f, srgb[0], 1.0f)), toLinear(qBound(0.0f, srgb[1], 1.0f)),
                    toLinear(qBound(0.0f, srgb[2], 1.0f)), s.data());
    for (float &band : s) band = std::log(band);
#else
    Q_UNUSED(srgb);
#endif
    return s;
}

Spectrum toLogSpectrum16(quint16 r, quint16 g, quint16 b)
{
    Spectrum s {};
#ifdef HAVE_LIBMYPAINT
    // the table holds the same toLinear(v / 65535.0f) that toLogSpectrum() evaluates (its clamp is a no-op here)
    static const std::vector<float> lut = [] {
        std::vector<float> t(65536);
        for (int v = 0; v < 65536; v++) t[size_t(v)] = toLinear(float(v) / 65535.0f);
        return t;
    }();
    rgb_to_spectral(lut[r], lut[g], lut[b], s.data());
    for (float &band : s) band = std::log(band);
#else
    Q_UNUSED(r);
    Q_UNUSED(g);
    Q_UNUSED(b);
#endif
    return s;
}

Rgb mixSpectral(const Rgb &a, const Spectrum &logA, const Rgb &b, const Spectrum &logB, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    Spectrum m;
    for (size_t i = 0; i < m.size(); i++) m[i] = logA[i] + (logB[i] - logA[i]) * t;
    return fromLogSpectrum(m);
}

Rgb fromLogSpectrum(const Spectrum &logSpectrum)
{
    Rgb rgb {0.0f, 0.0f, 0.0f};
#ifdef HAVE_LIBMYPAINT
    Spectrum s;
    for (size_t i = 0; i < s.size(); i++) s[i] = std::exp(logSpectrum[i]);
    spectral_to_rgb(s.data(), rgb.data());
    for (float &c : rgb) c = toGamma(c);
#else
    Q_UNUSED(logSpectrum);
#endif
    return rgb;
}

Rgb mix(const Rgb &a, const Rgb &b, float t, Mode mode)
{
    if (mode == Mode::Standard) return lerp(a, b, t);
    if (mode == Mode::Perceptual && hasSpectral()) {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        const Spectrum sa = toLogSpectrum(a);
        const Spectrum sb = toLogSpectrum(b);
        Spectrum m;
        for (size_t i = 0; i < m.size(); i++) m[i] = sa[i] + (sb[i] - sa[i]) * t;
        return fromLogSpectrum(m);
    }
    return oklabToSrgb(lerp(srgbToOklab(a), srgbToOklab(b), t));
}

KoColor mix(const KoColor &a, const KoColor &b, float t, Mode mode)
{
    QColor qa, qb;
    a.toQColor(&qa);
    b.toQColor(&qb);
    const Rgb m = mix(Rgb{float(qa.redF()), float(qa.greenF()), float(qa.blueF())},
                      Rgb{float(qb.redF()), float(qb.greenF()), float(qb.blueF())}, t, mode);
    QColor result = QColor::fromRgbF(m[0], m[1], m[2], qa.alphaF() + (qb.alphaF() - qa.alphaF()) * t);
    return KoColor(result, a.colorSpace());
}

void mixWithAlpha(const Rgb &a, float alphaA, const Rgb &b, float alphaB, float t, Mode mode, Rgb *color, float *alpha)
{
    const float wa = (1.0f - t) * alphaA;
    const float wb = t * alphaB;
    const float colorT = wa + wb > 1e-6f ? wb / (wa + wb) : t;
    *color = mix(a, b, colorT, mode);
    *alpha = alphaA + (alphaB - alphaA) * t;
}

KoColor mixWithAlpha(const KoColor &a, const KoColor &b, float t, Mode mode)
{
    const float alphaA = float(a.opacityF());
    const float alphaB = float(b.opacityF());
    const float wa = (1.0f - t) * alphaA;
    const float wb = t * alphaB;
    const float colorT = wa + wb > 1e-6f ? wb / (wa + wb) : t;
    KoColor result = mix(a, b, colorT, mode);
    result.setOpacity(qreal(alphaA + (alphaB - alphaA) * t));
    return result;
}
}
