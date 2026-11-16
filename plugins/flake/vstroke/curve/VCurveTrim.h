/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_TRIM_H
#define VCURVE_TRIM_H

#include <QPolygonF>
#include <QVector>

#include <functional>

#include "VCurveData.h"
#include "kritavstroke_export.h"

/**
 * Trim, as Blender's Grease Pencil trim (grease_pencil_trim.cc):
 * 1. find every crossing of the centerlines, self-crossings included
 *    (but loops hidden under the stroke's width don't count), and where
 *    another stroke ends on a stroke with its ink, its centerline short;
 * 2. cut the strokes there (beziers with de Casteljau, values
 *    interpolated);
 * 3. delete the pieces the drag, lasso or click touches (a drag or click
 *    touches the ink, a lasso the centerline);
 * 4. join the kept pieces that meet again;
 * 5. cut ends are flat, unless caps are kept.
 *
 * Two other ways to erase (Options::erase) skip the crossings: the whole
 * stroke (sub-path) the drag, lasso or click touches goes, or only the
 * part of it inside the area (by point): the points inside go, and new
 * points cut the line where it leaves the area.
 */
namespace VCurveTrim
{
struct Selector {
    enum Mode { Drag, Lasso, Click };
    Mode mode = Drag;
    QPolygonF path;       ///< the drag path or the lasso, in document units
    qreal radius = 1.0;   ///< Drag: the brush radius; Click: how near the click must be
    QPointF point;        ///< Click
};

struct Options {
    enum Erase {
        ToCrossings,  ///< the pieces between crossings
        WholeStroke,  ///< the whole sub-path
        Area          ///< only the part inside the area; a click erases as a drag of one point
    };
    Erase erase = ToCrossings;
    bool keepCaps = false;  ///< cut ends keep the shape's cap
    bool endsOnly = false;  ///< ToCrossings: only the first and last piece of an open stroke may go
    qreal tolerance = 0.05; ///< bezier flattening for the crossings, in document units
    qreal leeway = 0.0;     ///< ToCrossings: a stroke ending this far short of another's ink still cuts it, in document units
};

/// a cut at segment + t of a stroke
struct Cut {
    int segment;
    qreal t;
};

/**
 * The crossings of each target with itself, the other targets and
 * \p cutters, sorted along the stroke, and where another stroke's end
 * reaches its ink (or comes within \p leeway of it). Crossings on an open
 * stroke's ends, or nearer them than the two inks are wide, don't count;
 * strokes shorter than they are wide (dots, pen tips) neither cut nor are cut.
 */
KRITAVSTROKE_EXPORT QVector<QVector<Cut>> crossings(const QVector<VDocStroke> &targets,
                                                    const QVector<VCurveData> &cutters, qreal tolerance,
                                                    qreal leeway = 0.0);

/**
 * Trims \p targets (document units). Strokes that lose nothing come back
 * as they were; the others come back as their kept parts, marked changed.
 */
KRITAVSTROKE_EXPORT QVector<VDocStroke> trim(const QVector<VDocStroke> &targets, const QVector<VCurveData> &cutters,
                                             const Selector &selector, const Options &options = Options());

/**
 * Where \p curve (one sub-path) is inside the area of \p selector (Drag:
 * within the radius of the path; Lasso: inside it; Click: within the
 * radius of the point), as sorted, separate ranges of segment positions
 * (segment + t). A single point gives the range 0-0 when inside.
 */
KRITAVSTROKE_EXPORT QVector<QPair<qreal, qreal>> insideRanges(const VCurveData &curve, const Selector &selector,
                                                             qreal tolerance);

/**
 * The parts of \p curve (one sub-path) where \p inside holds, checked every
 * \p step along the line. Comes back as it was when all of it does; cut ends
 * are flat, and pieces shorter than they are wide go, as with Trim.
 */
KRITAVSTROKE_EXPORT QVector<VCurveData> keepInside(const VCurveData &curve,
                                                   const std::function<bool(const QPointF &)> &inside, qreal step);

/// the part of \p curve (one sub-path) between segment positions \p from and \p to (segment + t; past the end wraps on closed ones)
KRITAVSTROKE_EXPORT VCurveData extract(const VCurveData &curve, qreal from, qreal to);
}

#endif // VCURVE_TRIM_H
