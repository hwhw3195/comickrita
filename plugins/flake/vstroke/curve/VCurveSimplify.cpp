/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveSimplify.h"

#include <QLineF>
#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

#include <kis_algebra_2d.h>

#include "VCurveFit.h"
#include "VCurveWarp.h"

namespace {

qreal dot(const QPointF &a, const QPointF &b)
{
    return a.x() * b.x() + a.y() * b.y();
}

qreal norm(const QPointF &v)
{
    return std::sqrt(dot(v, v));
}

QPointF unit(const QPointF &v)
{
    const qreal l = norm(v);
    return l > 0 ? v / l : QPointF();
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal l2 = dot(ab, ab);
    const qreal t = l2 > 0 ? qBound(0.0, dot(p - a, ab) / l2, 1.0) : 0.0;
    return norm(p - (a + t * ab));
}

qreal distanceToPolyline(const QPointF &p, const QPolygonF &line)
{
    if (line.size() == 1) return norm(p - line[0]);
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < line.size(); i++) best = qMin(best, distanceToSegment(p, line[i], line[i + 1]));
    return best;
}

/// a rough length of the segment from \p a to \p b (control polygon for beziers)
qreal segmentLength(const VCurvePoint &a, const VCurvePoint &b, const QTransform &t)
{
    const QPointF p0 = t.map(a.position), p1 = t.map(b.position);
    if (a.segment != VCurveData::CurveSegment) return norm(p1 - p0);
    const QPointF c1 = t.map(a.handleOut), c2 = t.map(b.handleIn);
    return (norm(c1 - p0) + norm(c2 - c1) + norm(p1 - c2) + norm(p1 - p0)) / 2;
}

qreal smoothstep(qreal t)
{
    t = qBound(0.0, t, 1.0);
    return t * t * (3 - 2 * t);
}

/**
 * The one segment from pts[a] to pts[b] that stands in for the run between
 * them: a line when every segment of the run is one, otherwise a bezier
 * along the end tangents with handles a third of the run's length. Returns
 * how far (after mapping by \p toDocument) the old line strays from it,
 * with the point nearest that place in \p worstPoint.
 */
qreal collapse(const QVector<VCurvePoint> &pts, int a, int b, const QTransform &toDocument,
               VCurvePoint *first, VCurvePoint *last, int *worstPoint)
{
    bool allLines = true;
    for (int k = a; k < b; k++) allLines &= pts[k].segment != VCurveData::CurveSegment;

    // the candidate, in local units, and its polyline in document units
    *first = pts[a];
    *last = pts[b];
    QPolygonF candidate;
    if (allLines) {
        first->segment = VCurveData::LineSegment;
        first->handleOut = first->position;
        last->handleIn = last->position;
        candidate << toDocument.map(pts[a].position) << toDocument.map(pts[b].position);
    } else {
        QPointF t1 = pts[a].segment == VCurveData::CurveSegment ? pts[a].handleOut - pts[a].position : QPointF();
        if (norm(t1) < 1e-12) t1 = pts[a + 1].position - pts[a].position;
        QPointF t2 = pts[b - 1].segment == VCurveData::CurveSegment ? pts[b].handleIn - pts[b].position : QPointF();
        if (norm(t2) < 1e-12) t2 = pts[b - 1].position - pts[b].position;
        qreal localArc = 0;
        for (int k = a; k < b; k++) localArc += segmentLength(pts[k], pts[k + 1], QTransform());
        first->segment = VCurveData::CurveSegment;
        first->handleOut = first->position + unit(t1) * localArc / 3;
        last->handleIn = last->position + unit(t2) * localArc / 3;
        for (int s = 0; s <= 32; s++) {
            candidate << toDocument.map(VCurvePoint::segmentPoint(*first, *last, s / 32.0));
        }
    }

    // the old line, sampled
    qreal worst = 0;
    *worstPoint = -1;
    for (int k = a; k < b; k++) {
        const bool curve = pts[k].segment == VCurveData::CurveSegment;
        const int samples = curve ? 4 : 1;
        for (int s = k == a ? 1 : 0; s < samples + (curve ? 0 : 1); s++) {
            const qreal t = curve ? qreal(s) / samples : s * 0.5;
            const QPointF p = toDocument.map(t == 0 ? pts[k].position : VCurvePoint::segmentPoint(pts[k], pts[k + 1], t));
            const qreal d = distanceToPolyline(p, candidate);
            if (d > worst) {
                worst = d;
                *worstPoint = t <= 0.5 ? k : k + 1;
            }
        }
    }
    return worst;
}

