/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_TIP_SET_H
#define CSP_TIP_SET_H

#include <QVector>
#include <array>
#include <memory>
#include <vector>

#include <KoColor.h>
#include <kis_brush.h>
#include <kis_random_source.h>

#include "CspBrushParams.h"

class KisDabCache;

/**
 * The brush tips of one stroke: a Krita brush (and dab cache) per CSP
 * tip, the choice of tip per dab following CSP's repeat methods, and the
 * main/sub color mapping of gray tips (black -> main, white -> sub,
 * grays mixed with the brush's mixing mode) done by Krita's GRADIENTMAP
 * brush application.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspTipSet
{
public:
    struct Variant {
        KisBrushSP brush;
        std::shared_ptr<KisDabCache> dabCache;
        qreal longestSide {1.0}; ///< tip px that equal the brush size
    };

    explicit CspTipSet(const CspBrushParams &params);
    ~CspTipSet();

    bool isValid() const { return !m_tips.isEmpty(); }
    int count() const { return m_tips.size(); }

    /// Index of the tip for the next dab, -1 when the brush stops painting
    /// ("One time only" after the last tip)
    int nextTipIndex(KisRandomSourceSP random);

    /// Brush of tip @p index, flipped as requested
    Variant &variant(int index, bool flipH, bool flipV);

    /// Gray tips paint main..sub; call when the colors change
    void setColors(const KoColor &main, const KoColor &sub, CspColorMath::Mode mode);

    /// Unscaled image of tip @p index (for the ribbon sampler)
    const QImage &tipImage(int index) const;

    /**
     * Tip @p index as pixels of @p cs (width x height, row-major): gray tips
     * mapped main..sub, color tips converted. Valid after setColors().
     */
    const QByteArray &colorizedTip(int index, const KoColorSpace *cs);

private:
    Variant makeVariant(int index, bool flipH, bool flipV) const;

    CspBrushParams m_params;
    QVector<CspBrushParams::Tip> m_tips; ///< empty image = circle tip
    std::vector<std::array<std::unique_ptr<Variant>, 4>> m_variants; ///< [tip][flipH + 2 * flipV]
    KoAbstractGradientSP m_gradient;
    QVector<KoColor> m_lut; ///< gray 0..255 -> color
    QVector<QByteArray> m_colorized;
    const KoColorSpace *m_colorizedSpace {nullptr};
    QVector<int> m_order;
    int m_counter {0};
    KoColor m_main;
    KoColor m_sub;
};

#endif // CSP_TIP_SET_H
