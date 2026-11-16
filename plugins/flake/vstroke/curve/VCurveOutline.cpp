/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveOutline.h"

#include <QPolygonF>
#include <QtMath>

#include <kis_algebra_2d.h>

#include "VCurveData.h"

namespace {

const int DiskSegments = 16;
const int MaxBezierSegments = 128;

struct Vertex {
    QPointF pos;
    qreal radius;
    bool corner;    ///< a point of the curve, not one from flattening a bezier
};

/**
 * Adds a polygon with positive signed area, so that all pieces of the
 * outline wind the same way and the nonzero fill rule gives their union.
 */
void addPositivePolygon(QPainterPath &path, QPolygonF polygon)
{
    qreal area = 0;
    for (int i = 0; i < polygon.size(); i++) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    if (qAbs(area) < 1e-12) return;
    if (area < 0) {
        std::reverse(polygon.begin(), polygon.end());
    }
    path.addPolygon(polygon);
    path.closeSubpath();
}

void addDisk(QPainterPath &path, const QPointF &center, qreal radius)
{
    if (radius <= 0) return;
    QPolygonF polygon;
    for (int i = 0; i < DiskSegments; i++) {
        const qreal angle = 2 * M_PI * i / DiskSegments;
        polygon << center + radius * QPointF(qCos(angle), qSin(angle));
    }
    addPositivePolygon(path, polygon);
}

QPointF normal(const QPointF &direction)
{
    return QPointF(-direction.y(), direction.x());
}

QPointF bezierPoint(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p1, qreal t)
{
    const qreal u = 1.0 - t;
    return u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p1;
}

/// segments for a flattened cubic within tolerance (Wang's formula)
int bezierSegments(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p1, qreal tolerance)
{
    const qreal l = qMax(KisAlgebra2D::norm(p0 - 2 * c1 + c2), KisAlgebra2D::norm(c1 - 2 * c2 + p1));
    const int n = qCeil(qSqrt(0.75 * l / qMax(tolerance, 1e-9)));
    return qBound(1, n, MaxBezierSegments);
}

/// the polyline of one sub-path, beziers flattened, without repeated points
QVector<Vertex> flattenCurve(const VCurveData &d, int curve, qreal radiusScale, qreal minRadius,
                             qreal tolerance, bool closed)
{
    const int begin = d.curveBegin(curve);
    const int end = d.curveEnd(curve);
    auto radius = [&](qreal r) { return qMax(minRadius, r * radiusScale); };

    QVector<Vertex> vertices;
    auto add = [&](const Vertex &v) {
        if (!vertices.isEmpty() && vertices.last().pos == v.pos) {
            Vertex &last = vertices.last();
            last.radius = qMax(last.radius, v.radius);
            last.corner = last.corner || v.corner;
        } else {
            vertices << v;
        }
    };

    for (int i = begin; i < end; i++) {
        add({d.positions[i], radius(d.radii[i]), true});

        const bool last = i + 1 == end;
        if (last && !closed) break;
        const int next = last ? begin : i + 1;
        if (d.segment(i) != VCurveData::CurveSegment) continue;

        const QPointF p0 = d.positions[i];
        const QPointF c1 = d.handlesOut[i];
        const QPointF c2 = d.handlesIn[next];
        const QPointF p1 = d.positions[next];
        const int n = bezierSegments(p0, c1, c2, p1, tolerance);
        for (int k = 1; k < n; k++) {
            const qreal t = qreal(k) / n;
            add({bezierPoint(p0, c1, c2, p1, t), radius(d.radii[i] + (d.radii[next] - d.radii[i]) * t), false});
        }
    }

    if (closed && vertices.size() > 1 && vertices.last().pos == vertices.first().pos) {
        vertices.first().corner = vertices.first().corner || vertices.last().corner;
        vertices.removeLast();
    }
    return vertices;
}

void addQuad(QPainterPath &path, const Vertex &a, const Vertex &b)
{
    const QPointF d = b.pos - a.pos;
    const qreal length = KisAlgebra2D::norm(d);
    if (length < 1e-12 || (a.radius <= 0 && b.radius <= 0)) return;

    const QPointF n = normal(d / length);
    addPositivePolygon(path, QPolygonF({a.pos + n * a.radius, b.pos + n * b.radius,
                                        b.pos - n * b.radius, a.pos - n * a.radius}));
}

void addJoin(QPainterPath &path, const Vertex &v, const QPointF &d1, const QPointF &d2,
             Qt::PenJoinStyle join, qreal miterLimit)
{
    const qreal r = v.radius;
    if (r <= 0) return;

    if (join == Qt::RoundJoin) {
        addDisk(path, v.pos, r);
        return;
    }

    const qreal cross = d1.x() * d2.y() - d1.y() * d2.x();
    if (qAbs(cross) < 1e-12 && KisAlgebra2D::dotProduct(d1, d2) > 0) return; // straight on

    // the outer side of the turn
    const qreal s = cross > 0 ? -1.0 : 1.0;
    const QPointF n1 = normal(d1);
    const QPointF n2 = normal(d2);
    const QPointF a = v.pos + s * n1 * r;
    const QPointF b = v.pos + s * n2 * r;

    const QPointF bisector = n1 + n2;
    const qreal bisectorLength = KisAlgebra2D::norm(bisector);
    const qreal cosHalf = 0.5 * bisectorLength;   // the cosine of half the turn

    if ((join == Qt::MiterJoin || join == Qt::SvgMiterJoin) && cosHalf > 1e-9 && 1.0 / cosHalf <= miterLimit) {
        const QPointF tip = v.pos + s * bisector / bisectorLength * (r / cosHalf);
        addPositivePolygon(path, QPolygonF({v.pos, a, tip, b}));
    } else {
        addPositivePolygon(path, QPolygonF({v.pos, a, b}));
    }
}

void addCap(QPainterPath &path, const Vertex &v, const QPointF &outward, Qt::PenCapStyle cap)
{
    const qreal r = v.radius;
    if (r <= 0) return;

    if (cap == Qt::RoundCap) {
        addDisk(path, v.pos, r);
    } else if (cap == Qt::SquareCap) {
        const QPointF n = normal(outward);
        addPositivePolygon(path, QPolygonF({v.pos + n * r, v.pos + n * r + outward * r,
                                            v.pos - n * r + outward * r, v.pos - n * r}));
    }
}

QPointF direction(const Vertex &a, const Vertex &b)
{
    const QPointF d = b.pos - a.pos;
    return d / KisAlgebra2D::norm(d);
}

}

