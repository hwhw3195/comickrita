/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspTipSet.h"

#include <algorithm>

#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoStopGradient.h>
#include <kis_auto_brush.h>
#include <kis_circle_mask_generator.h>
#include <kis_dab_cache.h>
#include <kis_gbr_brush.h>

namespace {
const int gradientStops = 33;
}

CspTipSet::CspTipSet(const CspBrushParams &params)
    : m_params(params)
{
    if (params.useMaterial && !params.tips.isEmpty()) {
        m_tips = params.tips;
    } else {
        m_tips << CspBrushParams::Tip(); // circle
    }
    m_variants.resize(m_tips.size());
}

CspTipSet::~CspTipSet()
{
}

int CspTipSet::nextTipIndex(KisRandomSourceSP random)
{
    const int n = m_tips.size();
    const int k = m_counter++;
    if (n == 1) {
        return m_params.repeatMethod == CspBrushParams::OneTimeOnly && k > 0 ? -1 : 0;
    }

    switch (m_params.repeatMethod) {
    case CspBrushParams::Reverse: {
        const int period = 2 * n - 2;
        const int i = k % period;
        return i < n ? i : period - i;
    }
    case CspBrushParams::NoRepeat:
        return qMin(k, n - 1);
    case CspBrushParams::RandomOrder:
        return random ? random->generate(0, n - 1) : k % n;
    case CspBrushParams::OneTimeOnly:
        return k < n ? k : -1;
    case CspBrushParams::OneRandomCycle:
        if (m_order.isEmpty()) {
            for (int i = 0; i < n; i++) m_order << i;
            for (int i = n - 1; i > 0 && random; i--) std::swap(m_order[i], m_order[random->generate(0, i)]);
        }
        return k < n ? m_order[k] : -1;
    case CspBrushParams::Repeat:
    default:
        return k % n;
    }
}

CspTipSet::Variant &CspTipSet::variant(int index, bool flipH, bool flipV)
{
    std::unique_ptr<Variant> &slot = m_variants[index][(flipH ? 1 : 0) + (flipV ? 2 : 0)];
    if (!slot) {
        slot.reset(new Variant(makeVariant(index, flipH, flipV)));
        if (m_gradient && slot->brush->applyingGradient()) {
            slot->brush->setGradient(m_gradient);
        }
    }
    return *slot;
}

const QImage &CspTipSet::tipImage(int index) const
{
    return m_tips[index].image;
}

CspTipSet::Variant CspTipSet::makeVariant(int index, bool flipH, bool flipV) const
{
    Variant v;
    const CspBrushParams::Tip &tip = m_tips[index];

    if (tip.image.isNull()) {
        const bool antialias = m_params.antiAlias > 0;
        KisCircleMaskGenerator *mask =
            new KisCircleMaskGenerator(m_params.size, 1.0, m_params.hardness, m_params.hardness, 2, antialias);
        v.brush = KisBrushSP(new KisAutoBrush(mask, 0.0, 0.0));
        v.longestSide = m_params.size;
    } else {
        const QImage image = tip.image.mirrored(flipH, flipV);
        KisGbrBrush *brush = new KisGbrBrush(image, tip.name);
        brush->setBrushApplication(tip.isGray ? GRADIENTMAP : IMAGESTAMP);
        v.brush = KisBrushSP(brush);
        v.longestSide = qMax(image.width(), image.height());
    }

    v.brush->setSpacing(m_params.spacingFraction());
    v.brush->notifyStrokeStarted();
    v.dabCache = std::make_shared<KisDabCache>(v.brush);
    return v;
}

void CspTipSet::setColors(const KoColor &main, const KoColor &sub, CspColorMath::Mode mode)
{
    if (m_gradient && main == m_main && sub == m_sub) return;
    m_main = main;
    m_sub = sub;

    const KoColor &black = m_params.swapMainSub ? sub : main;
    const KoColor &white = m_params.swapMainSub ? main : sub;
    // gray tips keep a perceptual gradient: spectral mixing with black would darken mid grays
    if (mode == CspColorMath::Mode::Perceptual) mode = CspColorMath::Mode::Oklab;

    QList<KoGradientStop> stops;
    for (int i = 0; i < gradientStops; i++) {
        const qreal t = qreal(i) / (gradientStops - 1);
        stops << KoGradientStop(t, CspColorMath::mix(black, white, t, mode), COLORSTOP);
    }
    m_lut.resize(256);
    for (int g = 0; g < 256; g++) {
        m_lut[g] = CspColorMath::mix(black, white, g / 255.0f, mode);
    }
    m_colorized.clear();
    QSharedPointer<KoStopGradient> gradient(new KoStopGradient());
    gradient->setColorSpace(const_cast<KoColorSpace *>(main.colorSpace()));
    gradient->setStops(stops);
    m_gradient = gradient;

    for (auto &variants : m_variants) {
        for (auto &slot : variants) {
            if (slot && slot->brush->applyingGradient()) slot->brush->setGradient(m_gradient);
        }
    }
}

const QByteArray &CspTipSet::colorizedTip(int index, const KoColorSpace *cs)
{
    if (m_colorizedSpace != cs) {
        m_colorized.clear();
        m_colorizedSpace = cs;
    }
    if (m_colorized.size() != m_tips.size()) m_colorized = QVector<QByteArray>(m_tips.size());

    QByteArray &buffer = m_colorized[index];
    if (!buffer.isEmpty()) return buffer;

    const CspBrushParams::Tip &tip = m_tips[index];
    const int w = tip.image.width();
    const int h = tip.image.height();
    const int pixelSize = cs->pixelSize();
    buffer = QByteArray(w * h * pixelSize, 0);
    quint8 *dst = reinterpret_cast<quint8 *>(buffer.data());

    if (tip.isGray && !m_lut.isEmpty()) {
        QVector<KoColor> lut(256);
        for (int g = 0; g < 256; g++) {
            lut[g] = m_lut[g].convertedTo(cs);
        }
        for (int y = 0; y < h; y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(tip.image.constScanLine(y));
            for (int x = 0; x < w; x++, dst += pixelSize) {
                const QRgb c = line[x];
                memcpy(dst, lut[qRed(c)].data(), pixelSize);
                cs->setOpacity(dst, quint8(qAlpha(c)), 1);
            }
        }
    } else {
        // ARGB32 in memory is B, G, R, A: Krita's 8-bit sRGB layout
        const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
        srgb->convertPixelsTo(tip.image.constBits(), dst, cs, w * h,
                              KoColorConversionTransformation::internalRenderingIntent(),
                              KoColorConversionTransformation::internalConversionFlags());
    }
    return buffer;
}
