/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VPointEdit.h"

#include <limits>
#include <cmath>

#include <kis_algebra_2d.h>

#include "VCurveShape.h"

namespace {

/// the distance from \p p to the nearest of \p anchors, squared
qreal nearestDistance2(const QPointF &p, const QVector<QPointF> &anchors)
{
    qreal nearest2 = std::numeric_limits<qreal>::max();
    for (const QPointF &a : anchors) {
        const QPointF v = p - a;
        nearest2 = qMin(nearest2, v.x() * v.x() + v.y() * v.y());
    }
    return nearest2;
}

}

bool VPointEdit::Weights::movesAny() const
{
    return points.contains(1.0);
}

VPointEdit::Weights VPointEdit::weights(const VCurveData &d, const QVector<bool> &selected,
                                        const QTransform &toDocument, qreal radius, Profile profile)
{
    const int n = d.pointCount();
    const bool handles = d.hasCurveSegments();
    Weights w;
    w.points = QVector<qreal>(n, 0.0);
    if (handles) {
        w.handlesOut = QVector<qreal>(n, 0.0);
        w.handlesIn = QVector<qreal>(n, 0.0);
    }

    QVector<QPointF> anchors;
    for (int i = 0; i < n && i < selected.size(); i++) {
        if (selected[i]) {
            // the selected point's handles move with it, keeping its tangent
            w.points[i] = 1.0;
            if (handles) w.handlesOut[i] = w.handlesIn[i] = 1.0;
            anchors << toDocument.map(d.positions[i]);
        }
    }
    if (radius <= 0 || anchors.isEmpty()) return w;

    const qreal radius2 = radius * radius;
    auto weightAt = [&] (const QPointF &local) {
        const qreal nearest2 = nearestDistance2(toDocument.map(local), anchors);
        return nearest2 < radius2 ? falloff(profile, std::sqrt(nearest2), radius) : 0.0;
    };
    for (int i = 0; i < n; i++) {
        if (w.points[i] == 1.0) continue;
        w.points[i] = weightAt(d.positions[i]);
        if (handles) {
            w.handlesOut[i] = weightAt(d.handlesOut[i]);
            w.handlesIn[i] = weightAt(d.handlesIn[i]);
        }
    }
    return w;
}

void VPointEdit::move(VCurveData &d, const Weights &weights, const QPointF &delta)
{
    const int n = qMin(d.pointCount(), weights.points.size());
    for (int i = 0; i < n; i++) {
        if (weights.points[i] != 0.0) d.positions[i] += weights.points[i] * delta;
    }
    if (!d.hasCurveSegments() || weights.handlesOut.size() < n || weights.handlesIn.size() < n) return;
    for (int i = 0; i < n; i++) {
        if (weights.handlesOut[i] != 0.0) d.handlesOut[i] += weights.handlesOut[i] * delta;
        if (weights.handlesIn[i] != 0.0) d.handlesIn[i] += weights.handlesIn[i] * delta;
    }
}

void VPointEdit::scaleRadii(VCurveData &d, const Weights &weights, qreal factor)
{
    for (int i = 0; i < d.pointCount() && i < weights.points.size(); i++) {
        if (weights.points[i] == 0.0) continue;
        d.radii[i] *= 1.0 + weights.points[i] * (factor - 1.0);
    }
}

bool VPointEdit::hasHandle(const VCurveData &d, int index, HandleSide side)
{
    const int from = side == HandleSide::Out ? index : d.previousIndex(index);
    if (from < 0) return false;
    if (side == HandleSide::Out && d.nextIndex(index) < 0) return false;
    return d.segment(from) == VCurveData::CurveSegment;
}

QPointF VPointEdit::handle(const VCurveData &d, int index, HandleSide side)
{
    if (d.handlesOut.isEmpty()) return d.positions[index];
    return side == HandleSide::Out ? d.handlesOut[index] : d.handlesIn[index];
}

