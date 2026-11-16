/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_vector_deform_maps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include <QMutex>
#include <QPolygonF>
#include <QtMath>

#include <KisBezierPatch.h>
#include <KisBezierTransformMesh.h>
#include <kis_green_coordinates_math.h>

namespace {

using DeformMap = KisShapeLayerHooks::DeformMap;

DeformMap identityMap()
{
    DeformMap result;
    result.map = [] (const QPointF &p) { return p; };
    result.mapAll = [] (const QVector<QPointF> &points, QVector<QPointF> &mapped) { mapped = points; };
    result.identity = true;
    return result;
}

/// a map's batch form, point by point
DeformMap withBatch(DeformMap map)
{
    const std::function<QPointF(const QPointF &)> one = map.map;
    map.mapAll = [one] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
        mapped.resize(points.size());
        for (int i = 0; i < points.size(); i++) mapped[i] = one(points[i]);
    };
    return map;
}

QPointF bezier(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t)
{
    const qreal s = 1 - t;
    return s * s * s * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t * p3;
}

// ---- Mesh

/// samples per patch edge for its length fractions
const int EdgeSamples = 128;
using Fractions = std::array<qreal, EdgeSamples + 1>;

/// how far along the curve, by length, each of its samples at t = k / EdgeSamples is
Fractions lengthFractions(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3)
{
    Fractions result;
    result[0] = 0;
    QPointF last = p0;
    for (int k = 1; k <= EdgeSamples; k++) {
        const QPointF p = bezier(p0, p1, p2, p3, qreal(k) / EdgeSamples);
        result[k] = result[k - 1] + std::hypot(p.x() - last.x(), p.y() - last.y());
        last = p;
    }
    const qreal length = result[EdgeSamples];
    for (int k = 0; k <= EdgeSamples; k++) result[k] = length > 0 ? result[k] / length : qreal(k) / EdgeSamples;
    return result;
}

/**
 * The t where lerp(\p a(t), \p b(t), \p w) reaches \p target: both rise from 0
 * to 1, sampled. Beyond, it goes on as at the ends.
 */
