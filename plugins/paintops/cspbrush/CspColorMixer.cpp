/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspColorMixer.h"

#include <QColor>
#include <QPointF>
#include <QScopedPointer>
#include <QThread>
#include <QtConcurrentMap>
#include <QtMath>

#include <cmath>
#include <limits>
#include <numeric>

#include <KoColorConversionTransformation.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceTraits.h>
#include <KoCompositeOpRegistry.h>
#include <KoMixColorsOp.h>
#include <kis_fixed_paint_device.h>
#include <kis_paint_device.h>
#include <kis_painter.h>

#include "CspColorMath.h"

bool CspMixProfile::enabled = false;
qint64 CspMixProfile::prepNs = 0;
qint64 CspMixProfile::sampleNs = 0;
qint64 CspMixProfile::layDownNs = 0;
qint64 CspMixProfile::flushNs = 0;
qint64 CspMixProfile::dabNs = 0;
int CspMixProfile::dabs = 0;

void CspMixProfile::reset()
{
    prepNs = sampleNs = layDownNs = flushNs = dabNs = 0;
    dabs = 0;
}

namespace {
// Perceptual mixing works on gamma-encoded sRGB: device pixels go through 16-bit sRGB
using Px16 = KoBgrU16Traits::Pixel;

const KoColorSpace *srgb16()
{
    // explicit profile: the default 16-bit RGB profile is not the sRGB tone curve
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    return registry->rgb16(registry->p709SRGBProfile());
}

QVector<Px16> toSrgb16(const KoColorSpace *cs, const quint8 *data, int n)
{
    QVector<Px16> out(n);
    cs->convertPixelsTo(data, reinterpret_cast<quint8 *>(out.data()), srgb16(), n,
                        KoColorConversionTransformation::internalRenderingIntent(),
                        KoColorConversionTransformation::internalConversionFlags());
    return out;
}

CspColorMath::Rgb rgbOf(const Px16 &p)
{
    return {p.red / 65535.0f, p.green / 65535.0f, p.blue / 65535.0f};
}

/// Log-spectra of the pixels, reusing the last one for runs of the same color
class SpectrumCache
{
public:
    const CspColorMath::Spectrum &at(const Px16 &p)
    {
        if (!m_valid || p.red != m_last.red || p.green != m_last.green || p.blue != m_last.blue) {
            m_spectrum = CspColorMath::toLogSpectrum16(p.red, p.green, p.blue);
            m_last = p;
            m_valid = true;
        }
        return m_spectrum;
    }

private:
    Px16 m_last {};
    CspColorMath::Spectrum m_spectrum {};
    bool m_valid {false};
};

KoColor sampleSpectral(const KoColorSpace *cs, const quint8 *data, int n, const QVector<qint16> &weights)
{
    const QVector<Px16> px = toSrgb16(cs, data, n);
    const bool weighted = weights.size() == n;
    SpectrumCache cache;
    CspColorMath::Spectrum sum {};
    double colorWeight = 0.0;
    double alphaSum = 0.0;
    double weightSum = 0.0;
    for (int i = 0; i < n; i++) {
        const double w = weighted ? weights[i] : 1.0;
        if (w <= 0) continue;
        const double alpha = px[i].alpha / 65535.0;
        weightSum += w;
        alphaSum += w * alpha;
        if (alpha <= 0) continue;
        // like a premultiplied average: transparent pixels bring no hue
        const CspColorMath::Spectrum &s = cache.at(px[i]);
        for (size_t b = 0; b < sum.size(); b++) sum[b] += float(w * alpha) * s[b];
        colorWeight += w * alpha;
    }

    KoColor result(cs);
    if (colorWeight <= 0) {
        cs->setOpacity(result.data(), quint8(0), 1);
        return result;
    }
    for (float &band : sum) band = float(band / colorWeight);
    const CspColorMath::Rgb rgb = CspColorMath::fromLogSpectrum(sum);
    Px16 out;
    out.red = quint16(qRound(rgb[0] * 65535.0f));
    out.green = quint16(qRound(rgb[1] * 65535.0f));
    out.blue = quint16(qRound(rgb[2] * 65535.0f));
    out.alpha = quint16(qRound(alphaSum / weightSum * 65535.0));
    return KoColor(reinterpret_cast<const quint8 *>(&out), srgb16()).convertedTo(cs);
}

/// Perceptual lay-down of one (possibly mirrored) copy: each pixel mixed with its own canvas color
void layDownPerceptual(KisPainter *painter, const QRect &rect, KisFixedPaintDeviceSP mask, const KoColor &paint,
                       qreal strength)
{
    const KoColorSpace *cs = painter->device()->colorSpace();
    const int n = rect.width() * rect.height();
    QByteArray dst(n * cs->pixelSize(), 0);
    painter->device()->readBytes(reinterpret_cast<quint8 *>(dst.data()), rect);
    QVector<Px16> px = toSrgb16(cs, reinterpret_cast<const quint8 *>(dst.constData()), n);

    const KoColor p16 = paint.convertedTo(srgb16());
    const Px16 &pp = *reinterpret_cast<const Px16 *>(p16.data());
    const CspColorMath::Rgb paintRgb = rgbOf(pp);
    const float paintAlpha = pp.alpha / 65535.0f;

    KisFixedPaintDeviceSP coverage = new KisFixedPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    coverage->setRect(rect);
    coverage->lazyGrowBufferWithoutInitialization();
    quint8 *cov = coverage->data();
    const quint8 *m = mask->data();
    const float k = float(qBound(0.0, strength, 1.0)) / 255.0f;

    const CspColorMath::Spectrum paintLog = CspColorMath::toLogSpectrum16(pp.red, pp.green, pp.blue);
    Px16 *pxData = px.data();

    // pixels [begin, end): each depends only on its own inputs, the memos only short-cut an identical computation
    auto process = [&](int begin, int end) {
        bool resultValid = false;
        Px16 lastIn {};
        Px16 lastOut {};
        quint8 lastMask = 0;
        SpectrumCache canvasCache;
        for (int i = begin; i < end; i++) {
            const float w = m[i] * k;
            cov[i] = w > 0.0f ? 255 : 0;
            if (w <= 0.0f) continue;
            Px16 &p = pxData[i];
            if (resultValid && m[i] == lastMask && p.red == lastIn.red && p.green == lastIn.green
                && p.blue == lastIn.blue && p.alpha == lastIn.alpha) {
                p = lastOut;
                continue;
            }
            const Px16 in = p;
            const float alphaA = in.alpha / 65535.0f;
            // mixWithAlpha(), Perceptual
            const float wa = (1.0f - w) * alphaA;
            const float wb = w * paintAlpha;
            const float colorT = wa + wb > 1e-6f ? wb / (wa + wb) : w;
            const float a = alphaA + (paintAlpha - alphaA) * w;
            CspColorMath::Rgb c;
            if (colorT <= 0.0f) {
                c = rgbOf(in);
            } else if (colorT >= 1.0f) {
                c = paintRgb;
            } else {
                c = CspColorMath::mixSpectral(rgbOf(in), canvasCache.at(in), paintRgb, paintLog, colorT);
            }
            p.red = quint16(qRound(c[0] * 65535.0f));
            p.green = quint16(qRound(c[1] * 65535.0f));
            p.blue = quint16(qRound(c[2] * 65535.0f));
            p.alpha = quint16(qRound(qBound(0.0f, a, 1.0f) * 65535.0f));
            lastIn = in;
            lastOut = p;
            lastMask = m[i];
            resultValid = true;
        }
    };
    const int w16 = rect.width();
    const int h = rect.height();
    const int bands = qMin(QThread::idealThreadCount(), h / 16);
    if (n >= 128 * 128 && bands >= 2) {
        // bands of rows; each has its own memos and writes only its own rows
        QVector<QPair<int, int>> bandList;
        for (int b = 0; b < bands; b++) {
            bandList << qMakePair(int(qint64(h) * b / bands) * w16, int(qint64(h) * (b + 1) / bands) * w16);
        }
        QtConcurrent::blockingMap(bandList, [&](QPair<int, int> &band) { process(band.first, band.second); });
    } else {
        process(0, n);
    }

    KisFixedPaintDeviceSP paintDab = new KisFixedPaintDevice(cs);
    paintDab->setRect(rect);
    paintDab->lazyGrowBufferWithoutInitialization();
    srgb16()->convertPixelsTo(reinterpret_cast<const quint8 *>(px.constData()), paintDab->data(), cs, n,
                              KoColorConversionTransformation::internalRenderingIntent(),
                              KoColorConversionTransformation::internalConversionFlags());

    painter->setOpacityF(1.0);
    painter->bltFixedWithFixedSelection(rect.x(), rect.y(), paintDab, coverage,
                                        rect.x(), rect.y(), rect.x(), rect.y(),
                                        rect.width(), rect.height());
}
}