namespace {
QPointF &handleRef(VCurveData &d, int index, VPointEdit::HandleSide side)
{
    return side == VPointEdit::HandleSide::Out ? d.handlesOut[index] : d.handlesIn[index];
}

VPointEdit::HandleSide otherSide(VPointEdit::HandleSide side)
{
    return side == VPointEdit::HandleSide::Out ? VPointEdit::HandleSide::In : VPointEdit::HandleSide::Out;
}

qreal length(const QPointF &v)
{
    return std::sqrt(v.x() * v.x() + v.y() * v.y());
}

/// makes the segment leaving \p from a bezier that traces its line, as KoPathSegment::toCubic
void lineToCurve(VCurveData &d, int from)
{
    const int to = d.nextIndex(from);
    if (from < 0 || to < 0 || d.segment(from) == VCurveData::CurveSegment) return;
    const QPointF a = d.positions[from];
    const QPointF b = d.positions[to];
    d.setCurveSegment(from, a + (b - a) / 3.0, a + 2.0 * (b - a) / 3.0);
}

/// both handles on one line through the point, as KoPathPointTypeCommand; \p symmetric: equally long
void alignHandles(VCurveData &d, int index, bool symmetric)
{
    using VPointEdit::HandleSide;
    if (!VPointEdit::hasHandle(d, index, HandleSide::In) || !VPointEdit::hasHandle(d, index, HandleSide::Out)) return;
    const QPointF p = d.positions[index];
    QPointF in = d.handlesIn[index] - p;
    QPointF out = d.handlesOut[index] - p;
    const qreal inLength = length(in);
    const qreal outLength = length(out);
    if (inLength <= 1e-12 || outLength <= 1e-12) return;
    in /= inLength;
    out /= outLength;
    QPointF direction = 0.5 * (out - in);
    const qreal directionLength = length(direction);
    // handles pointing the same way: keep the outgoing one's direction
    direction = directionLength > 1e-9 ? direction / directionLength : out;
    const qreal average = 0.5 * (inLength + outLength);
    d.handlesIn[index] = p - direction * (symmetric ? average : inLength);
    d.handlesOut[index] = p + direction * (symmetric ? average : outLength);
}
}

void VPointEdit::moveHandle(VCurveData &d, int index, HandleSide side, const QPointF &position)
{
    if (!hasHandle(d, index, side)) return;
    handleRef(d, index, side) = position;
    const HandleSide other = otherSide(side);
    const bool smooth = d.hasFlag(index, VCurveData::SmoothPoint);
    const bool symmetric = d.hasFlag(index, VCurveData::SymmetricPoint);
    if ((!smooth && !symmetric) || !hasHandle(d, index, other)) return;
    const QPointF p = d.positions[index];
    const QPointF moved = position - p;
    const qreal movedLength = length(moved);
    if (movedLength <= 1e-12) return;
    const qreal otherLength = symmetric ? movedLength : length(handleRef(d, index, other) - p);
    handleRef(d, index, other) = p - moved / movedLength * otherLength;
}

void VPointEdit::setPointType(VCurveData &d, int index, PointType type)
{
    const int previous = d.previousIndex(index);
    switch (type) {
    case PointType::Corner:
        d.clearFlag(index, VCurveData::SmoothPoint);
        d.clearFlag(index, VCurveData::SymmetricPoint);
        break;
    case PointType::Smooth:
    case PointType::Symmetric:
        lineToCurve(d, previous);
        lineToCurve(d, index);
        d.clearFlag(index, type == PointType::Smooth ? VCurveData::SymmetricPoint : VCurveData::SmoothPoint);
        d.setFlag(index, type == PointType::Smooth ? VCurveData::SmoothPoint : VCurveData::SymmetricPoint);
        alignHandles(d, index, type == PointType::Symmetric);
        break;
    case PointType::Curve:
        lineToCurve(d, previous);
        lineToCurve(d, index);
        break;
    case PointType::Line:
        if (previous >= 0) d.setLineSegment(previous);
        if (d.nextIndex(index) >= 0) d.setLineSegment(index);
        d.clearFlag(index, VCurveData::SmoothPoint);
        d.clearFlag(index, VCurveData::SymmetricPoint);
        break;
    }
}

VPointEdit::PointType VPointEdit::pointType(const VCurveData &d, int index)
{
    if (d.hasFlag(index, VCurveData::SymmetricPoint)) return PointType::Symmetric;
    if (d.hasFlag(index, VCurveData::SmoothPoint)) return PointType::Smooth;
    return PointType::Corner;
}