/// RDP over the points of one sub-path, with a segment model per run
struct Thinner {
    QVector<VCurvePoint> pts;
    QVector<bool> keep;
    QVector<int> attributeIndexes;  ///< attributes compared (not time)
    QTransform toDocument;
    qreal tolerance;
    qreal attributeTolerance;

    void run(int a, int b)
    {
        QVector<QPair<int, int>> stack{{a, b}};
        while (!stack.isEmpty()) {
            const QPair<int, int> range = stack.takeLast();
            const int split = tryCollapse(range.first, range.second);
            if (split > 0) {
                stack << qMakePair(range.first, split) << qMakePair(split, range.second);
            }
        }
    }

    /// collapses the range into one segment and returns 0, or returns where to split it
    int tryCollapse(int a, int b)
    {
        if (b - a < 2) return 0;

        VCurvePoint first, last;
        int worstPoint = -1;
        const qreal worst = collapse(pts, a, b, toDocument, &first, &last, &worstPoint);
        if (worst > tolerance) return qBound(a + 1, worstPoint, b - 1);

        qreal arc = 0;
        QVector<qreal> along{0.0};
        for (int k = a; k < b; k++) {
            arc += segmentLength(pts[k], pts[k + 1], toDocument);
            along << arc;
        }

        // the values along the new segment, by length; a split goes where
        // they are most off. No attribute tolerance: the values don't count.
        qreal worstValue = 1.0;
        int worstValuePoint = -1;
        for (int k = a + 1; k < b && attributeTolerance > 0; k++) {
            const qreal f = arc > 0 ? along[k - a] / arc : 0.5;
            const qreal r = pts[a].radius + f * (pts[b].radius - pts[a].radius);
            const qreal scale = qMax(qMax(std::abs(r), std::abs(pts[k].radius)), 1e-9);
            qreal off = std::abs(r - pts[k].radius) / (attributeTolerance * scale);
            for (int index : attributeIndexes) {
                const qreal v = pts[a].attributes[index] + f * (pts[b].attributes[index] - pts[a].attributes[index]);
                off = qMax(off, std::abs(v - pts[k].attributes[index]) / attributeTolerance);
            }
            if (off > worstValue) {
                worstValue = off;
                worstValuePoint = k;
            }
        }
        if (worstValuePoint > 0) return worstValuePoint;

        pts[a].segment = first.segment;
        pts[a].handleOut = first.handleOut;
        pts[b].handleIn = last.handleIn;
        for (int k = a + 1; k < b; k++) keep[k] = false;
        return 0;
    }
};

/// the direction leaving a stroke at one end, from the end back along about \p reach
QPointF outward(const VCurveData &c, bool atStart, qreal reach)
{
    const int n = c.pointCount();
    const int end = atStart ? 0 : n - 1;
    const int step = atStart ? 1 : -1;
    qreal travelled = 0;
    int i = end;
    while (i + step >= 0 && i + step < n && travelled < reach) {
        travelled += norm(c.positions[i + step] - c.positions[i]);
        i += step;
    }
    return unit(c.positions[end] - c.positions[i]);
}

qreal curveLength(const VCurveData &c)
{
    qreal length = 0;
    for (int i = 0; i + 1 < c.pointCount(); i++) length += norm(c.positions[i + 1] - c.positions[i]);
    return length;
}

/// moves the points within \p portion (by length) of one end toward \p target, fully at the end
void pinch(VCurveData &c, bool atStart, const QPointF &target, qreal portion)
{
    const int n = c.pointCount();
    const int end = atStart ? 0 : n - 1;
    const int step = atStart ? 1 : -1;
    const QPointF delta = target - c.positions[end];
    qreal travelled = 0;
    for (int i = end; i >= 0 && i < n; i += step) {
        if (i != end) travelled += norm(c.positions[i] - c.positions[i - step]);
        if (portion <= 0 ? i != end : travelled >= portion) break;
        const qreal w = portion <= 0 ? 1.0 : smoothstep(1.0 - travelled / portion);
        c.positions[i] += w * delta;
        if (!c.handlesOut.isEmpty()) {
            c.handlesOut[i] += w * delta;
            c.handlesIn[i] += w * delta;
        }
    }
}