CspColorMixer::CspColorMixer(const CspBrushParams &params, bool erasing)
    : m_params(params)
    , m_erasing(erasing)
{
}

QVector<CspColorMixer::MirrorCopy> CspColorMixer::mirrorCopies(KisPainter *painter, const QRect &rect)
{
    const bool h = painter->hasHorizontalMirroring();
    const bool v = painter->hasVerticalMirroring();
    // the order of calculateAllMirroredRects(): none, then H; H, HV, V; or V
    QVector<QPair<bool, bool>> flips = {{false, false}};
    if (h && v) flips << qMakePair(true, false) << qMakePair(true, true) << qMakePair(false, true);
    else if (h) flips << qMakePair(true, false);
    else if (v) flips << qMakePair(false, true);

    const QVector<QRect> rects = painter->calculateAllMirroredRects(rect);
    QVector<MirrorCopy> copies;
    for (int i = 0; i < rects.size() && i < flips.size(); i++) {
        copies << MirrorCopy{rects[i], flips[i].first, flips[i].second};
    }
    return copies;
}

namespace {
/// Linear through the points (x ascending), flat outside them
qreal interpolate(const std::initializer_list<QPointF> &points, qreal x)
{
    const QPointF *previous = nullptr;
    for (const QPointF &p : points) {
        if (x <= p.x()) {
            if (!previous) return p.y();
            return previous->y() + (p.y() - previous->y()) * (x - previous->x()) / (p.x() - previous->x());
        }
        previous = &p;
    }
    return previous ? previous->y() : 0.0;
}
}

