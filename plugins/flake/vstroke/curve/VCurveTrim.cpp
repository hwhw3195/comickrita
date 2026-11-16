/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveTrim.h"

#include <QHash>
#include <QLineF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

/// crossings this near a segment end snap to it, as Blender does
const qreal SnapFactor = 1e-4;

qreal dot(const QPointF &a, const QPointF &b)
{
    return a.x() * b.x() + a.y() * b.y();
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

qreal norm(const QPointF &v)
{
    return std::sqrt(dot(v, v));
}

int segmentCount(const VCurveData &c)
{
    const int n = c.pointCount();
    return c.curveCount() > 0 && c.isCyclic(0) && n > 1 ? n : qMax(0, n - 1);
}

struct Edge {
    QPointF a, b;
    int stroke;     ///< target index; -1 for cutters
    int index;      ///< edge number along its stroke
    int segment;
    qreal t0, t1;
};

/// the edges of one sub-path, beziers flattened to \p tolerance
QVector<Edge> flatten(const VCurveData &c, int stroke, qreal tolerance)
{
    QVector<Edge> edges;
    const int n = c.pointCount();
    const int count = segmentCount(c);
    const QStringList none;
    for (int k = 0; k < count; k++) {
        const VCurvePoint a = c.point(k, none);
        const VCurvePoint b = c.point((k + 1) % n, none);
        int parts = 1;
        if (a.segment == VCurveData::CurveSegment) {
            const qreal length = norm(a.handleOut - a.position) + norm(b.handleIn - a.handleOut) + norm(b.position - b.handleIn);
            parts = qBound(2, int(std::ceil(std::sqrt(length / tolerance))), 64);
        }
        QPointF previous = a.position;
        qreal previousT = 0;
        for (int s = 1; s <= parts; s++) {
            const qreal t = qreal(s) / parts;
            const QPointF p = s == parts ? b.position : VCurvePoint::segmentPoint(a, b, t);
            // no edge where the pen stood still: the edges around it share a point
            if (p == previous) continue;
            edges << Edge{previous, p, stroke, edges.size(), k, previousT, t};
            previous = p;
            previousT = t;
        }
    }
    return edges;
}

/// a uniform grid of edges
class EdgeGrid
{
public:
    EdgeGrid(const QVector<Edge> &edges)
        : m_edges(edges)
    {
        QRectF bounds;
        qreal total = 0;
        for (const Edge &e : edges) {
            bounds |= QRectF(e.a, e.b).normalized().adjusted(-1e-9, -1e-9, 1e-9, 1e-9);
            total += norm(e.b - e.a);
        }
        m_cell = qMax(edges.isEmpty() ? 1.0 : 2 * total / edges.size(),
                      std::sqrt(bounds.width() * bounds.height() / qMax(1, edges.size())));
        m_cell = qMax(m_cell, 1e-6);
        for (int i = 0; i < edges.size(); i++) {
            forCells(QRectF(edges[i].a, edges[i].b).normalized(), [&] (quint64 key) { m_cells[key] << i; });
        }
    }

    QVector<int> candidates(const QRectF &rect) const
    {
        QVector<int> result;
        forCells(rect, [&] (quint64 key) {
            auto it = m_cells.constFind(key);
            if (it != m_cells.constEnd()) result << *it;
        });
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

private:
    template <typename F>
    void forCells(const QRectF &rect, F f) const
    {
        const int x0 = int(std::floor(rect.left() / m_cell)), x1 = int(std::floor(rect.right() / m_cell));
        const int y0 = int(std::floor(rect.top() / m_cell)), y1 = int(std::floor(rect.bottom() / m_cell));
        for (int x = x0; x <= x1; x++) {
            for (int y = y0; y <= y1; y++) f((quint64(quint32(x)) << 32) | quint32(y));
        }
    }

    const QVector<Edge> &m_edges;
    qreal m_cell = 1.0;
    QHash<quint64, QVector<int>> m_cells;
};

/// where segment a0-a1 crosses b0-b1, as factors along each; false when they don't
bool intersect(const QPointF &a0, const QPointF &a1, const QPointF &b0, const QPointF &b1, qreal *u, qreal *v)
{
    const QPointF r = a1 - a0;
    const QPointF s = b1 - b0;
    const qreal denominator = cross(r, s);
    if (std::abs(denominator) < 1e-15) return false;
    *u = cross(b0 - a0, s) / denominator;
    *v = cross(b0 - a0, r) / denominator;
    return *u >= -SnapFactor && *u <= 1 + SnapFactor && *v >= -SnapFactor && *v <= 1 + SnapFactor;
}

/// overlap including touching edges: QRectF::intersects() is false for a flat rectangle, such as a level line's
bool overlaps(const QRectF &a, const QRectF &b)
{
    return a.left() <= b.right() && b.left() <= a.right() && a.top() <= b.bottom() && b.top() <= a.bottom();
}

qreal radiusAt(const VCurveData &c, qreal position)
{
    const int n = c.pointCount();
    const int segment = int(std::floor(position));
    const qreal r0 = c.radii[segment % n];
    return r0 + (position - segment) * (c.radii[(segment + 1) % n] - r0);
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal l2 = dot(ab, ab);
    const qreal t = l2 > 0 ? qBound(0.0, dot(p - a, ab) / l2, 1.0) : 0.0;
    return norm(p - (a + t * ab));
}

qreal segmentDistance(const QPointF &a0, const QPointF &a1, const QPointF &b0, const QPointF &b1)
{
    qreal u, v;
    if (intersect(a0, a1, b0, b1, &u, &v)) return 0;
    return qMin(qMin(distanceToSegment(a0, b0, b1), distanceToSegment(a1, b0, b1)),
                qMin(distanceToSegment(b0, a0, a1), distanceToSegment(b1, a0, a1)));
}

QPolygonF polyline(const VCurveData &c, qreal tolerance)
{
    QPolygonF line;
    const QVector<Edge> edges = flatten(c, 0, tolerance);
    if (edges.isEmpty()) {
        if (c.pointCount() > 0) line << c.positions.first();
        return line;
    }
    line << edges.first().a;
    for (const Edge &e : edges) line << e.b;
    return line;
}

qreal distanceToPolyline(const QPointF &p, const QPolygonF &line)
{
    if (line.size() == 1) return norm(p - line[0]);
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < line.size(); i++) best = qMin(best, distanceToSegment(p, line[i], line[i + 1]));
    return best;
}

/// the radius at each point of polyline(\p c)
QVector<qreal> polylineRadii(const VCurveData &c, qreal tolerance)
{
    QVector<qreal> radii;
    const QVector<Edge> edges = flatten(c, 0, tolerance);
    if (edges.isEmpty()) {
        if (c.pointCount() > 0) radii << c.radii.first();
        return radii;
    }
    radii << radiusAt(c, edges.first().segment + edges.first().t0);
    for (const Edge &e : edges) radii << radiusAt(c, e.segment + e.t1);
    return radii;
}

/// how far \p p is outside the ink of \p line (negative inside), the radius taken as the larger at each edge's ends
qreal distanceToInk(const QPointF &p, const QPolygonF &line, const QVector<qreal> &radii)
{
    if (line.size() == 1) return norm(p - line[0]) - radii[0];
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < line.size(); i++) {
        best = qMin(best, distanceToSegment(p, line[i], line[i + 1]) - qMax(radii[i], radii[i + 1]));
    }
    return best;
}

