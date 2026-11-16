/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_RIBBON_BUILDER_H
#define CSP_RIBBON_BUILDER_H

#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QVector>

#include <kis_types.h>
#include <kis_random_source.h>

#include "CspBrushParams.h"

class CspTipSet;
class KoColorSpace;

/**
 * CSP "Ribbon": the brush tip image is stretched along the stroke as a
 * continuous band instead of being stamped.
 *
 * The stroke samples (one per paintAt(), a few px apart) form a quad strip.
 * Edge k crosses the stroke at sample k, from L_k (u = 0) to R_k (u = 1);
 * v is the arc length divided by the length of one tile, so the tip image
 * repeats along the stroke, its top and bottom joining (as CSP's ribbon
 * materials are designed). Pixels are mapped back to (u, v) per quad and
 * the tip is sampled bilinearly, wrapping along the stroke so the seams
 * between tiles stay continuous.
 *
 * Edge k needs the direction towards sample k + 1, so a quad is rendered
 * one sample late. Where the stroke turns tighter than the half width, the
 * inner edge is pinned (it would fold back), which makes a fan.
 *
 * Image orientation at Angle 0: the image's y axis runs along the stroke,
 * the top at the start; its x axis runs from the left to the right side
 * of the stroke direction. Angle turns this in steps of 90 degrees.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspRibbonBuilder
{
public:
    struct Sample {
        QPointF pos;
        qreal brushSize {1.0}; ///< size after dynamics (= the tip's longest side), px
        qreal thickness {1.0}; ///< squash across the stroke
    };

    struct Edge {
        QPointF left;
        QPointF right;
        qreal v {0.0};
    };

    CspRibbonBuilder(const CspBrushParams &params, CspTipSet *tips);

    /// Distance between samples for a brush of @p brushSize px
    static qreal sampleSpacing(qreal brushSize);

    /**
     * Adds a stroke sample. When a quad got finished it is rendered into a
     * new dab of @p cs and true is returned; @p dab and @p rect (canvas
     * position) are set.
     */
    bool addSample(const Sample &sample, const KoColorSpace *cs, KisRandomSourceSP random,
                   KisFixedPaintDeviceSP *dab, QRect *rect);

    /// Edges computed so far (for tests)
    const QVector<Edge> &edges() const { return m_edges; }

    /// Tip shown on tile @p tile, -1 if the brush stopped ("One time only")
    int tipForTile(int tile, KisRandomSourceSP random);

private:
    struct TipFrame {
        int alongPx {1};   ///< tip px along the stroke
        int acrossPx {1};  ///< tip px across the stroke
    };

    TipFrame frame(int tipIndex) const;
    Edge makeEdge(const Sample &at, const QPointF &direction) const;
    qreal tileLength(const Sample &sample, int tipIndex) const;
    void renderQuad(const Edge &a, const Edge &b, const KoColorSpace *cs, KisRandomSourceSP random,
                    KisFixedPaintDeviceSP *dab, QRect *rect);
    bool sampleTip(int tipIndex, bool mirrorAlong, qreal u, qreal v, const KoColorSpace *cs, quint8 *dst);

    CspBrushParams m_params;
    CspTipSet *m_tips;
    int m_quarterTurns {0};
    QVector<Sample> m_samples; ///< the last three samples
    QVector<Edge> m_edges;
    QVector<int> m_tileTips;
    QByteArray m_transparent;
};

#endif // CSP_RIBBON_BUILDER_H