QStringList unitedNames(const QVector<VDocStroke> &strokes)
{
    QStringList names;
    for (const VDocStroke &s : strokes) {
        for (const QString &name : s.curve.attributes.keys()) {
            if (!names.contains(name)) names << name;
        }
    }
    names.sort();
    return names;
}

/// \p a then \p b (both one open sub-path), meeting at the middle of the gap with \p transition
VCurveData concatenate(VCurveData a, VCurveData b, qreal transition, const QStringList &names)
{
    const QPointF meet = (a.positions.last() + b.positions.first()) / 2;
    const bool merge = transition > 0;
    if (merge) {
        pinch(a, false, meet, transition * curveLength(a));
        pinch(b, true, meet, transition * curveLength(b));
    }

    VCurveBuilder builder(names);
    builder.beginCurve(false, (a.capFlags(0) & VCurveData::StartFlat) | (b.capFlags(0) & VCurveData::EndFlat));
    for (int i = 0; i < a.pointCount(); i++) {
        VCurvePoint p = a.point(i, names);
        if (i == a.pointCount() - 1) {
            if (merge) {
                // one point where the two met
                const VCurvePoint q = b.point(0, names);
                VCurvePoint mixed = VCurvePoint::lerp(p, q, 0.5);
                mixed.handleIn = p.handleIn;
                mixed.handleOut = q.handleOut;
                mixed.segment = q.segment;
                mixed.flags = p.flags | q.flags;
                p = mixed;
            } else {
                // a straight bridge over the gap
                p.segment = VCurveData::LineSegment;
                p.handleOut = p.position;
            }
        }
        builder.add(p);
    }
    for (int i = merge ? 1 : 0; i < b.pointCount(); i++) {
        builder.add(b.point(i, names));
    }
    return builder.take();
}

struct Components {
    QVector<int> parent;
    explicit Components(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    }
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        parent[a] = b;
        return true;
    }
};

/// a stroke resampled to about \p spacing, in document units, its values in the order of \p names
QVector<VCurvePoint> resample(const VCurveData &c, qreal spacing, const QStringList &names)
{
    QVector<VCurvePoint> out;
    const int n = c.pointCount();
    if (n == 0) return out;
    const int segmentCount = c.isCyclic(0) && n > 1 ? n : n - 1;
    for (int k = 0; k < segmentCount; k++) {
        const VCurvePoint a = c.point(k, names);
        const VCurvePoint b = c.point((k + 1) % n, names);
        const int parts = qMax(1, int(std::ceil(segmentLength(a, b, QTransform()) / spacing)));
        for (int s = 0; s < parts; s++) {
            const qreal t = qreal(s) / parts;
            VCurvePoint p = VCurvePoint::lerp(a, b, t);
            p.position = VCurvePoint::segmentPoint(a, b, t);
            p.handleIn = p.handleOut = p.position;
            p.segment = VCurveData::LineSegment;
            p.flags = 0;
            out << p;
        }
    }
    VCurvePoint last = c.point(c.isCyclic(0) && n > 1 ? 0 : n - 1, names);
    last.handleIn = last.handleOut = last.position;
    last.segment = VCurveData::LineSegment;
    last.flags = 0;
    out << last;
    return out;
}

QPointF tangentAt(const QVector<VCurvePoint> &pts, int i)
{
    const int a = qMax(0, i - 2);
    const int b = qMin(pts.size() - 1, i + 2);
    return unit(pts[b].position - pts[a].position);
}

/**
 * Whether \p a runs along \p b: the mean distance from a's points to b over
 * their shared part is within \p distance, the shared part is a fair share
 * of the shorter stroke, and the directions agree there.
 */
