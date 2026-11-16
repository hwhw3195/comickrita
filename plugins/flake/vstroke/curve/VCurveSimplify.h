/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_SIMPLIFY_H
#define VCURVE_SIMPLIFY_H

#include <QPointF>
#include <QTransform>
#include <QVector>

#include <functional>

#include "VCurveData.h"
#include "kritavstroke_export.h"

/**
 * Fewer points: thinning within a tolerance, joining strokes whose ends
 * meet, and merging overlapping strokes into one.
 *
 * The two joins follow NijiGPen (a Blender Grease Pencil add-on): "Join
 * ends" its Pinch Together with Join Strokes, "Merge overlapping" its line
 * fit. NijiGPen is GPL-3, so these are reimplemented from its logic; no
 * code is taken from it.
 */
namespace VCurveSimplify
{
/**
 * Ramer–Douglas–Peucker over the segments: a run of points becomes one
 * segment while the old line stays within \p tolerance of it (measured
 * after mapping by \p toDocument). Runs of lines become a line; runs with
 * beziers a bezier along the end tangents. A point also stays when the
 * line through its neighbours misses its radius by more than
 * \p attributeTolerance of it, or its pressure, tilt or other values by
 * more than \p attributeTolerance; 0 leaves the values out.
 *
 * Ends, corners and dab points always stay. With \p mask (one per point),
 * only points whose flag is set go, and the first and last point of each
 * run of set flags stay, so nothing outside the run changes.
 */
KRITAVSTROKE_EXPORT VCurveData thin(const VCurveData &curves, const QTransform &toDocument, qreal tolerance,
                                    qreal attributeTolerance = 0.1, const QVector<bool> &mask = QVector<bool>());

/**
 * Clip Studio's simplify: no tolerance, each call takes \p share (0 to 1)
 * of the points that may go, the one that moves the line least first
 * (from the line as it is now), so repeated calls go all the way down to
 * the ends. Corners may go; the ends of open sub-paths and dab points
 * stay, and closed ones keep three points. Once an open sub-path has
 * nothing left to take, its beziers straighten by \p share, into lines.
 *
 * With \p weights (the brush), only points with a weight take part, the
 * ends of each run of them stay, so nothing outside changes, and the
 * share is of the summed weights, at least one point.
 */
KRITAVSTROKE_EXPORT VCurveData progressive(const VCurveData &curves, const QTransform &toDocument, qreal share,
                                           const QVector<qreal> &weights = QVector<qreal>());

/// sub-path \p curves (one, open or closed) in the other direction
KRITAVSTROKE_EXPORT VCurveData reversed(const VCurveData &curves);

/// whether strokes of the two owners may become one
using Compatible = std::function<bool(int ownerA, int ownerB)>;

struct JoinOptions {
    qreal gap = 10.0;             ///< document units: ends this close join
    qreal angleTolerance = 0.785; ///< radians: how far the directions may bend at the join
    /// the part of each stroke, by length, that bends to meet the other; 0 bridges the gap with a line
    qreal transition = 0.1;
};

/**
 * Joins open strokes whose ends are within the gap and continue each
 * other's direction, nearest pairs first; a stroke may be reversed to
 * join. With a transition, each stroke's end part bends to the middle of
 * the gap and the two meeting points become one. The joined stroke has the
 * owner of its first part. \p eligible limits the joins to ends where it
 * says so.
 */
KRITAVSTROKE_EXPORT QVector<VDocStroke> joinEnds(const QVector<VDocStroke> &strokes, const JoinOptions &options,
                                                 const Compatible &compatible,
                                                 const std::function<bool(const QPointF &)> &eligible = {});

struct MergeOptions {
    qreal distance = 6.0;         ///< document units: strokes about this close overlap
    qreal fitError = 1.0;         ///< document units: how close the new line stays to the merged centerline
    qreal maxAngle = 0.785;       ///< radians: overlapping strokes run within this angle of each other

    /// the width of the merged lines: pulled toward a radius, then tapered
    bool adjustWidth = false;
    qreal radius = 2.0;           ///< document units: the radius they are pulled toward, at full pressure (times the pressure)
    qreal bias = 0.0;             ///< 0 to 1: how far, from their averaged radius to \ref radius
    /// the width multiplier along the line, by length from start (0) to end (1); empty: none
    std::function<qreal(qreal)> taper;
};

/**
 * Turns overlapping strokes into one:
 * 1. cluster: strokes link where they run close (mean nearest distance
 *    within \p distance over their shared part) in about the same
 *    direction;
 * 2. order the points of each cluster along a minimum spanning tree's
 *    longest path;
 * 3. move each path point to the middle of the nearby points and average
 *    their radius and values;
 * 4. fit cubic beziers;
 * 5. with \p options.adjustWidth, the radius becomes (1 - bias) times the
 *    averaged radius plus bias times the set radius, times the taper; a
 *    taper gets the line enough points to show (exact subdivision).
 * The new stroke has the owner of the cluster's longest stroke.
 * \p eligible limits the merges to clusters holding a stroke it accepts.
 */
KRITAVSTROKE_EXPORT QVector<VDocStroke> mergeOverlapping(const QVector<VDocStroke> &strokes, const MergeOptions &options,
                                                         const Compatible &compatible,
                                                         const std::function<bool(const VDocStroke &)> &eligible = {});
}

#endif // VCURVE_SIMPLIFY_H