int VPointEdit::nearestPoint(const VCurveData &d, const QTransform &toDocument, const QPointF &position,
                             qreal maxDistance)
{
    int best = -1;
    qreal bestDistance = maxDistance;
    for (int i = 0; i < d.pointCount(); i++) {
        const qreal distance = KisAlgebra2D::norm(toDocument.map(d.positions[i]) - position);
        if (distance <= bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

QVector<int> VPointEdit::pointsIn(const VCurveData &d, const QTransform &toDocument, const QRectF &rect)
{
    QVector<int> result;
    for (int i = 0; i < d.pointCount(); i++) {
        if (rect.contains(toDocument.map(d.positions[i]))) result << i;
    }
    return result;
}

QVector<int> VPointEdit::pointsIn(const VCurveData &d, const QTransform &toDocument, const QPainterPath &area)
{
    QVector<int> result;
    const QRectF bounds = area.boundingRect();
    for (int i = 0; i < d.pointCount(); i++) {
        const QPointF p = toDocument.map(d.positions[i]);
        if (bounds.contains(p) && area.contains(p)) result << i;
    }
    return result;
}

void VPointEdit::splitSelected(const VCurveData &curves, const QVector<bool> &selected,
                               VCurveData *picked, VCurveData *rest)
{
    const QStringList names = curves.attributeNames();
    VCurveBuilder pickedOut(names);
    VCurveBuilder restOut(names);
    auto isSelected = [&selected] (int index) { return index < selected.size() && selected[index]; };

    for (int c = 0; c < curves.curveCount(); c++) {
        const int begin = curves.curveBegin(c);
        const int n = curves.curveEnd(c) - begin;
        if (n == 0) continue;
        const bool cyclic = curves.isCyclic(c);
        const int segmentCount = cyclic ? n : n - 1;
        // segment k goes from point k to point k + 1 (wrapping on a closed sub-path)
        QVector<bool> segmentPicked(segmentCount);
        int pickedCount = 0;
        for (int k = 0; k < segmentCount; k++) {
            segmentPicked[k] = isSelected(begin + k) && isSelected(begin + (k + 1) % n);
            pickedCount += segmentPicked[k];
        }
        const bool whole = segmentCount == 0 ? isSelected(begin) : pickedCount == segmentCount;
        if (whole || pickedCount == 0) {
            (whole ? pickedOut : restOut).addCurve(curves, c);
            continue;
        }

        // runs of segments on one side; a closed sub-path starts where the sides change
        int start = 0;
        if (cyclic) {
            while (segmentPicked[start] == segmentPicked[(start + segmentCount - 1) % segmentCount]) start++;
        }
        const quint8 caps = curves.capFlags(c);
        int k = 0;
        while (k < segmentCount) {
            const bool side = segmentPicked[(start + k) % segmentCount];
            int end = k;
            while (end < segmentCount && segmentPicked[(start + end) % segmentCount] == side) end++;
            // points start + k .. start + end
            const bool atStart = !cyclic && k == 0;
            const bool atEnd = !cyclic && end == segmentCount;
            quint8 flags = 0;
            flags |= atStart ? (caps & VCurveData::StartFlat) : VCurveData::StartFlat;
            flags |= atEnd ? (caps & VCurveData::EndFlat) : VCurveData::EndFlat;
            VCurveBuilder &out = side ? pickedOut : restOut;
            out.beginCurve(false, flags);
            for (int j = k; j <= end; j++) {
                VCurvePoint p = curves.point(begin + (start + j) % n, names);
                if (j == k) p.handleIn = p.position;
                if (j == end) {
                    p.handleOut = p.position;
                    p.segment = VCurveData::LineSegment;
                }
                out.add(p);
            }
            k = end;
        }
    }
    if (picked) *picked = pickedOut.take();
    if (rest) *rest = restOut.take();
}

VCurveEditCommand::VCurveEditCommand(const QList<VCurveShape*> &shapes, const QList<VCurveData> &before,
                                     const QList<VCurveData> &after, const KUndo2MagicString &text,
                                     KUndo2Command *parent)
    : KUndo2Command(text, parent),
      m_shapes(shapes),
      m_before(before),
      m_after(after)
{
}

void VCurveEditCommand::redo()
{
    KUndo2Command::redo();
    for (int i = 0; i < m_shapes.size(); i++) {
        m_shapes[i]->setCurvesAndUpdate(m_after[i]);
    }
}

void VCurveEditCommand::undo()
{
    KUndo2Command::undo();
    for (int i = 0; i < m_shapes.size(); i++) {
        m_shapes[i]->setCurvesAndUpdate(m_before[i]);
    }
}

qreal VPointEdit::pointSpacing(const VCurveData &curves, int index, const QTransform &toView)
{
    if (index < 0 || index >= curves.pointCount()) return 0;
    const QPointF at = toView.map(curves.positions[index]);
    qreal sum = 0;
    int count = 0;
    for (const int neighbour : {curves.previousIndex(index), curves.nextIndex(index)}) {
        if (neighbour < 0 || neighbour == index) continue;
        sum += QLineF(at, toView.map(curves.positions[neighbour])).length();
        count++;
    }
    return count > 0 ? sum / count : 0;
}
