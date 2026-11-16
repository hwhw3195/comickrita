/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveWarp.h"

#include <QLineF>

#include <cmath>

namespace {
/// handles more than this far out of line make a corner
const qreal CornerAngle = M_PI / 9;

QPointF rotated(const QPointF &v, qreal cosine, qreal sine)
{
    return QPointF(v.x() * cosine - v.y() * sine, v.x() * sine + v.y() * cosine);
}

/**
 * The handles after smooth moved the points from \p positions: each bezier
 * keeps its shape, scaled and turned as its chord was. Handles kept as
 * they were would overshoot once the points pull together, and bend each
 * segment into a ridge. A point that didn't move keeps the direction of
 * its handles; one that moved has its two handles brought back in line.
 */
void followHandles(VCurveData &d, const QVector<QPointF> &positions, const QVector<QPointF> &handlesOut,
                   const QVector<QPointF> &handlesIn, const QVector<bool> &corner)
{
    const int n = d.pointCount();
    QVector<bool> moved(n, false);
    for (int i = 0; i < n; i++) moved[i] = d.positions[i] != positions[i];

    for (int i = 0; i < n; i++) {
        const int j = d.nextIndex(i);
        if (j < 0 || d.segment(i) != VCurveData::CurveSegment || (!moved[i] && !moved[j])) continue;
        const QPointF before = positions[j] - positions[i];
        const QPointF after = d.positions[j] - d.positions[i];
        const qreal lengthBefore = std::hypot(before.x(), before.y());
        const qreal lengthAfter = std::hypot(after.x(), after.y());
        const QPointF out = handlesOut[i] - positions[i];
        const QPointF in = handlesIn[j] - positions[j];
        if (lengthBefore < 1e-9 || lengthAfter < 1e-9) {
            d.handlesOut[i] = d.positions[i] + out;
            d.handlesIn[j] = d.positions[j] + in;
            continue;
        }
        const qreal scale = lengthAfter / lengthBefore;
        const qreal cosine = (before.x() * after.x() + before.y() * after.y()) / (lengthBefore * lengthAfter);
        const qreal sine = (before.x() * after.y() - before.y() * after.x()) / (lengthBefore * lengthAfter);
        d.handlesOut[i] = d.positions[i] + scale * (moved[i] ? rotated(out, cosine, sine) : out);
        d.handlesIn[j] = d.positions[j] + scale * (moved[j] ? rotated(in, cosine, sine) : in);
    }

    // the two segments at a moved point turned apart: in line again
    for (int i = 0; i < n; i++) {
        if (!moved[i] || corner[i]) continue;
        const int curve = d.curveOf(i);
        const int previous = i > d.curveBegin(curve) ? i - 1 : (d.isCyclic(curve) ? d.curveEnd(curve) - 1 : -1);
        if (previous < 0 || d.segment(previous) != VCurveData::CurveSegment || d.segment(i) != VCurveData::CurveSegment) {
            continue;
        }
        const QPointF in = d.positions[i] - d.handlesIn[i];
        const QPointF out = d.handlesOut[i] - d.positions[i];
        const qreal lin = std::hypot(in.x(), in.y());
        const qreal lout = std::hypot(out.x(), out.y());
        if (lin < 1e-9 || lout < 1e-9) continue;
        QPointF direction = in / lin + out / lout;
        const qreal l = std::hypot(direction.x(), direction.y());
        if (l < 1e-9) continue;
        direction /= l;
        d.handlesIn[i] = d.positions[i] - direction * lin;
        d.handlesOut[i] = d.positions[i] + direction * lout;
    }
}
}

QVector<qreal> VCurveWarp::brushWeights(const VCurveData &d, const QTransform &toDocument, const QPointF &center,
                                        qreal radius, VFalloff::Profile profile)
{
    QVector<qreal> weights(d.pointCount(), 0.0);
    for (int i = 0; i < d.pointCount(); i++) {
        const qreal distance = QLineF(toDocument.map(d.positions[i]), center).length();
        if (distance < radius) weights[i] = VFalloff::falloff(profile, distance, radius);
    }
    return weights;
}