/// whether the drag or lasso touches \p piece: a drag where it reaches the ink, a lasso where it takes the centerline
bool touches(const QPolygonF &piece, const QVector<qreal> &radii, const VCurveTrim::Selector &selector)
{
    if (selector.mode == VCurveTrim::Selector::Drag) {
        const QPolygonF &path = selector.path;
        if (path.size() == 1) return distanceToInk(path[0], piece, radii) <= selector.radius;
        if (piece.size() == 1) {
            return distanceToPolyline(piece[0], path) <= selector.radius + radii[0];
        }
        for (int i = 0; i + 1 < piece.size(); i++) {
            const qreal reach = selector.radius + qMax(radii[i], radii[i + 1]);
            for (int j = 0; j + 1 < path.size(); j++) {
                if (segmentDistance(piece[i], piece[i + 1], path[j], path[j + 1]) <= reach) return true;
            }
        }
        return false;
    }
    // lasso: a point inside, or an edge across
    const QPolygonF &lasso = selector.path;
    for (const QPointF &p : piece) {
        if (lasso.containsPoint(p, Qt::OddEvenFill)) return true;
    }
    for (int i = 0; i + 1 < piece.size(); i++) {
        for (int j = 0; j < lasso.size(); j++) {
            qreal u, v;
            if (intersect(piece[i], piece[i + 1], lasso[j], lasso[(j + 1) % lasso.size()], &u, &v)) return true;
        }
    }
    return false;
}

/**
 * The factors along p0-p1 within \p r of segment a-b. The distance is
 * convex along the edge, so they make one interval: its nearest place by
 * ternary search, its ends by bisection.
 */