bool runsAlong(const QVector<VCurvePoint> &a, const QVector<VCurvePoint> &b, const VCurveFit::PointGrid &gridB,
               const QVector<QPointF> &positionsB, const VCurveSimplify::MergeOptions &options)
{
    qreal sum = 0;
    qreal alignment = 0;
    int shared = 0;
    for (int i = 0; i < a.size(); i++) {
        const QVector<int> near = gridB.near(a[i].position, 2 * options.distance);
        int best = -1;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        for (int j : near) {
            const qreal d = norm(positionsB[j] - a[i].position);
            if (d < bestDistance) {
                bestDistance = d;
                best = j;
            }
        }
        if (best < 0) continue;
        sum += bestDistance;
        alignment += std::abs(dot(tangentAt(a, i), tangentAt(b, best)));
        shared++;
    }
    const int needed = qMax(3, int(0.3 * qMin(a.size(), b.size())));
    if (shared < needed) return false;
    return sum / shared <= options.distance && alignment / shared >= std::cos(options.maxAngle);
}

/// one stroke from the points of a cluster
VCurveData fitCluster(const QVector<QVector<VCurvePoint>> &members, const VCurveSimplify::MergeOptions &options,
                      const QStringList &names, qreal spacing)
{
    QVector<VCurvePoint> all;
    QVector<QPointF> positions;
    for (const QVector<VCurvePoint> &m : members) {
        for (const VCurvePoint &p : m) {
            all << p;
            positions << p.position;
        }
    }

    const QVector<QVector<int>> tree = VCurveFit::spanningTree(positions, 8);
    const QVector<int> order = VCurveFit::longestPath(tree, positions);
    const VCurveFit::PointGrid grid(positions, options.distance);

    // to the middle of the nearby points, with their mean values
    QVector<VCurvePoint> path;
    for (int j = 0; j < order.size(); j++) {
        const QPointF p = positions[order[j]];
        const QPointF before = positions[order[qMax(0, j - 3)]];
        const QPointF after = positions[order[qMin(order.size() - 1, j + 3)]];
        const QPointF tangent = unit(after - before);
        const QPointF normal(-tangent.y(), tangent.x());
        const QVector<int> near = grid.near(p, options.distance);
        VCurvePoint mean = all[order[j]];
        qreal offset = 0;
        qreal radius = 0;
        QVector<qreal> attributes(names.size(), 0.0);
        for (int q : near) {
            offset += dot(positions[q] - p, normal);
            radius += all[q].radius;
            for (int k = 0; k < names.size(); k++) attributes[k] += all[q].attributes[k];
        }
        const int count = qMax(1, near.size());
        mean.position = p + normal * (offset / count);
        mean.radius = near.isEmpty() ? mean.radius : radius / count;
        if (!near.isEmpty()) {
            for (int k = 0; k < names.size(); k++) mean.attributes[k] = attributes[k] / count;
        }
        path << mean;
    }

    // the path zigzags between the strokes: keep the points that move on
    QVector<VCurvePoint> forward;
    for (int j = 0; j < path.size(); j++) {
        if (forward.isEmpty() || j == path.size() - 1) {
            forward << path[j];
            continue;
        }
        const QPointF direction = tangentAt(path, j);
        if (dot(path[j].position - forward.last().position, direction) > spacing * 0.5) forward << path[j];
    }
    if (forward.size() < 2) return VCurveData();

    // a light smoothing evens out what is left of the zigzag
    VCurveBuilder raw(names);
    raw.beginCurve();
    for (const VCurvePoint &p : forward) raw.add(p);
    VCurveData line = raw.take();
    VCurveWarp::smooth(line, QVector<qreal>(), 0.5, 2, VCurveWarp::Affect::Position | VCurveWarp::Affect::Radius);

    const VCurveFit::FitResult fit = VCurveFit::fitCubics(line.positions, options.fitError);
    VCurveBuilder builder(names);
    builder.beginCurve();
    for (int k = 0; k < fit.knots.size(); k++) {
        VCurvePoint p = line.point(fit.knots[k], names);
        p.handleIn = k > 0 ? fit.cubics[k - 1].c2 : p.position;
        if (k < fit.cubics.size()) {
            p.segment = VCurveData::CurveSegment;
            p.handleOut = fit.cubics[k].c1;
        } else {
            p.segment = VCurveData::LineSegment;
            p.handleOut = p.position;
        }
        p.flags = 0;
        builder.add(p);
    }
    return builder.take();
}