bool VCurveWarp::isCorner(const VCurveData &d, int index)
{
    if (!d.hasCurveSegments()) return false;
    const int curve = d.curveOf(index);
    const int begin = d.curveBegin(curve);
    int previous = index - 1;
    if (index == begin) previous = d.isCyclic(curve) ? d.curveEnd(curve) - 1 : -1;
    if (previous < 0 || d.segment(previous) != VCurveData::CurveSegment || d.segment(index) != VCurveData::CurveSegment) {
        return false;
    }
    const QPointF in = d.positions[index] - d.handlesIn[index];
    const QPointF out = d.handlesOut[index] - d.positions[index];
    const qreal lin = std::hypot(in.x(), in.y());
    const qreal lout = std::hypot(out.x(), out.y());
    if (lin < 1e-9 || lout < 1e-9) return false;
    const qreal cosine = (in.x() * out.x() + in.y() * out.y()) / (lin * lout);
    return cosine < std::cos(CornerAngle);
}

void VCurveWarp::smooth(VCurveData &d, const QVector<qreal> &weights, qreal strength, int iterations, AffectFlags affect)
{
    const int n = d.pointCount();
    if (n < 3 || strength <= 0 || iterations <= 0) return;

    QVector<bool> fixed(n, false);
    for (int i = 0; i < n; i++) fixed[i] = isCorner(d, i);

    const bool pressure = (affect & Affect::Pressure) && d.attributes.contains(QStringLiteral("pressure"));
    QVector<qreal> *pressures = pressure ? &d.attributes[QStringLiteral("pressure")] : nullptr;

    const bool handles = (affect & Affect::Position) && d.hasCurveSegments();

    for (int iteration = 0; iteration < iterations; iteration++) {
        const QVector<QPointF> positions = d.positions;
        const QVector<qreal> radii = d.radii;
        const QVector<qreal> oldPressures = pressures ? *pressures : QVector<qreal>();
        const QVector<QPointF> handlesOut = d.handlesOut;
        const QVector<QPointF> handlesIn = d.handlesIn;

        for (int c = 0; c < d.curveCount(); c++) {
            const int begin = d.curveBegin(c);
            const int end = d.curveEnd(c);
            const bool cyclic = d.isCyclic(c);
            if (end - begin < 3) continue;
            for (int i = begin; i < end; i++) {
                const bool isEnd = !cyclic && (i == begin || i == end - 1);
                if (isEnd) continue;
                const qreal w = (weights.isEmpty() ? 1.0 : (i < weights.size() ? weights[i] : 0.0)) * strength;
                if (w <= 0) continue;
                const int previous = i == begin ? end - 1 : i - 1;
                const int next = i == end - 1 ? begin : i + 1;

                if ((affect & Affect::Position) && !fixed[i]) {
                    d.positions[i] = positions[i] + w * ((positions[previous] + positions[next]) / 2 - positions[i]);
                }
                if (affect & Affect::Radius) {
                    d.radii[i] = radii[i] + w * ((radii[previous] + radii[next]) / 2 - radii[i]);
                }
                if (pressures) {
                    (*pressures)[i] = oldPressures[i] + w * ((oldPressures[previous] + oldPressures[next]) / 2 - oldPressures[i]);
                }
            }
        }

        if (handles) followHandles(d, positions, handlesOut, handlesIn, fixed);
    }
}

namespace {

/// a point's values smoothAlong() averages
struct SmoothSample {
    QPointF position;
    qreal radius = 0.0;
    qreal pressure = 0.0;
};

SmoothSample operator*(qreal k, const SmoothSample &a) { return {k * a.position, k * a.radius, k * a.pressure}; }
SmoothSample operator+(const SmoothSample &a, const SmoothSample &b)
{
    return {a.position + b.position, a.radius + b.radius, a.pressure + b.pressure};
}
SmoothSample operator-(const SmoothSample &a, const SmoothSample &b)
{
    return {a.position - b.position, a.radius - b.radius, a.pressure - b.pressure};
}

/// the weighted sum of samples for one Gaussian
struct SmoothSum {
    SmoothSample value;
    qreal weight = 0.0;
    void add(const SmoothSample &v, qreal w)
    {
        value = value + w * v;
        weight += w;
    }
};

} // namespace