// The constants of the four functions below are fitted to strokes drawn in Clip Studio Paint: three modes,
// Color stretch 0 / 10 / 50 / 95 / 100, Amount of paint 0 / 50 / 100, Density of paint 50 (the reference
// table of CspBrushStrokeTest::testMatchesCspReference). Amount and Density are known at those values only;
// in between, the curves are the simplest ones through them.

qreal CspColorMixer::halfDistance(qreal colorStretch, int mixingType)
{
    const qreal stretch = qBound(0.0, colorStretch, 1.0);
    if (mixingType == CspBrushParams::MixRunningColor) {
        // short up to the middle of the range; at 100 the start color is never given up
        if (stretch >= 1.0) return std::numeric_limits<qreal>::infinity();
        if (stretch > 0.95) return 2.25 * 0.05 / (1.0 - stretch);
        return interpolate({{0.0, 0.1}, {0.1, 0.1}, {0.5, 0.2}, {0.95, 2.25}}, stretch);
    }
    // Blend: 95 and 100 are the same. Smear gives its color up 1.4 times slower at every stretch
    const qreal blend = interpolate({{0.0, 0.2}, {0.1, 0.5}, {0.5, 2.0}, {0.95, 3.4}}, stretch);
    return mixingType == CspBrushParams::MixSmear ? 1.4 * blend : blend;
}

qreal CspColorMixer::startCanvasShare(qreal amount, int mixingType)
{
    const qreal canvas = 1.0 - qBound(0.0, amount, 1.0);
    // at Amount 50: Blend 64%, Smear 68%, Running color 30%
    const qreal power = mixingType == CspBrushParams::MixRunningColor ? 1.75
                      : mixingType == CspBrushParams::MixSmear ? 0.55 : 0.65;
    return std::pow(canvas, power);
}

qreal CspColorMixer::canvasShare(qreal amount, int mixingType)
{
    const qreal canvas = 1.0 - qBound(0.0, amount, 1.0);
    // Running color takes little from the canvas while it moves: 6% at Amount 50
    return mixingType == CspBrushParams::MixRunningColor ? std::pow(canvas, 4.0) : canvas;
}