/// the merged line's radius pulled toward the set radius, then tapered along it
void adjustWidth(VCurveData &line, const VCurveSimplify::MergeOptions &options)
{
    if (line.isEmpty()) return;
    auto length = [] (const VCurveData &c, QVector<qreal> *along) {
        qreal total = 0;
        along->resize(c.pointCount());
        (*along)[0] = 0;
        for (int i = 0; i + 1 < c.pointCount(); i++) {
            const VCurvePoint a = c.point(i, QStringList());
            const VCurvePoint b = c.point(i + 1, QStringList());
            for (int s = 1; s <= 8; s++) {
                total += norm(VCurvePoint::segmentPoint(a, b, s / 8.0) - VCurvePoint::segmentPoint(a, b, (s - 1) / 8.0));
            }
            (*along)[i + 1] = total;
        }
        return total;
    };
    QVector<qreal> along;
    qreal total = length(line, &along);
    if (options.taper && total > 0) {
        // enough points for the taper to show between the few fitted knots
        line = line.subdivided(total / 32, QTransform());
        total = length(line, &along);
    }
    const qreal bias = qBound(0.0, options.bias, 1.0);
    for (int i = 0; i < line.pointCount(); i++) {
        // the radius at full pressure, as Thickness's Set: brush strokes keep their pressure tapers
        const qreal target = options.radius * line.attribute(QStringLiteral("pressure"), i, 1.0);
        qreal r = (1 - bias) * line.radii[i] + bias * target;
        if (options.taper) r *= qMax(0.0, options.taper(total > 0 ? along[i] / total : 0.0));
        line.radii[i] = r;
    }
}

}

VCurveData VCurveSimplify::thin(const VCurveData &d, const QTransform &toDocument, qreal tolerance,
                                qreal attributeTolerance, const QVector<bool> &mask)
{
    const QStringList names = d.attributeNames();
    VCurveBuilder builder(names);

    QVector<int> attributeIndexes;
    for (int k = 0; k < names.size(); k++) {
        // the time of samples isn't linear along the line, and nothing draws by it
        if (names[k] != QLatin1String("time")) attributeIndexes << k;
    }

    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int n = d.curveEnd(c) - begin;
        const bool cyclic = d.isCyclic(c) && n > 2;

        Thinner thinner;
        thinner.toDocument = toDocument;
        thinner.tolerance = tolerance;
        thinner.attributeTolerance = attributeTolerance;
        thinner.attributeIndexes = attributeIndexes;
        for (int k = 0; k < n + (cyclic ? 1 : 0); k++) {
            thinner.pts << d.point(begin + k % n, names);
        }
        const int m = thinner.pts.size();
        thinner.keep = QVector<bool>(m, true);

        auto inside = [&] (int k) {
            const int i = begin + (k % n);
            return mask.isEmpty() || (i < mask.size() && mask[i]);
        };
        QVector<bool> fixed(m, false);
        for (int k = 0; k < m; k++) {
            const int i = begin + (k % n);
            fixed[k] = k == 0 || k == m - 1 || !inside(k) || d.hasFlag(i, VCurveData::Dab) || VCurveWarp::isCorner(d, i);
            if (!fixed[k] && !mask.isEmpty()) {
                // the ends of a run stay, so nothing outside it moves
                fixed[k] = !inside(k - 1) || !inside(k + 1);
            }
        }

        int a = 0;
        for (int k = 1; k < m; k++) {
            if (!fixed[k]) continue;
            thinner.run(a, k);
            a = k;
        }

        if (cyclic) {
            // the closing copy of the first point carries its new incoming handle
            thinner.pts[0].handleIn = thinner.pts[m - 1].handleIn;
        }
        builder.beginCurve(d.isCyclic(c), d.capFlags(c));
        for (int k = 0; k < m - (cyclic ? 1 : 0); k++) {
            if (thinner.keep[k]) builder.add(thinner.pts[k]);
        }
    }
    return builder.take();
}

