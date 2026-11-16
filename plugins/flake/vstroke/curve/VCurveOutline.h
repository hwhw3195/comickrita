/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_OUTLINE_H
#define VCURVE_OUTLINE_H

#include <QPainterPath>

#include "kritavstroke_export.h"

class VCurveData;

/// how the line of a curve turns corners and ends, as in SVG
struct KRITAVSTROKE_EXPORT VCurveStrokeStyle
{
    Qt::PenJoinStyle join = Qt::RoundJoin;
    Qt::PenCapStyle cap = Qt::RoundCap;
    qreal miterLimit = 4.0;        ///< as SVG stroke-miterlimit: miter length over line width

    bool operator==(const VCurveStrokeStyle &rhs) const {
        return join == rhs.join && cap == rhs.cap && miterLimit == rhs.miterLimit;
    }
};

namespace VCurveOutline
{
/**
 * The area the line of \p curves covers, with the width following the
 * radius of each point: one quad per segment (beziers flattened to within
 * \p tolerance), joins at the points and caps at the ends of open
 * sub-paths. Joins inside a flattened bezier are round for round joins and
 * beveled otherwise. The pieces wind the same way and are united by the
 * winding fill rule.
 *
 * Radii are multiplied by \p radiusScale (to the units of the positions)
 * and raised to \p minRadius.
 */
KRITAVSTROKE_EXPORT QPainterPath line(const VCurveData &curves, const VCurveStrokeStyle &style,
                                      qreal radiusScale = 1.0, qreal minRadius = 0.0, qreal tolerance = 0.1);

/**
 * The area a fill covers: each cyclic sub-path closed on its own, and the
 * open sub-paths joined in order into one area closed by a straight line,
 * so a closed shape cut open by a trim keeps its fill.
 */
KRITAVSTROKE_EXPORT QPainterPath fill(const VCurveData &curves, Qt::FillRule fillRule);
}

#endif // VCURVE_OUTLINE_H
