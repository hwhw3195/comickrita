/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_WARP_H
#define VCURVE_WARP_H

#include <QFlags>
#include <QTransform>
#include <QVector>

#include "VCurveData.h"
#include "VFalloff.h"
#include "kritavstroke_export.h"

/**
 * Edits that move existing points and change their values, never adding or
 * removing any (as Blender's Grease Pencil sculpt and smooth).
 */
namespace VCurveWarp
{
enum class Affect {
    Position = 1,
    Radius = 2,
    Pressure = 4
};
Q_DECLARE_FLAGS(AffectFlags, Affect)

/**
 * The brush weight of each point: the falloff of its distance (positions
 * mapped by \p toDocument) to \p center, 0 from \p radius on.
 */
KRITAVSTROKE_EXPORT QVector<qreal> brushWeights(const VCurveData &curves, const QTransform &toDocument,
                                                const QPointF &center, qreal radius,
                                                VFalloff::Profile profile = VFalloff::Profile::Smooth);

/**
 * Blender's smooth: \p iterations times, each point moves toward the mean
 * of its two neighbours by weight * \p strength of the way (all points from
 * the values of the previous iteration). An empty \p weights means 1
 * everywhere.
 *
 * Each bezier keeps its shape, scaled and turned as its chord was, and the
 * handles of a moved point stay in line, so sparse beziers don't bend into
 * ridges. The ends of open sub-paths and corners (a point whose two bezier
 * handles aren't in line) don't move; their radius and pressure still
 * smooth, and their handles keep their directions.
 */
KRITAVSTROKE_EXPORT void smooth(VCurveData &curves, const QVector<qreal> &weights, qreal strength, int iterations,
                                AffectFlags affect = Affect::Position);

/**
 * The Smooth of the tools: each point moves toward a Gaussian average of
 * the line around it, by weight * \p strength of the way, \p iterations
 * times. Unlike smooth(), the average is taken by distance along the line
 * (\p distance, in the curves' units, is about how far it reaches: sigma
 * is half of it), so a line smooths the same however densely its points
 * lie. Blender's Grease Pencil smooth averages a number of neighbours by
 * index instead, which still depends on the density.
 *
 * \p keepShape averages with 1.5 G(sigma) - 0.5 G(sigma * sqrt 3), after
 * Blender's keep shape: wobbles shorter than the distance still go, but
 * curves much longer than it hardly shrink (no change to first order; a
 * plain Gaussian shrinks them to first order). Blender's own weights,
 * 2 G(sigma) - G(sigma * sqrt 3), would swell them a little instead.
 *
 * The ends of open sub-paths and corners stay, as with smooth(), and the
 * average doesn't reach past them: the line beyond one is mirrored
 * through it, so a straight line stays straight up to it and an end
 * keeps its direction. Beziers and handles follow as with smooth().
 */
KRITAVSTROKE_EXPORT void smoothAlong(VCurveData &curves, const QVector<qreal> &weights, qreal strength, qreal distance,
                                     int iterations, AffectFlags affect = Affect::Position, bool keepShape = false);

/// whether point \p index is a corner smooth keeps in place
KRITAVSTROKE_EXPORT bool isCorner(const VCurveData &curves, int index);

/// a Move or Offset dab of the transform tool's Liquify, in image pixels
struct Dab {
    QPointF position;
    QPointF offset;
    qreal sigma = 1.0;
    bool wash = false;
    qreal flow = 1.0;

    bool operator==(const Dab &rhs) const {
        return position == rhs.position && offset == rhs.offset && sigma == rhs.sigma &&
               wash == rhs.wash && flow == rhs.flow;
    }
};

/**
 * Moves every position and bezier handle as Liquify's grid worker moves
 * its points (KisLiquifyTransformWorker::translatePoints): by
 * exp(-d² / 2σ²) of the offset, up to 3σ. Build up measures d from where
 * a point is now; wash measures it from where it was in \p snapshot and
 * moves the point toward snapshot + offset by the flow, only farther out.
 * \p toImage maps local positions to image pixels.
 */
KRITAVSTROKE_EXPORT void translate(VCurveData &curves, const Dab &dab, const QTransform &toImage,
                                   const VCurveData &snapshot);

/// \p snapshot moved by every dab in turn
KRITAVSTROKE_EXPORT VCurveData translated(const VCurveData &snapshot, const QVector<Dab> &dabs,
                                          const QTransform &toImage);

/**
 * Moves \p curves on by the dabs from index \p from, as translated() does:
 * translated(snapshot, dabs) equals translated(snapshot, the first \p from
 * dabs) moved on by this, exactly. The live Liquify preview applies only
 * the dabs added since its last update.
 */
KRITAVSTROKE_EXPORT void translateFrom(VCurveData &curves, const QVector<Dab> &dabs, int from,
                                       const QTransform &toImage, const VCurveData &snapshot);
}

Q_DECLARE_OPERATORS_FOR_FLAGS(VCurveWarp::AffectFlags)

#endif // VCURVE_WARP_H