VCurveData VCurveSimplify::progressive(const VCurveData &d, const QTransform &toDocument, qreal share,
                                       const QVector<qreal> &weights)
{
    const QStringList names = d.attributeNames();
    VCurveBuilder builder(names);
    share = qBound(0.0, share, 1.0);
    auto weight = [&] (int i) { return weights.isEmpty() ? 1.0 : (i < weights.size() ? weights[i] : 0.0); };

    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int n = d.curveEnd(c) - begin;
        const bool cyclic = d.isCyclic(c) && n > 2;

        // as thin(): a closed sub-path gets a closing copy of its first point
        QVector<VCurvePoint> pts;
        for (int k = 0; k < n + (cyclic ? 1 : 0); k++) pts << d.point(begin + k % n, names);
        const int m = pts.size();
        auto index = [&] (int k) { return begin + k % n; };
        auto inside = [&] (int k) { return weight(index(k)) > 0; };

        // ends, dab points and the ends of each run under the brush stay
        QVector<bool> removable(m, false);
        for (int k = 1; k + 1 < m; k++) {
            removable[k] = inside(k) && inside(k - 1) && inside(k + 1) && !d.hasFlag(index(k), VCurveData::Dab);
        }
        QVector<int> previous(m), next(m);
        for (int k = 0; k < m; k++) {
            previous[k] = k - 1;
            next[k] = k + 1;
        }
        QVector<bool> kept(m, true);

        // how far the line moves when point k goes, from the line as it is now
        auto importance = [&] (int k, VCurvePoint *first, VCurvePoint *last) {
            const QVector<VCurvePoint> run{pts[previous[k]], pts[k], pts[next[k]]};
            int worstPoint;
            return collapse(run, 0, 2, toDocument, first, last, &worstPoint);
        };

        qreal weightSum = 0;
        int candidates = 0;
        for (int k = 0; k < m; k++) {
            if (!removable[k]) continue;
            weightSum += weight(index(k));
            candidates++;
        }
        // a closed sub-path keeps three points
        const int limit = cyclic ? qMax(0, n - 3) : candidates;
        const int count = candidates > 0 ? qMin(qMin(candidates, limit), qMax(1, int(std::ceil(share * weightSum)))) : 0;

        // least important first; an entry is stale once a neighbour went
        using Entry = std::pair<qreal, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
        QVector<int> stamp(m, 0);
        QVector<int> queuedStamp(m, -1);
        auto push = [&] (int k) {
            if (!removable[k] || !kept[k]) return;
            VCurvePoint first, last;
            queuedStamp[k] = stamp[k];
            queue.push({importance(k, &first, &last), k});
        };
        for (int k = 0; k < m; k++) push(k);

        int removed = 0;
        while (removed < count && !queue.empty()) {
            const Entry top = queue.top();
            queue.pop();
            const int k = top.second;
            if (!kept[k] || queuedStamp[k] != stamp[k]) continue;
            VCurvePoint first, last;
            importance(k, &first, &last);
            const int a = previous[k];
            const int b = next[k];
            pts[a].segment = first.segment;
            pts[a].handleOut = first.handleOut;
            pts[b].handleIn = last.handleIn;
            kept[k] = false;
            next[a] = b;
            previous[b] = a;
            removed++;
            for (int neighbour : {a, b}) {
                stamp[neighbour]++;
                push(neighbour);
            }
        }

        // an open sub-path down to the ends of its runs: the segments straighten
        bool anyLeft = false;
        for (int k = 0; k < m; k++) anyLeft |= removable[k] && kept[k];
        if (!cyclic && !anyLeft) {
            for (int a = 0; a + 1 < m; a = next[a]) {
                const int b = next[a];
                if (pts[a].segment != VCurveData::CurveSegment || !inside(a) || !inside(b)) continue;
                const qreal w = share * (weight(index(a)) + weight(index(b))) / 2;
                const QPointF chord = pts[b].position - pts[a].position;
                pts[a].handleOut += w * (pts[a].position + chord / 3 - pts[a].handleOut);
                pts[b].handleIn += w * (pts[b].position - chord / 3 - pts[b].handleIn);
                // straight enough is straight
                const qreal off = qMax(distanceToSegment(toDocument.map(pts[a].handleOut), toDocument.map(pts[a].position),
                                                         toDocument.map(pts[b].position)),
                                       distanceToSegment(toDocument.map(pts[b].handleIn), toDocument.map(pts[a].position),
                                                         toDocument.map(pts[b].position)));
                if (off < 0.01) {
                    pts[a].segment = VCurveData::LineSegment;
                    pts[a].handleOut = pts[a].position;
                    pts[b].handleIn = pts[b].position;
                }
            }
        }

        if (cyclic) pts[0].handleIn = pts[m - 1].handleIn;
        builder.beginCurve(d.isCyclic(c), d.capFlags(c));
        for (int k = 0; k < m - (cyclic ? 1 : 0); k++) {
            if (kept[k]) builder.add(pts[k]);
        }
    }
    return builder.take();
}