qreal CspColorMixer::densityAlpha(qreal density, int mixingType)
{
    // at Density 50 a Blend stroke over an empty layer settles at 83%, a Running color stroke at 100%
    const qreal power = mixingType == CspBrushParams::MixRunningColor ? 8.0 : 2.5;
    return 1.0 - std::pow(1.0 - qBound(0.0, density, 1.0), power);
}

namespace {
/// A color as the mixing math takes it: gamma-encoded sRGB and alpha, 0..1
struct FloatColor {
    CspColorMath::Rgb rgb {0.0f, 0.0f, 0.0f};
    float alpha {0.0f};
};

// Through QColor, as CspColorMath::mix(KoColor) does: the color arrives as 8-bit sRGB whatever the layer's depth
FloatColor toFloat(const KoColor &color)
{
    QColor q;
    color.toQColor(&q);
    return {{float(q.redF()), float(q.greenF()), float(q.blueF())}, float(color.opacityF())};
}

KoColor toKoColor(const CspColorMath::Rgb &rgb, float alpha, const KoColorSpace *cs)
{
    KoColor result(QColor::fromRgbF(qBound(0.0f, rgb[0], 1.0f), qBound(0.0f, rgb[1], 1.0f),
                                    qBound(0.0f, rgb[2], 1.0f)), cs);
    result.setOpacity(qreal(qBound(0.0f, alpha, 1.0f)));
    return result;
}

FloatColor mixed(const FloatColor &a, const FloatColor &b, float t, CspColorMath::Mode mode)
{
    FloatColor result;
    CspColorMath::mixWithAlpha(a.rgb, a.alpha, b.rgb, b.alpha, t, mode, &result.rgb, &result.alpha);
    return result;
}
}

KoColor CspColorMixer::paintColor(const KoColor &canvas, const KoColor &brush, qreal amount, qreal density,
                                  qreal distance)
{
    const CspColorMath::Mode mode = m_params.mixingMode;
    const int type = m_params.mixingType;
    const bool spectral = mode == CspColorMath::Mode::Perceptual && CspColorMath::hasSpectral();
    const FloatColor canvasColor = toFloat(canvas);
    const float canvasAlpha = qBound(0.0f, canvasColor.alpha, 1.0f);

    // where the carried color is heading: the canvas color with the drawing color mixed in. Each counts by
    // its alpha, so an empty layer brings no color. The eraser has no drawing color
    FloatColor target = canvasColor;
    if (!m_erasing) {
        target = mixed(canvasColor, toFloat(brush), float(1.0 - canvasShare(amount, type)), mode);
    }
    // the alpha the paint is heading for: Density of paint, between the canvas's alpha and opaque
    const float alphaTarget = m_erasing ? canvasAlpha
                                        : canvasAlpha + (1.0f - canvasAlpha) * float(densityAlpha(density, type));

    if (!m_started) {
        // the stroke starts from the color under its first dab, with some of the drawing color already in it
        FloatColor start = canvasColor;
        if (!m_erasing) {
            start = mixed(canvasColor, toFloat(brush), float(1.0 - startCanvasShare(amount, type)), mode);
        }
        m_carried = start.rgb;
        // with any drawing color in it the brush holds color in full; with none, as much as the canvas gave
        m_carriedWeight = !m_erasing && amount > 0.0 ? 1.0f : canvasAlpha;
        m_carriedAlpha = alphaTarget;
        m_carriedLogValid = false;
        m_space = (m_erasing ? canvas : brush).colorSpace();
        m_started = true;
    } else {
        // the share of the way to the target covered in this step: half of it every halfDistance()
        const qreal half = halfDistance(m_params.colorStretch, type);
        const float f = half <= 0.0 ? 1.0f : float(1.0 - std::pow(0.5, qMax(0.0, distance) / half));
        const float wa = (1.0f - f) * m_carriedWeight;
        // a target with any color in it counts in full: over an empty layer the drawing color takes over
        // at the same pace as on paint (CSP: the same half-distance inside and outside the band)
        const float wb = target.alpha > 0.0f ? f : 0.0f;
        if (wb > 0.0f) {
            const float colorT = wb / (wa + wb);
            if (colorT >= 1.0f) {
                m_carried = target.rgb;
                m_carriedLogValid = false;
            } else if (!spectral) {
                m_carried = CspColorMath::mix(m_carried, target.rgb, colorT, mode);
            } else {
                // Perceptual keeps the carried color as a spectrum: going to RGB and back at every dab
                // loses a little each time, and many small steps would stop short of the target
                if (!m_carriedLogValid) {
                    m_carriedLog = CspColorMath::toLogSpectrum(m_carried);
                    m_carriedLogValid = true;
                }
                const CspColorMath::Spectrum targetLog = CspColorMath::toLogSpectrum(target.rgb);
                for (size_t i = 0; i < m_carriedLog.size(); i++) {
                    m_carriedLog[i] += (targetLog[i] - m_carriedLog[i]) * colorT;
                }
                m_carried = CspColorMath::fromLogSpectrum(m_carriedLog);
            }
            // once the brush holds color it keeps it: over an empty layer only the drawing color changes it
            m_carriedWeight = qMax(m_carriedWeight, wa + wb);
        }
        // the alpha settles sooner than the color: in CSP over about 0.45 of the color's distance
        const float fAlpha = half <= 0.0 ? 1.0f : float(1.0 - std::pow(0.5, qMax(0.0, distance) / (0.45 * half)));
        m_carriedAlpha += (alphaTarget - m_carriedAlpha) * fAlpha;
    }

    if (m_erasing) {
        // no drawing color to lay down: the canvas color moves along, and density takes alpha away
        KoColor paint = type == CspBrushParams::MixRunningColor ? canvas : toKoColor(m_carried, 1.0f, m_space);
        paint.setOpacity(qBound(0.0, canvas.opacityF() * (1.0 - density), 1.0));
        return paint;
    }

    // Smear lays its color down opaque (Density of paint is not used); Blend and Running color carry the
    // alpha along with the color. A brush that holds no color yet leaves the canvas as it is
    float alpha = type == CspBrushParams::MixSmear ? 1.0f : m_carriedAlpha;
    if (m_carriedWeight < 1.0f) alpha = canvasAlpha + (alpha - canvasAlpha) * m_carriedWeight;
    return toKoColor(m_carriedWeight > 0.0f ? m_carried : canvasColor.rgb, alpha, m_space);
}