QPainterPath VCurveOutline::line(const VCurveData &curves, const VCurveStrokeStyle &style,
                                 qreal radiusScale, qreal minRadius, qreal tolerance)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);

    for (int c = 0; c < curves.curveCount(); c++) {
        const bool closed = curves.isCyclic(c) && curves.curveEnd(c) - curves.curveBegin(c) > 1;
        const QVector<Vertex> v = flattenCurve(curves, c, radiusScale, minRadius, tolerance, closed);
        const int m = v.size();

        if (m == 1) {
            // a dot
            if (style.cap == Qt::SquareCap) {
                const qreal r = v[0].radius;
                addPositivePolygon(path, QPolygonF(QRectF(v[0].pos - QPointF(r, r), QSizeF(2 * r, 2 * r))));
            } else if (style.cap == Qt::RoundCap) {
                addDisk(path, v[0].pos, v[0].radius);
            }
            continue;
        }

        const bool cyclic = closed && m > 2;
        const int segmentCount = cyclic ? m : m - 1;
        for (int k = 0; k < segmentCount; k++) {
            addQuad(path, v[k], v[(k + 1) % m]);
        }

        for (int k = 0; k < m; k++) {
            const bool hasIn = cyclic || k > 0;
            const bool hasOut = cyclic || k < m - 1;
            if (!hasIn || !hasOut) continue;

            const QPointF d1 = direction(v[(k + m - 1) % m], v[k]);
            const QPointF d2 = direction(v[k], v[(k + 1) % m]);
            const Qt::PenJoinStyle join =
                v[k].corner ? style.join
                            : style.join == Qt::RoundJoin ? Qt::RoundJoin : Qt::BevelJoin;
            addJoin(path, v[k], d1, d2, join, style.miterLimit);
        }

        if (!cyclic) {
            // ends a trim cut are flat
            const quint8 caps = curves.capFlags(c);
            addCap(path, v.first(), -direction(v[0], v[1]), caps & VCurveData::StartFlat ? Qt::FlatCap : style.cap);
            addCap(path, v.last(), direction(v[m - 2], v[m - 1]), caps & VCurveData::EndFlat ? Qt::FlatCap : style.cap);
        }
    }

    return path;
}

QPainterPath VCurveOutline::fill(const VCurveData &curves, Qt::FillRule fillRule)
{
    QPainterPath path;
    path.setFillRule(fillRule);

    auto addSegments = [&](int c, bool close) {
        const int begin = curves.curveBegin(c);
        const int end = curves.curveEnd(c);
        const int last = close ? end : end - 1;
        for (int i = begin; i < last; i++) {
            const int next = i + 1 < end ? i + 1 : begin;
            if (curves.segment(i) == VCurveData::CurveSegment) {
                path.cubicTo(curves.handlesOut[i], curves.handlesIn[next], curves.positions[next]);
            } else {
                path.lineTo(curves.positions[next]);
            }
        }
    };

    // closed sub-paths on their own (holes keep working)
    for (int c = 0; c < curves.curveCount(); c++) {
        if (!curves.isCyclic(c) || curves.curveEnd(c) - curves.curveBegin(c) < 3) continue;
        path.moveTo(curves.positions[curves.curveBegin(c)]);
        addSegments(c, true);
        path.closeSubpath();
    }

    // open ones joined in order into one area
    bool started = false;
    for (int c = 0; c < curves.curveCount(); c++) {
        if (curves.isCyclic(c) && curves.curveEnd(c) - curves.curveBegin(c) >= 3) continue;
        const QPointF start = curves.positions[curves.curveBegin(c)];
        if (started) {
            path.lineTo(start);
        } else {
            path.moveTo(start);
            started = true;
        }
        addSegments(c, false);
    }
    if (started) path.closeSubpath();

    return path;
}