VCurveData VCurveSimplify::reversed(const VCurveData &c)
{
    const QStringList names = c.attributeNames();
    const int n = c.pointCount();
    const bool cyclic = c.curveCount() > 0 && c.isCyclic(0);
    QVector<VCurvePoint> pts;
    for (int j = 0; j < n; j++) {
        const int old = n - 1 - j;
        VCurvePoint p = c.point(old, names);
        std::swap(p.handleIn, p.handleOut);
        // the segment leaving the new point arrived at the old one
        const int arriving = old > 0 ? old - 1 : (cyclic ? n - 1 : -1);
        p.segment = arriving >= 0 ? c.segment(arriving) : VCurveData::LineSegment;
        pts << p;
    }
    const quint8 caps = c.capFlags(0);
    const quint8 swapped = ((caps & VCurveData::StartFlat) ? VCurveData::EndFlat : 0) |
                           ((caps & VCurveData::EndFlat) ? VCurveData::StartFlat : 0);
    VCurveBuilder builder(names);
    builder.beginCurve(cyclic, swapped);
    for (const VCurvePoint &p : pts) builder.add(p);
    return builder.take();
}

QVector<VDocStroke> VCurveSimplify::joinEnds(const QVector<VDocStroke> &strokes, const JoinOptions &options,
                                             const Compatible &compatible,
                                             const std::function<bool(const QPointF &)> &eligible)
{
    const int n = strokes.size();
    auto joinable = [&] (int s) {
        const VCurveData &c = strokes[s].curve;
        return c.curveCount() == 1 && !c.isCyclic(0) && c.pointCount() >= 2;
    };

    // end e of stroke s is index 2 * s + e (0: start, 1: finish)
    QVector<QPointF> endPositions(2 * n);
    QVector<QPointF> endDirections(2 * n);
    for (int s = 0; s < n; s++) {
        if (!joinable(s)) continue;
        const VCurveData &c = strokes[s].curve;
        const qreal reach = qMin(3 * options.gap, curveLength(c) / 3);
        endPositions[2 * s] = c.positions.first();
        endPositions[2 * s + 1] = c.positions.last();
        endDirections[2 * s] = outward(c, true, reach);
        endDirections[2 * s + 1] = outward(c, false, reach);
    }

    struct Pair {
        int e1, e2;
        qreal distance;
    };
    QVector<Pair> pairs;
    const VCurveFit::PointGrid grid(endPositions, qMax(options.gap, 1e-6));
    const qreal minCosine = std::cos(options.angleTolerance);
    for (int e1 = 0; e1 < 2 * n; e1++) {
        if (!joinable(e1 / 2) || (eligible && !eligible(endPositions[e1]))) continue;
        for (int e2 : grid.near(endPositions[e1], options.gap)) {
            if (e2 <= e1 || e2 / 2 == e1 / 2 || !joinable(e2 / 2)) continue;
            if (eligible && !eligible(endPositions[e2])) continue;
            if (compatible && !compatible(strokes[e1 / 2].owner, strokes[e2 / 2].owner)) continue;
            // one end leaves the way the other comes in
            if (dot(endDirections[e1], -endDirections[e2]) < minCosine) continue;
            pairs << Pair{e1, e2, norm(endPositions[e1] - endPositions[e2])};
        }
    }
    std::sort(pairs.begin(), pairs.end(), [] (const Pair &a, const Pair &b) { return a.distance < b.distance; });

    QVector<int> link(2 * n, -1);
    Components components(n);
    for (const Pair &p : pairs) {
        if (link[p.e1] >= 0 || link[p.e2] >= 0) continue;
        // no loops: a chain joins at most once with itself
        if (!components.unite(p.e1 / 2, p.e2 / 2)) continue;
        link[p.e1] = p.e2;
        link[p.e2] = p.e1;
    }

    const QStringList names = unitedNames(strokes);
    QVector<VDocStroke> result;
    QVector<bool> done(n, false);
    for (int s = 0; s < n; s++) {
        if (done[s]) continue;
        if (link[2 * s] < 0 && link[2 * s + 1] < 0) {
            result << strokes[s];
            done[s] = true;
            continue;
        }
        // a chain starts at a stroke with a free end
        if (link[2 * s] >= 0 && link[2 * s + 1] >= 0) continue;

        const int startEnd = link[2 * s] < 0 ? 0 : 1;  // the free end
        int stroke = s;
        int exitEnd = 1 - startEnd;
        VCurveData joined = startEnd == 0 ? strokes[s].curve : reversed(strokes[s].curve);
        done[s] = true;
        while (link[2 * stroke + exitEnd] >= 0) {
            const int entry = link[2 * stroke + exitEnd];
            stroke = entry / 2;
            const int entryEnd = entry % 2;
            const VCurveData next = entryEnd == 0 ? strokes[stroke].curve : reversed(strokes[stroke].curve);
            joined = concatenate(joined, next, options.transition, names);
            done[stroke] = true;
            exitEnd = 1 - entryEnd;
        }
        result << VDocStroke{joined, strokes[s].owner, true};
    }
    return result;
}