namespace {
/// The color of @p n pixels weighted by @p weights (empty = uniform)
KoColor sampleDense(const KoColorSpace *cs, const quint8 *pixels, int n, const QVector<qint16> &weights,
                    CspColorMath::Mode mode)
{
    if (mode == CspColorMath::Mode::Perceptual && CspColorMath::hasSpectral()) {
        return sampleSpectral(cs, pixels, n, weights);
    }

    QScopedPointer<KoMixColorsOp::Mixer> mixer(cs->mixColorsOp()->createMixer());
    if (weights.size() == n) {
        // the real sum: with less the alpha comes out too high, and reads as opaque on all but the smallest dabs
        const int weightSum = std::accumulate(weights.constBegin(), weights.constEnd(), 0);
        mixer->accumulate(pixels, weights.constData(), weightSum, n);
    } else {
        mixer->accumulateAverage(pixels, n);
    }
    KoColor result(cs);
    if (mixer->currentWeightsSum() > 0) {
        mixer->computeMixedColor(result.data());
    } else {
        cs->setOpacity(result.data(), quint8(0), 1);
    }
    return result;
}

/// Weights from either @p weights (size n) or @p mask (non-null), or none
KoColor sampleImpl(KisPaintDeviceSP device, const QRect &rect, const QVector<qint16> *weights, const quint8 *mask,
                   CspColorMath::Mode mode, int maxSamples)
{
    const KoColorSpace *cs = device->colorSpace();
    const int n = rect.width() * rect.height();
    const int pixelSize = int(cs->pixelSize());
    QByteArray pixels(n * pixelSize, 0);
    device->readBytes(reinterpret_cast<quint8 *>(pixels.data()), rect);
    const bool weighted = mask || (weights && weights->size() == n);
    auto weightAt = [&](int i) -> qint16 { return mask ? qint16(mask[i]) : (*weights)[i]; };

    if (maxSamples > 0 && n > maxSamples) {
        // a regular grid of pixels, with their weights
        const int step = qCeil(qSqrt(double(n) / maxSamples));
        const int w = rect.width();
        const int h = rect.height();
        QByteArray sampled;
        sampled.reserve((w / step + 1) * (h / step + 1) * pixelSize);
        QVector<qint16> sampledWeights;
        int count = 0;
        for (int y = step / 2; y < h; y += step) {
            for (int x = step / 2; x < w; x += step) {
                const int i = y * w + x;
                sampled.append(pixels.constData() + i * pixelSize, pixelSize);
                if (weighted) sampledWeights.append(weightAt(i));
                count++;
            }
        }
        // a tip with little coverage (a hairline, sparse dots) may miss the grid: then every pixel is read
        const bool gridHit = !weighted || std::accumulate(sampledWeights.constBegin(), sampledWeights.constEnd(), 0) > 0;
        if (count > 0 && gridHit) {
            return sampleDense(cs, reinterpret_cast<const quint8 *>(sampled.constData()), count, sampledWeights, mode);
        }
    }

    QVector<qint16> denseWeights;
    if (mask) {
        denseWeights.resize(n);
        for (int i = 0; i < n; i++) denseWeights[i] = mask[i];
        weights = &denseWeights;
    }
    static const QVector<qint16> none;
    return sampleDense(cs, reinterpret_cast<const quint8 *>(pixels.constData()), n, weights ? *weights : none, mode);
}
}