qreal solveFraction(const Fractions &a, const Fractions &b, qreal w, qreal target)
{
    auto at = [&] (int k) { return a[k] + (b[k] - a[k]) * w; };
    const qreal step = 1.0 / EdgeSamples;
    if (target <= 0) {
        const qreal rise = at(1) - at(0);
        return rise > 1e-12 ? target * step / rise : target;
    }
    if (target >= 1) {
        const qreal rise = at(EdgeSamples) - at(EdgeSamples - 1);
        return rise > 1e-12 ? 1 + (target - 1) * step / rise : target;
    }
    int lo = 0;
    int hi = EdgeSamples;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (at(mid) <= target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const qreal rise = at(hi) - at(lo);
    return (lo + (rise > 1e-12 ? (target - at(lo)) / rise : 0)) * step;
}

struct MeshPatch {
    std::array<QPointF, 12> p;
    QRectF source;
    Fractions top, bottom, left, right;

    explicit MeshPatch(const KisBezierPatch &patch)
        : p(patch.points),
          source(patch.originalRect)
    {
        using P = KisBezierPatch;
        top = lengthFractions(p[P::TL], p[P::TL_HC], p[P::TR_HC], p[P::TR]);
        bottom = lengthFractions(p[P::BL], p[P::BL_HC], p[P::BR_HC], p[P::BR]);
        left = lengthFractions(p[P::TL], p[P::TL_VC], p[P::BL_VC], p[P::BL]);
        right = lengthFractions(p[P::TR], p[P::TR_VC], p[P::BR_VC], p[P::BR]);
    }

    /// as KisBezierPatch::sampleRegularGrid() sends the sample at proportions (\p u, \p v)
    QPointF transformed(qreal u, qreal v) const
    {
        using P = KisBezierPatch;
        const QPointF p0 = bezier(p[P::TL], p[P::TL_VC], p[P::BL_VC], p[P::BL], v);
        const QPointF relP1 = (p[P::TL_HC] - p[P::TL]) * (1 - v) + (p[P::BL_HC] - p[P::BL]) * v;
        const QPointF relP2 = (p[P::TR_HC] - p[P::TR]) * (1 - v) + (p[P::BR_HC] - p[P::BR]) * v;
        const QPointF p3 = bezier(p[P::TR], p[P::TR_VC], p[P::BR_VC], p[P::BR], v);

        const QPointF q0 = bezier(p[P::TL], p[P::TL_HC], p[P::TR_HC], p[P::TR], u);
        const QPointF relQ1 = (p[P::TL_VC] - p[P::TL]) * (1 - u) + (p[P::TR_VC] - p[P::TR]) * u;
        const QPointF relQ2 = (p[P::BL_VC] - p[P::BL]) * (1 - u) + (p[P::BR_VC] - p[P::BR]) * u;
        const QPointF q3 = bezier(p[P::BL], p[P::BL_HC], p[P::BR_HC], p[P::BR], u);

        const QPointF transf1 = bezier(p0, p0 + relP1, p3 + relP2, p3, u);
        const QPointF transf2 = bezier(q0, q0 + relQ1, q3 + relQ2, q3, v);
        return 0.5 * (transf1 + transf2);
    }

    /**
     * Where \p point of the source goes. The grid places the sample of
     * proportions (u, v) at (lerp(top(u), bottom(u), v), lerp(left(v), right(v), u))
     * in the source rect: that is solved for (u, v), one by the other in turn.
     */
    QPointF map(const QPointF &point) const
    {
        const qreal x = source.width() > 0 ? (point.x() - source.left()) / source.width() : 0;
        const qreal y = source.height() > 0 ? (point.y() - source.top()) / source.height() : 0;
        qreal u = x;
        qreal v = y;
        for (int i = 0; i < 8; i++) {
            const qreal lastU = u;
            const qreal lastV = v;
            u = solveFraction(top, bottom, qBound(0.0, v, 1.0), x);
            v = solveFraction(left, right, qBound(0.0, u, 1.0), y);
            if (qAbs(u - lastU) + qAbs(v - lastV) < 1e-9) break;
        }
        return transformed(u, v);
    }
};

struct MeshMath {
    std::vector<MeshPatch> patches;
    std::vector<qreal> columns; ///< the patches' left edges, then the last one's right
    std::vector<qreal> rows;
    int columnCount = 0;

    QPointF map(const QPointF &p) const
    {
        if (patches.empty()) return p;
        // the patch over p; beyond the mesh, the one at its edge
        const auto column = std::upper_bound(columns.begin() + 1, columns.end() - 1, p.x()) - (columns.begin() + 1);
        const auto row = std::upper_bound(rows.begin() + 1, rows.end() - 1, p.y()) - (rows.begin() + 1);
        return patches[row * columnCount + column].map(p);
    }
};

// ---- Cage

struct CageMath {
    QVector<QPointF> original;
    QVector<QPointF> transformed;
    QPolygonF polygon;

    bool isInside(const QPointF &p) const { return polygon.containsPoint(p, Qt::OddEvenFill); }

    /// a point outside the cage: as the nearest edge moves it (the similarity taking it to its new place)
    QPointF outside(const QPointF &p) const
    {
        const int n = original.size();
        int nearest = 0;
        qreal nearestDistance = std::numeric_limits<qreal>::max();
        for (int i = 0; i < n; i++) {
            const QPointF a = original[i];
            const QPointF d = original[(i + 1) % n] - a;
            const qreal length2 = QPointF::dotProduct(d, d);
            const qreal t = length2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, d) / length2, 1.0) : 0;
            const QPointF off = p - (a + d * t);
            const qreal distance = QPointF::dotProduct(off, off);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = i;
            }
        }
        const QPointF a = original[nearest];
        const QPointF b = original[(nearest + 1) % n];
        const QPointF a2 = transformed[nearest];
        const QPointF b2 = transformed[(nearest + 1) % n];
        const QPointF d = b - a;
        const qreal length2 = QPointF::dotProduct(d, d);
        if (length2 <= 0) return p + (a2 - a);
        // z = (p - a) / (b - a), as complex numbers; then a2 + z (b2 - a2)
        const QPointF r = p - a;
        const QPointF z((r.x() * d.x() + r.y() * d.y()) / length2, (r.y() * d.x() - r.x() * d.y()) / length2);
        const QPointF d2 = b2 - a2;
        return a2 + QPointF(z.x() * d2.x() - z.y() * d2.y(), z.x() * d2.y() + z.y() * d2.x());
    }

    /**
     * Green coordinates divide by zero for a point on the line of an edge: such
     * a point moves by a hair off it.
     */
    QPointF stable(const QPointF &p) const
    {
        const int n = original.size();
        for (int i = 0; i < n; i++) {
            const QPointF a = original[(i + 1) % n] - original[i];
            const QPointF b = original[i] - p;
            const qreal cross = a.x() * b.y() - a.y() * b.x();
            if (qAbs(cross) <= 1e-9 * (qAbs(a.x()) + qAbs(a.y())) * (qAbs(b.x()) + qAbs(b.y()) + 1)) {
                return p + QPointF(1e-3, 1.3e-3);
            }
        }
        return p;
    }
};

struct CagePrepared {
    QVector<QPointF> original;
    QVector<QPointF> points;
    QVector<int> green;         ///< per point: its index among the Green coordinates, or -1 outside the cage
    KisGreenCoordinatesMath math;
    QVector<QPointF> lastCage;  ///< the transformed cage the math's normals are for
    QMutex mutex;
};

std::shared_ptr<CagePrepared> prepareCage(const CageMath &cage, const QVector<QPointF> &points)
{
    std::shared_ptr<CagePrepared> result = std::make_shared<CagePrepared>();
    result->original = cage.original;
    result->points = points;
    result->green.fill(-1, points.size());
    QVector<QPointF> inside;
    for (int i = 0; i < points.size(); i++) {
        if (!cage.isInside(points[i])) continue;
        result->green[i] = inside.size();
        inside << cage.stable(points[i]);
    }
    result->math.precalculateGreenCoordinates(cage.original, inside);
    return result;
}