QVector<VDocStroke> VCurveSimplify::mergeOverlapping(const QVector<VDocStroke> &strokes, const MergeOptions &options,
                                                     const Compatible &compatible,
                                                     const std::function<bool(const VDocStroke &)> &eligible)
{
    const int n = strokes.size();
    const QStringList names = unitedNames(strokes);
    const qreal spacing = qMax(options.distance / 2, 1e-3);

    QVector<QVector<VCurvePoint>> samples(n);
    QVector<QVector<QPointF>> positions(n);
    QVector<QRectF> bounds(n);
    for (int s = 0; s < n; s++) {
        if (strokes[s].curve.curveCount() != 1) continue;
        samples[s] = resample(strokes[s].curve, spacing, names);
        for (const VCurvePoint &p : samples[s]) positions[s] << p.position;
        bounds[s] = QPolygonF(positions[s]).boundingRect().adjusted(-options.distance, -options.distance,
                                                                    options.distance, options.distance);
    }

    Components components(n);
    for (int a = 0; a < n; a++) {
        if (samples[a].size() < 2) continue;
        const VCurveFit::PointGrid gridA(positions[a], options.distance);
        for (int b = a + 1; b < n; b++) {
            if (samples[b].size() < 2 || !bounds[a].intersects(bounds[b])) continue;
            if (compatible && !compatible(strokes[a].owner, strokes[b].owner)) continue;
            const VCurveFit::PointGrid gridB(positions[b], options.distance);
            if (runsAlong(samples[a], samples[b], gridB, positions[b], options) ||
                runsAlong(samples[b], samples[a], gridA, positions[a], options)) {
                components.unite(a, b);
            }
        }
    }

    QHash<int, QVector<int>> clusters;
    for (int s = 0; s < n; s++) clusters[components.find(s)] << s;

    QVector<VDocStroke> result;
    QVector<bool> done(n, false);
    for (int s = 0; s < n; s++) {
        if (done[s]) continue;
        const QVector<int> &cluster = clusters[components.find(s)];
        bool wanted = cluster.size() > 1;
        if (wanted && eligible) {
            wanted = std::any_of(cluster.begin(), cluster.end(), [&] (int m) { return eligible(strokes[m]); });
        }
        VCurveData merged;
        if (wanted) {
            QVector<QVector<VCurvePoint>> members;
            for (int m : cluster) members << samples[m];
            merged = fitCluster(members, options, names, spacing);
            if (options.adjustWidth) adjustWidth(merged, options);
        }
        if (merged.isEmpty()) {
            result << strokes[s];
            done[s] = true;
            continue;
        }
        int longest = cluster.first();
        for (int m : cluster) {
            if (curveLength(strokes[m].curve) > curveLength(strokes[longest].curve)) longest = m;
            done[m] = true;
        }
        result << VDocStroke{merged, strokes[longest].owner, true};
    }
    return result;
}