void VCurveWarp::smoothAlong(VCurveData &d, const QVector<qreal> &weights, qreal strength, qreal distance,
                             int iterations, AffectFlags affect, bool keepShape)
{
    const int n = d.pointCount();
    const qreal sigma = 0.5 * distance;
    if (n < 3 || strength <= 0 || iterations <= 0 || !(sigma > 0)) return;

    // corners stay, and the average stops at them as at the ends of open sub-paths
    QVector<bool> fixed(n, false);
    for (int i = 0; i < n; i++) fixed[i] = isCorner(d, i);

    const QString pressureName = QStringLiteral("pressure");
    const bool pressure = (affect & Affect::Pressure) && d.attributes.contains(pressureName);
    const bool handles = (affect & Affect::Position) && d.hasCurveSegments();
    // the wider Gaussian of keep shape, and how far either reaches
    const qreal sigma2 = keepShape ? sigma * std::sqrt(3.0) : sigma;
    const qreal reach = 3.0 * sigma2;
    const qreal a1 = -0.5 / (sigma * sigma);
    const qreal a2 = -0.5 / (sigma2 * sigma2);

    for (int iteration = 0; iteration < iterations; iteration++) {
        const QVector<QPointF> positions = d.positions;
        const QVector<QPointF> handlesOut = d.handlesOut;
        const QVector<QPointF> handlesIn = d.handlesIn;
        const QVector<qreal> radii = d.radii;
        const QVector<qreal> pressures = pressure ? d.attributes.value(pressureName) : QVector<qreal>();
        auto sampleAt = [&] (int i) {
            return SmoothSample{positions[i], radii[i], pressure ? pressures[i] : 0.0};
        };
        // the new values, written once the whole pass has read the old ones
        QVector<QPair<int, SmoothSample>> results;

        for (int c = 0; c < d.curveCount(); c++) {
            const int begin = d.curveBegin(c);
            const int end = d.curveEnd(c);
            const int count = end - begin;
            const bool cyclic = d.isCyclic(c);
            if (count < 3) continue;

            // the length of each segment k to k + 1 (closed: the last one back to the start)
            QVector<qreal> segment(count, 0.0);
            for (int k = 0; k < count; k++) {
                if (k == count - 1 && !cyclic) break;
                segment[k] = QLineF(positions[begin + k], positions[begin + (k + 1) % count]).length();
            }
            // the segment between k and its neighbour on side dir (-1 toward the start)
            auto step = [&] (int k, int dir) { return dir < 0 ? segment[(k - 1 + count) % count] : segment[k]; };
            // a barrier the average stops at: the ends of an open line, corners
            auto barrier = [&] (int k) { return (!cyclic && (k == 0 || k == count - 1)) || fixed[begin + k]; };
            bool anyBarrier = !cyclic;
            for (int k = 0; k < count && !anyBarrier; k++) anyBarrier = fixed[begin + k];
            // the length of line a point stands for, half of each segment at it, so a
            // dense stretch doesn't count for more than a sparse one
            auto span = [&] (int k) {
                const qreal before = (k > 0 || cyclic) ? step(k, -1) : 0.0;
                const qreal after = (k < count - 1 || cyclic) ? step(k, 1) : 0.0;
                return 0.5 * (before + after);
            };

            for (int k = 0; k < count; k++) {
                const int i = begin + k;
                if (!cyclic && (k == 0 || k == count - 1)) continue;
                const qreal w = (weights.isEmpty() ? 1.0 : (i < weights.size() ? weights[i] : 0.0)) * strength;
                if (w <= 0) continue;

                SmoothSum g1, g2;
                auto add = [&] (const SmoothSample &v, qreal dist, qreal spanWeight) {
                    if (dist > reach || spanWeight <= 0) return;
                    g1.add(v, std::exp(a1 * dist * dist) * spanWeight);
                    if (keepShape) g2.add(v, std::exp(a2 * dist * dist) * spanWeight);
                };
                const SmoothSample self = sampleAt(i);
                add(self, 0.0, qMax(span(k), 1e-12));

                // one side, then the other: dir -1 toward the start, +1 toward the end
                for (int dir : {-1, 1}) {
                    int steps = 0;
                    int j = k;
                    qreal dist = 0.0;
                    bool mirrored = false;
                    while (true) {
                        int next = j + dir;
                        if (cyclic) next = (next + count) % count;
                        else if (next < 0 || next >= count) break;
                        // closed with no barrier: each side covers half the loop, so no point counts twice
                        if (cyclic && !anyBarrier && ++steps > count / 2) break;
                        dist += step(j, dir);
                        if (dist > reach) break;
                        add(sampleAt(begin + next), dist, span(next));
                        j = next;
                        if (barrier(j)) {
                            mirrored = true;
                            break;
                        }
                    }
                    if (!mirrored) continue;
                    // past the barrier b the line goes on mirrored through it: the point at
                    // distance t this side of b stands at t beyond it, with the value 2 b - it
                    const int b = j;
                    const qreal toBarrier = dist;
                    const SmoothSample atBarrier = sampleAt(begin + b);
                    int m = b;
                    qreal t = 0.0;
                    while (true) {
                        int next = m - dir;
                        if (cyclic) next = (next + count) % count;
                        else if (next < 0 || next >= count) break;
                        t += step(m, -dir);
                        if (toBarrier + t > reach) break;
                        add(2.0 * atBarrier - sampleAt(begin + next), toBarrier + t, span(next));
                        m = next;
                        if (barrier(m)) break;
                    }
                }

                if (g1.weight <= 0) continue;
                SmoothSample target = (1.0 / g1.weight) * g1.value;
                if (keepShape && g2.weight > 0) target = 1.5 * target - 0.5 * ((1.0 / g2.weight) * g2.value);
                results.append({i, self + w * (target - self)});
            }
        }

        for (const auto &r : results) {
            const int i = r.first;
            if ((affect & Affect::Position) && !fixed[i]) d.positions[i] = r.second.position;
            if (affect & Affect::Radius) d.radii[i] = qMax(0.0, r.second.radius);
            if (pressure) d.attributes[pressureName][i] = qBound(0.0, r.second.pressure, 1.0);
        }

        if (handles) followHandles(d, positions, handlesOut, handlesIn, fixed);
    }
}