void mapPreparedCage(const CageMath &cage, CagePrepared &prepared, QVector<QPointF> &mapped)
{
    QMutexLocker locker(&prepared.mutex);
    if (prepared.lastCage != cage.transformed) {
        prepared.math.generateTransformedCageNormals(cage.transformed);
        prepared.lastCage = cage.transformed;
    }
    mapped.resize(prepared.points.size());
    for (int i = 0; i < prepared.points.size(); i++) {
        const int index = prepared.green[i];
        mapped[i] = index >= 0 ? prepared.math.transformedPoint(index, cage.transformed) : cage.outside(prepared.points[i]);
    }
}

}

KisShapeLayerHooks::DeformMap KisVectorDeformMaps::warp(KisWarpTransformWorker::WarpType type, const QVector<QPointF> &origPoints,
                                                        const QVector<QPointF> &transfPoints, qreal alpha)
{
    if (origPoints.isEmpty() || origPoints.size() != transfPoints.size() || origPoints == transfPoints) return identityMap();

    DeformMap result;
    if (origPoints.size() == 1) {
        const QPointF offset = transfPoints[0] - origPoints[0];
        result.map = [offset] (const QPointF &p) { return p + offset; };
        return withBatch(result);
    }

    using Math = QPointF (*)(QPointF, QVector<QPointF>, QVector<QPointF>, qreal);
    Math math = nullptr;
    switch (type) {
    case KisWarpTransformWorker::AFFINE_TRANSFORM: math = &KisWarpTransformWorker::affineTransformMath; break;
    case KisWarpTransformWorker::SIMILITUDE_TRANSFORM: math = &KisWarpTransformWorker::similitudeTransformMath; break;
    case KisWarpTransformWorker::RIGID_TRANSFORM: math = &KisWarpTransformWorker::rigidTransformMath; break;
    default: return identityMap();
    }
    result.map = [math, origPoints, transfPoints, alpha] (const QPointF &p) { return math(p, origPoints, transfPoints, alpha); };
    return withBatch(result);
}

KisShapeLayerHooks::DeformMap KisVectorDeformMaps::mesh(const KisBezierTransformMesh &mesh)
{
    if (mesh.isIdentity()) return identityMap();
    std::shared_ptr<MeshMath> math = std::make_shared<MeshMath>();
    const QSize size = mesh.size();
    const int columns = size.width() - 1;
    const int rows = size.height() - 1;
    if (columns < 1 || rows < 1) return identityMap();
    math->columnCount = columns;
    math->patches.reserve(columns * rows);
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            math->patches.emplace_back(mesh.makePatch(column, row));
        }
    }
    for (int column = 0; column < columns; column++) math->columns.push_back(math->patches[column].source.left());
    math->columns.push_back(math->patches[columns - 1].source.right());
    for (int row = 0; row < rows; row++) math->rows.push_back(math->patches[row * columns].source.top());
    math->rows.push_back(math->patches[(rows - 1) * columns].source.bottom());

    DeformMap result;
    result.map = [math] (const QPointF &p) { return math->map(p); };
    return withBatch(result);
}

KisShapeLayerHooks::DeformMap KisVectorDeformMaps::cage(const QVector<QPointF> &origPoints, const QVector<QPointF> &transfPoints,
                                                        bool editing)
{
    const bool closed = origPoints.size() >= 3;
    if (editing || !closed || origPoints.size() != transfPoints.size()) {
        DeformMap result = identityMap();
        // what moves once the cage deforms: a session cut for another cage starts over
        if (closed) result.inside = QPolygonF(origPoints);
        return result;
    }

    std::shared_ptr<CageMath> cage = std::make_shared<CageMath>();
    cage->original = origPoints;
    cage->transformed = transfPoints;
    cage->polygon = QPolygonF(origPoints);

    DeformMap result;
    result.inside = cage->polygon;
    result.map = [cage] (const QPointF &p) {
        if (!cage->isInside(p)) return cage->outside(p);
        KisGreenCoordinatesMath math;
        math.precalculateGreenCoordinates(cage->original, {cage->stable(p)});
        math.generateTransformedCageNormals(cage->transformed);
        return math.transformedPoint(0, cage->transformed);
    };
    result.prepare = [cage] (const QVector<QPointF> &points) -> std::shared_ptr<void> {
        return prepareCage(*cage, points);
    };
    result.mapPrepared = [cage] (const std::shared_ptr<void> &prepared, QVector<QPointF> &mapped) {
        CagePrepared *p = static_cast<CagePrepared*>(prepared.get());
        if (!p || p->original != cage->original) return false;
        mapPreparedCage(*cage, *p, mapped);
        return true;
    };
    result.mapAll = [cage] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
        std::shared_ptr<CagePrepared> prepared = prepareCage(*cage, points);
        mapPreparedCage(*cage, *prepared, mapped);
    };
    return result;
}