KoColor CspColorMixer::sampleCanvas(KisPaintDeviceSP device, const QRect &rect, const QVector<qint16> &weights,
                                    CspColorMath::Mode mode, int maxSamples)
{
    return sampleImpl(device, rect, &weights, nullptr, mode, maxSamples);
}

KoColor CspColorMixer::sampleCanvas(KisPaintDeviceSP device, const QRect &rect, const quint8 *mask,
                                    CspColorMath::Mode mode, int maxSamples)
{
    return sampleImpl(device, rect, nullptr, mask, mode, maxSamples);
}

void CspColorMixer::layDown(KisPainter *painter, const QRect &rect, KisFixedPaintDeviceSP mask,
                            const KoColor &paint, qreal strength, CspColorMath::Mode mode)
{
    if (mode == CspColorMath::Mode::Perceptual && CspColorMath::hasSpectral()) {
        // each mirrored copy mixes with its own canvas, so renderMirrorMask can't be used
        const QString compositeOpId = painter->compositeOpId();
        painter->setCompositeOpId(COMPOSITE_COPY);
        painter->setFlow(1.0);

        KisFixedPaintDeviceSP m = new KisFixedPaintDevice(*mask);
        bool flippedH = false, flippedV = false;
        for (const MirrorCopy &copy : mirrorCopies(painter, rect)) {
            if (copy.flipHorizontally != flippedH || copy.flipVertically != flippedV) {
                m->mirror(copy.flipHorizontally != flippedH, copy.flipVertically != flippedV);
                flippedH = copy.flipHorizontally;
                flippedV = copy.flipVertically;
            }
            m->setRect(copy.rect);
            layDownPerceptual(painter, copy.rect, m, paint, strength);
        }
        painter->setCompositeOpId(compositeOpId);
        return;
    }

    const KoColorSpace *cs = painter->device()->colorSpace();
    KisFixedPaintDeviceSP paintDab = new KisFixedPaintDevice(cs);
    paintDab->setRect(rect);
    paintDab->lazyGrowBufferWithoutInitialization();
    paintDab->fill(rect, paint.convertedTo(cs));

    // COPY with mask and opacity is lerp(dst, src, mask * opacity), alpha included
    const QString compositeOpId = painter->compositeOpId();
    painter->setCompositeOpId(COMPOSITE_COPY);
    painter->setOpacityF(qBound(0.0, strength, 1.0));
    painter->setFlow(1.0);

    painter->bltFixedWithFixedSelection(rect.x(), rect.y(), paintDab, mask,
                                        rect.x(), rect.y(), rect.x(), rect.y(),
                                        rect.width(), rect.height());
    painter->renderMirrorMaskSafe(rect, paintDab, mask, false);

    painter->setCompositeOpId(compositeOpId);
}