namespace {

void translatePoints(QVector<QPointF> &points, const QVector<QPointF> &original, const VCurveWarp::Dab &dab,
                     const QTransform &toImage, const QTransform &toLocal)
{
    const qreal maxDistance = 3.0 * dab.sigma;
    for (int i = 0; i < points.size(); i++) {
        const QPointF now = toImage.map(points[i]);
        QPointF moved;
        if (dab.wash) {
            const QPointF from = toImage.map(original[i]);
            const qreal distance = QLineF(from, dab.position).length();
            if (distance > maxDistance) continue;
            const qreal lambda = std::exp(-0.5 * std::pow(distance / dab.sigma, 2));
            const QPointF target = from + lambda * dab.offset;
            if (QLineF(target, from).length() <= QLineF(now, from).length()) continue;
            moved = (1.0 - dab.flow) * now + dab.flow * target;
        } else {
            const qreal distance = QLineF(now, dab.position).length();
            if (distance > maxDistance) continue;
            const qreal lambda = std::exp(-0.5 * std::pow(distance / dab.sigma, 2));
            moved = now + lambda * dab.offset;
        }
        points[i] = toLocal.map(moved);
    }
}

}

void VCurveWarp::translate(VCurveData &d, const Dab &dab, const QTransform &toImage, const VCurveData &snapshot)
{
    if (dab.sigma <= 0) return;
    const QTransform toLocal = toImage.inverted();
    translatePoints(d.positions, snapshot.positions, dab, toImage, toLocal);
    if (d.hasCurveSegments() && snapshot.handlesOut.size() == d.handlesOut.size()) {
        translatePoints(d.handlesOut, snapshot.handlesOut, dab, toImage, toLocal);
        translatePoints(d.handlesIn, snapshot.handlesIn, dab, toImage, toLocal);
    }
}

VCurveData VCurveWarp::translated(const VCurveData &snapshot, const QVector<Dab> &dabs, const QTransform &toImage)
{
    VCurveData d = snapshot;
    translateFrom(d, dabs, 0, toImage, snapshot);
    return d;
}

void VCurveWarp::translateFrom(VCurveData &d, const QVector<Dab> &dabs, int from, const QTransform &toImage,
                               const VCurveData &snapshot)
{
    // where the curves can be reached by a dab at all
    const QRectF snapshotBounds = toImage.mapRect(snapshot.controlBounds());
    QRectF bounds = toImage.mapRect(d.controlBounds());
    for (int i = qMax(0, from); i < dabs.size(); i++) {
        const Dab &dab = dabs[i];
        const qreal reach = 3.0 * dab.sigma;
        const QRectF area(dab.position - QPointF(reach, reach), QSizeF(2 * reach, 2 * reach));
        const QRectF &tested = dab.wash ? snapshotBounds : bounds;
        if (area.right() < tested.left() || tested.right() < area.left() ||
            area.bottom() < tested.top() || tested.bottom() < area.top()) {
            continue;
        }
        translate(d, dab, toImage, snapshot);
        bounds = toImage.mapRect(d.controlBounds());
    }
}
