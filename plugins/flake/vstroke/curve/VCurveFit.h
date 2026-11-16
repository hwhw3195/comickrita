/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_FIT_H
#define VCURVE_FIT_H

#include <QHash>
#include <QPointF>
#include <QVector>

#include "kritavstroke_export.h"

/**
 * Geometry helpers for building one clean line out of many points: a point
 * grid for neighbour queries, a spanning tree over the k nearest
 * neighbours, the tree's longest path, and Schneider's cubic bezier fit
 * ("An Algorithm for Automatically Fitting Digitized Curves", Graphics
 * Gems, 1990).
 *
 * NijiGPen's line fit (a Blender add-on, GPL-3) does the same with scipy
 * and a Delaunay triangulation; this is a reimplementation from its idea,
 * with no code taken from it.
 */
namespace VCurveFit
{
/// a uniform grid of points, for "which points are near here"
class KRITAVSTROKE_EXPORT PointGrid
{
public:
    PointGrid(const QVector<QPointF> &points, qreal cellSize);

    /// the indexes of the points within \p radius of \p center
    QVector<int> near(const QPointF &center, qreal radius) const;
    /// the \p k points nearest to point \p index, nearest first, without itself
    QVector<int> nearest(int index, int k) const;

private:
    QPoint cellOf(const QPointF &p) const;
    static quint64 key(const QPoint &cell);

    QVector<QPointF> m_points;
    qreal m_cellSize;
    QHash<quint64, QVector<int>> m_cells;
};

/**
 * A minimum spanning tree of \p points over the graph linking each point to
 * its \p k nearest neighbours. Parts the graph leaves apart are linked by
 * their nearest pair, so the result is one tree. Returned as adjacency
 * lists.
 */
KRITAVSTROKE_EXPORT QVector<QVector<int>> spanningTree(const QVector<QPointF> &points, int k = 8);

/// the longest path through \p tree (by edge length), as point indexes
KRITAVSTROKE_EXPORT QVector<int> longestPath(const QVector<QVector<int>> &tree, const QVector<QPointF> &points);

struct Cubic {
    QPointF p0, c1, c2, p1;
};

struct FitResult {
    QVector<Cubic> cubics;
    /// for each end of a cubic (cubics.size() + 1 of them), the input point it sits on
    QVector<int> knots;
};

/// cubic beziers through \p points, each within \p maxError of the points it covers
KRITAVSTROKE_EXPORT FitResult fitCubics(const QVector<QPointF> &points, qreal maxError);
}

#endif // VCURVE_FIT_H