bool capsuleInterval(const QPointF &p0, const QPointF &p1, const QPointF &a, const QPointF &b, qreal r,
                     qreal *u0, qreal *u1)
{
    auto d = [&] (qreal u) { return distanceToSegment(p0 + u * (p1 - p0), a, b); };
    qreal lo = 0, hi = 1;
    for (int i = 0; i < 60; i++) {
        const qreal m1 = lo + (hi - lo) / 3;
        const qreal m2 = hi - (hi - lo) / 3;
        if (d(m1) < d(m2)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    const qreal nearest = (lo + hi) / 2;
    if (d(nearest) > r) return false;
    auto boundary = [&] (qreal outside, qreal inside) {
        if (d(outside) <= r) return outside;
        for (int i = 0; i < 50; i++) {
            const qreal mid = (outside + inside) / 2;
            if (d(mid) <= r) {
                inside = mid;
            } else {
                outside = mid;
            }
        }
        return inside;
    };
    *u0 = boundary(0.0, nearest);
    *u1 = boundary(1.0, nearest);
    return true;
}

/// \p range of a stroke as a separate stroke, with flat cut ends unless caps are kept
VCurveData keptPiece(const VCurveData &c, qreal from, qreal to, const VCurveTrim::Options &options)
{
    const int count = segmentCount(c);
    const bool cyclic = c.isCyclic(0);
    VCurveData piece = VCurveTrim::extract(c, from, to);
    quint8 caps = 0;
    const bool startCut = cyclic || from > 1e-9;
    const bool endCut = cyclic || to < count - 1e-9;
    caps |= startCut ? (options.keepCaps ? 0 : VCurveData::StartFlat) : (c.capFlags(0) & VCurveData::StartFlat);
    caps |= endCut ? (options.keepCaps ? 0 : VCurveData::EndFlat) : (c.capFlags(0) & VCurveData::EndFlat);
    piece.setCapFlags(0, caps);
    return piece;
}

/// Options::WholeStroke and Options::Area
/// shorter than it is wide, a kept piece would only show as a dot
bool isSliver(const VCurveData &piece, const VCurveTrim::Options &options)
{
    const QPolygonF line = polyline(piece, options.tolerance);
    qreal length = 0;
    for (int i = 0; i + 1 < line.size(); i++) length += norm(line[i + 1] - line[i]);
    const qreal diameter = 2 * *std::max_element(piece.radii.begin(), piece.radii.end());
    return length < qMax(options.tolerance, diameter);
}

/**
 * Dabs where the pen came down or lifted are sub-paths of their own at the
 * start or end of the stroke next to them, too small to touch on purpose.
 * When that end goes they would stay behind as a dot, so they go with it.
 */
QVector<VDocStroke> dropOrphanedTips(const QVector<VDocStroke> &targets, QVector<VDocStroke> result,
                                     const VCurveTrim::Options &options)
{
    auto near = [&] (const VCurveData &c, const VCurveData &tip) {
        const qreal radius = *std::max_element(tip.radii.begin(), tip.radii.end());
        const qreal reach = qMax(options.tolerance, 2 * radius);
        for (const QPointF &a : {c.positions.first(), c.positions.last()}) {
            for (const QPointF &b : {tip.positions.first(), tip.positions.last()}) {
                if (norm(a - b) <= reach) return true;
            }
        }
        return false;
    };
    for (int s = 0; s < targets.size(); s++) {
        const VCurveData &tip = targets[s].curve;
        if (tip.isEmpty() || !isSliver(tip, options)) continue;
        bool tipOfOne = false;
        for (int other : {s - 1, s + 1}) {
            if (other < 0 || other >= targets.size() || targets[other].owner != targets[s].owner) continue;
            const VCurveData &next = targets[other].curve;
            tipOfOne |= !next.isEmpty() && !isSliver(next, options) && near(next, tip);
        }
        if (!tipOfOne) continue;
        int entry = -1;
        bool attached = false;
        for (int r = 0; r < result.size(); r++) {
            if (result[r].owner != targets[s].owner || result[r].curve.isEmpty()) continue;
            if (result[r].curve == tip) {
                entry = r;
            } else if (!isSliver(result[r].curve, options) && near(result[r].curve, tip)) {
                attached = true;
            }
        }
        if (entry >= 0 && !attached) result[entry] = VDocStroke{VCurveData(), targets[s].owner, true};
    }
    return result;
}

QVector<VDocStroke> eraseDirect(const QVector<VDocStroke> &targets, const VCurveTrim::Selector &selector,
                                const VCurveTrim::Options &options)
{
    VCurveTrim::Selector area = selector;
    if (area.mode == VCurveTrim::Selector::Click && options.erase == VCurveTrim::Options::Area) {
        area.mode = VCurveTrim::Selector::Drag;
        area.path = QPolygonF({selector.point});
    }

    QVector<VDocStroke> result;
    if (options.erase == VCurveTrim::Options::WholeStroke) {
        int clicked = -1;
        if (area.mode == VCurveTrim::Selector::Click) {
            // the nearest stroke only
            qreal best = area.radius;
            for (int s = 0; s < targets.size(); s++) {
                const VCurveData &c = targets[s].curve;
                if (c.isEmpty()) continue;
                const qreal d = qMax(0.0, distanceToInk(area.point, polyline(c, options.tolerance),
                                                        polylineRadii(c, options.tolerance)));
                if (d <= best) {
                    best = d;
                    clicked = s;
                }
            }
        }
        for (int s = 0; s < targets.size(); s++) {
            // a drag takes the strokes whose ink it reaches
            VCurveTrim::Selector reach = area;
            if (reach.mode == VCurveTrim::Selector::Drag && !targets[s].curve.isEmpty()) {
                reach.radius += *std::max_element(targets[s].curve.radii.begin(), targets[s].curve.radii.end());
            }
            const bool gone = area.mode == VCurveTrim::Selector::Click
                ? s == clicked
                : !VCurveTrim::insideRanges(targets[s].curve, reach, options.tolerance).isEmpty();
            result << (gone ? VDocStroke{VCurveData(), targets[s].owner, true} : targets[s]);
        }
        return result;
    }

    for (const VDocStroke &target : targets) {
        const VCurveData &c = target.curve;
        const QVector<QPair<qreal, qreal>> inside = VCurveTrim::insideRanges(c, area, options.tolerance);
        if (inside.isEmpty()) {
            result << target;
            continue;
        }
        const int count = segmentCount(c);
        QVector<QPair<qreal, qreal>> kept;
        if (count > 0 && c.isCyclic(0)) {
            for (int i = 0; i < inside.size(); i++) {
                const qreal to = i + 1 < inside.size() ? inside[i + 1].first : inside[0].first + count;
                kept << qMakePair(inside[i].second, to);
            }
        } else if (count > 0) {
            qreal from = 0;
            for (const auto &range : inside) {
                kept << qMakePair(from, range.first);
                from = qMax(from, range.second);
            }
            kept << qMakePair(from, qreal(count));
        }
        bool any = false;
        for (const auto &range : kept) {
            if (range.second - range.first < 1e-6) continue;
            const VCurveData piece = keptPiece(c, range.first, range.second, options);
            if (isSliver(piece, options)) continue;
            result << VDocStroke{piece, target.owner, true};
            any = true;
        }
        if (!any) result << VDocStroke{VCurveData(), target.owner, true};
    }
    return result;
}

}

QVector<QPair<qreal, qreal>> VCurveTrim::insideRanges(const VCurveData &c, const Selector &selector, qreal tolerance)
{
    QVector<QPair<qreal, qreal>> ranges;
    if (c.pointCount() == 0) return ranges;
    const bool lasso = selector.mode == Selector::Lasso;
    const QPolygonF path = selector.mode == Selector::Click ? QPolygonF({selector.point}) : selector.path;
    if (path.isEmpty() || (lasso && path.size() < 3)) return ranges;
    const qreal r = lasso ? 0.0 : selector.radius;
    // a brush of size 0 has no area: nothing is inside it, not even where its path crosses the curve
    if (!lasso && r <= 0.0) return ranges;
    const QRectF area = path.boundingRect().adjusted(-r, -r, r, r);

    if (segmentCount(c) == 0) {
        const QPointF p = c.positions.first();
        if (lasso ? path.containsPoint(p, Qt::OddEvenFill) : distanceToPolyline(p, path) <= r) {
            ranges << qMakePair(0.0, 0.0);
        }
        return ranges;
    }

    for (const Edge &e : flatten(c, 0, tolerance)) {
        if (!overlaps(QRectF(e.a, e.b).normalized(), area)) continue;
        auto along = [&e] (qreal u) { return e.segment + e.t0 + u * (e.t1 - e.t0); };
        if (lasso) {
            // between the crossings with the lasso, in or out
            QVector<qreal> at{0.0, 1.0};
            for (int j = 0; j < path.size(); j++) {
                qreal u, v;
                if (intersect(e.a, e.b, path[j], path[(j + 1) % path.size()], &u, &v)) at << qBound(0.0, u, 1.0);
            }
            std::sort(at.begin(), at.end());
            for (int k = 0; k + 1 < at.size(); k++) {
                if (at[k + 1] - at[k] < 1e-12) continue;
                const QPointF middle = e.a + (at[k] + at[k + 1]) / 2 * (e.b - e.a);
                if (path.containsPoint(middle, Qt::OddEvenFill)) ranges << qMakePair(along(at[k]), along(at[k + 1]));
            }
            continue;
        }
        const QRectF edgeArea = QRectF(e.a, e.b).normalized().adjusted(-r, -r, r, r);
        for (int j = 0; j < qMax(1, path.size() - 1); j++) {
            const QPointF a = path[j];
            const QPointF b = path.size() > 1 ? path[j + 1] : a;
            if (!overlaps(QRectF(a, b).normalized(), edgeArea)) continue;
            qreal u0, u1;
            if (capsuleInterval(e.a, e.b, a, b, r, &u0, &u1)) ranges << qMakePair(along(u0), along(u1));
        }
    }

    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<qreal, qreal>> merged;
    for (const auto &range : ranges) {
        if (!merged.isEmpty() && range.first <= merged.last().second + 1e-9) {
            merged.last().second = qMax(merged.last().second, range.second);
        } else {
            merged << range;
        }
    }
    return merged;
}

QVector<VCurveData> VCurveTrim::keepInside(const VCurveData &c, const std::function<bool(const QPointF &)> &inside, qreal step)
{
    QVector<VCurveData> result;
    if (c.pointCount() == 0) return result;
    const int count = segmentCount(c);
    if (count == 0) {
        if (inside(c.positions.first())) result << c;
        return result;
    }

    Options options;
    options.tolerance = step;
    // the runs of samples inside, in segment positions
    QVector<QPair<qreal, qreal>> runs;
    bool wasInside = false;
    bool allInside = true;
    auto sample = [&] (const QPointF &p, qreal at) {
        const bool in = inside(p);
        allInside &= in;
        if (in && wasInside) {
            runs.last().second = at;
        } else if (in) {
            runs << qMakePair(at, at);
        }
        wasInside = in;
    };
    const QVector<Edge> edges = flatten(c, 0, step);
    for (const Edge &e : edges) {
        const int parts = qMax(1, int(std::ceil(norm(e.b - e.a) / step)));
        for (int i = e.index == 0 ? 0 : 1; i <= parts; i++) {
            const qreal u = qreal(i) / parts;
            sample(e.a + u * (e.b - e.a), e.segment + e.t0 + u * (e.t1 - e.t0));
        }
    }
    if (allInside) {
        result << c;
        return result;
    }
    // a closed line's run can go on past its start
    if (c.isCyclic(0) && runs.size() > 1 && runs.first().first <= 0 && runs.last().second >= count) {
        runs.first().first = runs.last().first - count;
        runs.removeLast();
        const QPair<qreal, qreal> around(runs.first().first + count, runs.first().second + count);
        runs.first() = around;
    }
    for (const auto &run : runs) {
        if (run.second - run.first < 1e-6) continue;
        const VCurveData piece = keptPiece(c, run.first, run.second, options);
        if (!isSliver(piece, options)) result << piece;
    }
    return result;
}

VCurveData VCurveTrim::extract(const VCurveData &c, qreal from, qreal to)
{
    const QStringList names = c.attributeNames();
    const int n = c.pointCount();
    QVector<VCurvePoint> out;
    if (n == 0) return VCurveData();

    for (int k = int(std::floor(from)); k < int(std::ceil(to - 1e-12)); k++) {
        const qreal ta = qMax(from - k, 0.0);
        const qreal tb = qMin(to - k, 1.0);
        VCurvePoint a = c.point(k % n, names);
        VCurvePoint b = c.point((k + 1) % n, names);
        if (tb < 1.0) {
            b = VCurvePoint::split(a, b, tb);
        }
        if (ta > 0.0) {
            VCurvePoint start = VCurvePoint::split(a, b, ta / tb);
            a = start;
        }
        if (out.isEmpty()) {
            out << a;
        } else {
            // the point between two segments: its incoming handle from the last, its outgoing from this one
            out.last().handleOut = a.handleOut;
            out.last().segment = a.segment;
        }
        out << b;
    }
    if (out.isEmpty()) {
        // a single point
        out << c.point(int(std::round(from)) % n, names);
    }
    out.last().segment = VCurveData::LineSegment;
    out.last().handleOut = out.last().position;
    out.first().handleIn = out.first().position;

    VCurveBuilder builder(names);
    builder.beginCurve(false);
    for (const VCurvePoint &p : out) builder.add(p);
    return builder.take();
}

QVector<QVector<VCurveTrim::Cut>> VCurveTrim::crossings(const QVector<VDocStroke> &targets,
                                                        const QVector<VCurveData> &cutters, qreal tolerance,
                                                        qreal leeway)
{
    QVector<Edge> edges;
    QVector<int> edgeCounts;
    QVector<qreal> arcAtEdge;   // of the target edges: the length along the stroke before each
    QVector<qreal> lengths;
    for (int s = 0; s < targets.size(); s++) {
        const QVector<Edge> e = flatten(targets[s].curve, s, tolerance);
        edgeCounts << e.size();
        edges << e;
        qreal length = 0;
        for (const Edge &edge : e) {
            arcAtEdge << length;
            length += norm(edge.b - edge.a);
        }
        lengths << length;
    }
    const int targetEdges = edges.size();
    // the cutters' sub-paths, each with its own id below 0, and the ends of every open stroke
    struct End {
        QPointF point;
        qreal radius;
        int stroke;
    };
    QVector<End> ends;
    auto addEnds = [&ends] (const VCurveData &c, int stroke) {
        if (segmentCount(c) == 0 || c.isCyclic(0)) return;
        ends << End{c.positions.first(), c.radii.first(), stroke} << End{c.positions.last(), c.radii.last(), stroke};
    };
    for (int s = 0; s < targets.size(); s++) addEnds(targets[s].curve, s);
    // a stroke shorter than it is wide (a dot, a pen-down or pen-lift tip) cuts nothing
    auto isDot = [tolerance] (const VCurveData &c, qreal length) {
        if (c.isEmpty()) return true;
        return length < qMax(tolerance, 2 * *std::max_element(c.radii.begin(), c.radii.end()));
    };
    QVector<bool> targetDot(targets.size());
    for (int s = 0; s < targets.size(); s++) targetDot[s] = isDot(targets[s].curve, lengths[s]);
    QVector<VCurveData> cutterCurves;   // by -id - 1
    QVector<bool> cutterDot;
    int cutterId = -1;
    for (const VCurveData &c : cutters) {
        for (int k = 0; k < c.curveCount(); k++, cutterId--) {
            const VCurveData sub = c.curve(k);
            const QVector<Edge> e = flatten(sub, cutterId, tolerance);
            qreal length = 0;
            for (const Edge &edge : e) length += norm(edge.b - edge.a);
            cutterCurves << sub;
            cutterDot << isDot(sub, length);
            if (cutterDot.last()) continue;
            edges << e;
            addEnds(sub, cutterId);
        }
    }
    auto curveOf = [&] (int stroke) -> const VCurveData & {
        return stroke >= 0 ? targets[stroke].curve : cutterCurves[-stroke - 1];
    };
    auto isDotStroke = [&] (int stroke) { return stroke >= 0 ? targetDot[stroke] : cutterDot[-stroke - 1]; };
    ends.erase(std::remove_if(ends.begin(), ends.end(), [&] (const End &end) { return isDotStroke(end.stroke); }), ends.end());
    const EdgeGrid grid(edges);

    QVector<QVector<qreal>> positions(targets.size());
    // where each target's centerline crosses another's, and whose
    QVector<QVector<QPair<QPointF, int>>> crossed(targets.size());
    for (int i = 0; i < targetEdges; i++) {
        const Edge &e = edges[i];
        if (targetDot[e.stroke]) continue;
        const VCurveData &curve = targets[e.stroke].curve;
        const bool cyclic = curve.isCyclic(0);
        const int count = edgeCounts[e.stroke];
        for (int j : grid.candidates(QRectF(e.a, e.b).normalized())) {
            if (j == i) continue;
            const Edge &f = edges[j];
            if (isDotStroke(f.stroke)) continue;
            if (f.stroke == e.stroke) {
                // neighbouring edges share a point, which isn't a crossing
                const int gap = std::abs(f.index - e.index);
                if (gap <= 1 || (cyclic && gap == count - 1)) continue;
            }
            qreal u, v;
            if (!intersect(e.a, e.b, f.a, f.b, &u, &v)) continue;
            if (u < SnapFactor) u = 0;
            if (u > 1 - SnapFactor) u = 1;
            const qreal position = e.segment + e.t0 + u * (e.t1 - e.t0);
            if (f.stroke == e.stroke) {
                // a loop under the stroke's own width isn't a crossing anyone can see
                const qreal w = qBound(0.0, v, 1.0);
                qreal loop = std::abs(arcAtEdge[j] + w * norm(f.b - f.a) - arcAtEdge[i] - u * norm(e.b - e.a));
                if (cyclic) loop = qMin(loop, lengths[e.stroke] - loop);
                const qreal other = f.segment + f.t0 + w * (f.t1 - f.t0);
                if (loop <= 2 * qMax(radiusAt(curve, position), radiusAt(curve, other))) continue;
                positions[e.stroke] << position;
                continue;
            }
            crossed[e.stroke] << qMakePair(e.a + u * (e.b - e.a), f.stroke);
            if (!cyclic) {
                // as near the stroke's own end as the inks are wide, the piece cut off would hide in the other's ink
                const qreal w = qBound(0.0, v, 1.0);
                const qreal hidden = radiusAt(curve, position) + radiusAt(curveOf(f.stroke), f.segment + f.t0 + w * (f.t1 - f.t0));
                const qreal arc = arcAtEdge[i] + u * norm(e.b - e.a);
                if (arc <= hidden || lengths[e.stroke] - arc <= hidden) continue;
            }
            positions[e.stroke] << position;
        }
    }

    // Another stroke that ends on this one, its ink on this one's but its
    // centerline short of it, crosses it too, where it is nearest. Not where
    // the centerlines cross by that end already, and not by this stroke's
    // own ends, where the piece cut off would be hidden in the other's ink
    // (as a pen-lift tip's). A stroke's own end doesn't count, or sharp turns
    // would.
    QVector<qreal> widest(targets.size(), 0.0);
    qreal widestOfAll = 0;
    for (int s = 0; s < targets.size(); s++) {
        const VCurveData &curve = targets[s].curve;
        if (!curve.isEmpty()) widest[s] = *std::max_element(curve.radii.begin(), curve.radii.end());
        widestOfAll = qMax(widestOfAll, widest[s]);
    }
    struct Nearest {
        int edge = -1;
        qreal u = 0;
        qreal distance = std::numeric_limits<qreal>::max();
    };
    for (const End &end : ends) {
        const qreal reach = widestOfAll + end.radius + leeway;
        const QRectF area(end.point - QPointF(reach, reach), end.point + QPointF(reach, reach));
        // the nearest edge of each target near the end
        QHash<int, Nearest> nearest;
        for (int j : grid.candidates(area)) {
            const Edge &e = edges[j];
            if (e.stroke < 0 || e.stroke == end.stroke || targetDot[e.stroke]) continue;
            const QPointF ab = e.b - e.a;
            const qreal u = qBound(0.0, dot(end.point - e.a, ab) / dot(ab, ab), 1.0);
            const qreal d = norm(end.point - (e.a + u * ab));
            if (d > widest[e.stroke] + end.radius + leeway) continue;
            Nearest &n = nearest[e.stroke];
            if (d < n.distance) n = Nearest{j, u, d};
        }
        for (auto it = nearest.constBegin(); it != nearest.constEnd(); ++it) {
            const int s = it.key();
            const VCurveData &curve = targets[s].curve;
            const Edge &e = edges[it->edge];
            const qreal position = e.segment + e.t0 + it->u * (e.t1 - e.t0);
            const qreal inks = radiusAt(curve, position) + end.radius;
            const qreal touch = inks + leeway;
            if (it->distance > touch) continue;
            const bool crossesHere = std::any_of(crossed[s].begin(), crossed[s].end(), [&] (const QPair<QPointF, int> &c) {
                return c.second == end.stroke && norm(c.first - end.point) <= touch;
            });
            if (crossesHere) continue;
            if (!curve.isCyclic(0)) {
                const qreal arc = arcAtEdge[it->edge] + it->u * norm(e.b - e.a);
                if (arc <= inks || lengths[s] - arc <= inks) continue;
            }
            positions[s] << position;
        }
    }

    QVector<QVector<Cut>> result(targets.size());
    for (int s = 0; s < targets.size(); s++) {
        QVector<qreal> &p = positions[s];
        std::sort(p.begin(), p.end());
        const int count = segmentCount(targets[s].curve);
        const bool cyclic = targets[s].curve.isCyclic(0);
        qreal last = -1;
        for (qreal g : p) {
            if (!cyclic && (g <= 1e-9 || g >= count - 1e-9)) continue;
            if (cyclic && g >= count - 1e-9) g = 0;
            if (last >= 0 && g - last < 1e-6) continue;
            int segment = int(std::floor(g));
            qreal t = g - segment;
            result[s] << Cut{segment, t};
            last = g;
        }
        if (cyclic && result[s].size() > 1) {
            // 0 and the end are the same place
            const Cut &first = result[s].first();
            const Cut &lastCut = result[s].last();
            if (first.segment + first.t < 1e-6 && count - (lastCut.segment + lastCut.t) < 1e-6) result[s].removeLast();
        }
    }
    return result;
}

QVector<VDocStroke> VCurveTrim::trim(const QVector<VDocStroke> &targets, const QVector<VCurveData> &cutters,
                                     const Selector &selector, const Options &options)
{
    if (options.erase != Options::ToCrossings) {
        return dropOrphanedTips(targets, eraseDirect(targets, selector, options), options);
    }
    const QVector<QVector<Cut>> cuts = crossings(targets, cutters, options.tolerance, options.leeway);

    // the pieces between crossings
    struct Piece {
        int stroke;
        qreal from, to;
        QPolygonF line;
        QVector<qreal> radii;
        bool remove = false;
    };
    QVector<QVector<Piece>> pieces(targets.size());
    for (int s = 0; s < targets.size(); s++) {
        const VCurveData &c = targets[s].curve;
        const int count = segmentCount(c);
        if (count == 0) continue;
        QVector<qreal> at;
        for (const Cut &cut : cuts[s]) at << cut.segment + cut.t;
        QVector<QPair<qreal, qreal>> ranges;
        if (c.isCyclic(0)) {
            if (at.isEmpty()) {
                ranges << qMakePair(0.0, qreal(count));
            } else {
                for (int i = 0; i < at.size(); i++) {
                    ranges << qMakePair(at[i], i + 1 < at.size() ? at[i + 1] : at[0] + count);
                }
            }
        } else {
            qreal from = 0;
            for (qreal g : at) {
                ranges << qMakePair(from, g);
                from = g;
            }
            ranges << qMakePair(from, qreal(count));
        }
        for (const auto &range : ranges) {
            const VCurveData piece = c.isCyclic(0) && at.isEmpty() ? c : extract(c, range.first, range.second);
            pieces[s] << Piece{s, range.first, range.second, polyline(piece, options.tolerance),
                               polylineRadii(piece, options.tolerance)};
        }
    }

    // which pieces go
    auto mayGo = [&] (const Piece &p, int index) {
        if (!options.endsOnly) return true;
        const VCurveData &c = targets[p.stroke].curve;
        return !c.isCyclic(0) && pieces[p.stroke].size() >= 2 && (index == 0 || index == pieces[p.stroke].size() - 1);
    };
    if (selector.mode == Selector::Click) {
        Piece *nearest = nullptr;
        qreal best = selector.radius;
        for (auto &list : pieces) {
            for (int i = 0; i < list.size(); i++) {
                if (!mayGo(list[i], i)) continue;
                const qreal d = qMax(0.0, distanceToInk(selector.point, list[i].line, list[i].radii));
                if (d <= best) {
                    best = d;
                    nearest = &list[i];
                }
            }
        }
        if (nearest) nearest->remove = true;
    } else {
        for (auto &list : pieces) {
            for (int i = 0; i < list.size(); i++) {
                Piece &p = list[i];
                const qreal reach = selector.radius + *std::max_element(p.radii.begin(), p.radii.end());
                const QRectF area = selector.path.boundingRect().adjusted(-reach, -reach, reach, reach);
                if (!mayGo(p, i) || !overlaps(p.line.boundingRect(), area)) continue;
                p.remove = touches(p.line, p.radii, selector);
            }
        }
    }

    QVector<VDocStroke> result;
    for (int s = 0; s < targets.size(); s++) {
        const QVector<Piece> &list = pieces[s];
        const bool anyRemoved = std::any_of(list.begin(), list.end(), [] (const Piece &p) { return p.remove; });
        if (!anyRemoved) {
            result << targets[s];
            continue;
        }
        const VCurveData &c = targets[s].curve;
        const bool cyclic = c.isCyclic(0);
        const int count = segmentCount(c);

        // runs of kept pieces, joined again; a closed stroke's run may wrap around
        QVector<QPair<qreal, qreal>> runs;
        int first = 0;
        if (cyclic) {
            while (!list[first].remove) first++;
        }
        for (int k = 0; k < list.size(); k++) {
            const Piece &p = list[(first + k) % list.size()];
            if (p.remove) continue;
            qreal from = p.from;
            qreal to = p.to;
            if (cyclic && from < list[first].from) {
                from += count;
                to += count;
            }
            const int previous = (first + k - 1 + list.size()) % list.size();
            if (!runs.isEmpty() && k > 0 && !list[previous].remove) {
                runs.last().second = to;
            } else {
                runs << qMakePair(from, to);
            }
        }

        bool any = false;
        for (const auto &run : runs) {
            const VCurveData piece = keptPiece(c, run.first, run.second, options);
            if (isSliver(piece, options)) continue;
            result << VDocStroke{piece, targets[s].owner, true};
            any = true;
        }
        if (!any) {
            // all of it went: an empty stroke tells the caller
            result << VDocStroke{VCurveData(), targets[s].owner, true};
        }
    }
    return dropOrphanedTips(targets, result, options);
}
