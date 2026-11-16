/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QElapsedTimer>
#include <QPainterPathStroker>
#include <QtMath>

#include <kistest.h>

#include <KoColorBackground.h>
#include <KoPathPoint.h>
#include <KoPathShape.h>

#include "VCurveOutline.h"
#include "VPointEdit.h"
#include "VPointEditTool.h"
#include "VStrokeCurves.h"
#include "VStrokeShape.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

/**
 * Point editing of freehand curves: points move as they are with a
 * falloff, handles follow, radius edits widen the brush, and undo restores
 * the arrays bit-exact. Also times a move on a 2000-point stroke.
 */
class VPointEditTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testFalloffMovesOnlyNearPoints();
    void testHandlesFollowPoints();
    void testHandlesBendWithFalloff();
    void testFalloffProfiles();
    void testUndoRestoresExactly();
    void testRadiusEditWidens();
    void testPicking();
    void benchmarkMovePoint();
    void testHandlesOfAPoint();
    void testHandlesShowWherePointsAreSparse();
    void testMoveHandleSmoothAndSymmetric();
    void testPointTypeCurveTracesTheLine();
    void testPointTypeLineDropsHandles();
    void testSplitSelected();
    void testSplitSelectedClosedAndBezier();
};

namespace {

/// a dense brush stroke: 2000 samples along a wave, in image pixels
QVector<VStrokePrimitive> denseStroke(int samples = 2000)
{
    QVector<VStrokePrimitive> stroke;
    KisPaintInformation prev = info(QPointF(10, 100), 0.5, 0);
    for (int i = 1; i < samples; i++) {
        KisPaintInformation next = info(QPointF(10 + i * 0.5, 100 + 20 * qSin(i * 0.01)), 0.3 + 0.4 * qAbs(qSin(i * 0.003)), i);
        stroke << VStrokePrimitive::line(prev, next);
        prev = next;
    }
    return stroke;
}

VStrokeShape *strokeShape(const QVector<VStrokePrimitive> &primitives)
{
    VStrokeData settings;
    settings.brushSize = 12;
    VStrokeShape *shape = VStrokeShape::fromImagePixels(primitives, 1.0, settings);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::black)));
    return shape;
}

}

void VPointEditTest::testFalloffMovesOnlyNearPoints()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(denseStroke()));
    const VCurveData before = shape->curves();
    QCOMPARE(before.pointCount(), 2000);

    QVector<bool> selected(before.pointCount(), false);
    selected[1000] = true;
    const QTransform toDocument = shape->absoluteTransformation();
    const VPointEdit::Weights w = VPointEdit::weights(before, selected, toDocument, 20.0);

    VCurveData after = before;
    VPointEdit::move(after, w, QPointF(0, 10));
    shape->setCurves(after);

    int moved = 0;
    const QPointF anchor = toDocument.map(before.positions[1000]);
    for (int i = 0; i < before.pointCount(); i++) {
        const qreal distance = QLineF(toDocument.map(before.positions[i]), anchor).length();
        const QPointF offset = shape->curves().positions[i] - before.positions[i];
        if (distance >= 20.0) {
            // untouched, bit for bit
            QCOMPARE(shape->curves().positions[i], before.positions[i]);
        } else {
            moved++;
            QVERIFY(offset.y() > 0 && offset.y() <= 10.0);
            QCOMPARE(offset.x(), 0.0);
        }
    }
    QCOMPARE(shape->curves().positions[1000], before.positions[1000] + QPointF(0, 10));
    // the samples within 20 px on either side (0.5 px apart), and nothing resampled
    QVERIFY(moved > 50 && moved < 90);
    QCOMPARE(shape->curves().pointCount(), before.pointCount());
    QCOMPARE(shape->curves().radii, before.radii);

    // no falloff: the selected point only
    VCurveData single = before;
    VPointEdit::move(single, VPointEdit::weights(before, selected, toDocument, 0.0), QPointF(3, 0));
    QCOMPARE(single.positions[999], before.positions[999]);
    QCOMPARE(single.positions[1000], before.positions[1000] + QPointF(3, 0));
}

void VPointEditTest::testHandlesFollowPoints()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(100, 0), 1);
    d.addPoint(QPointF(200, 0), 1);
    d.setCurveSegment(0, QPointF(30, 40), QPointF(70, 40));
    d.setCurveSegment(1, QPointF(130, -40), QPointF(170, -40));

    // no falloff: the selected point's handles only
    const VPointEdit::Weights w = VPointEdit::weights(d, {false, true, false}, QTransform(), 0.0);
    VPointEdit::move(d, w, QPointF(5, 7));
    QCOMPARE(d.positions[1], QPointF(105, 7));
    // the handle arriving at the point and the one leaving it
    QCOMPARE(d.handlesIn[1], QPointF(75, 47));
    QCOMPARE(d.handlesOut[1], QPointF(135, -33));
    // the other points' handles stay
    QCOMPARE(d.handlesOut[0], QPointF(30, 40));
    QCOMPARE(d.handlesIn[2], QPointF(170, -40));
}

void VPointEditTest::testHandlesBendWithFalloff()
{
    // a straight line of bezier segments, handles on the line
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < 5; i++) d.addPoint(QPointF(100 * i, 0), 1);
    for (int i = 0; i < 4; i++) {
        d.setCurveSegment(i, QPointF(100 * i + 100.0 / 3, 0), QPointF(100 * i + 200.0 / 3, 0));
    }
    const VCurveData before = d;

    const qreal radius = 250;
    const QPointF delta(0, 30);
    QVector<bool> selected(5, false);
    selected[2] = true;
    VPointEdit::move(d, VPointEdit::weights(d, selected, QTransform(), radius), delta);

    // the curve follows the falloff bump: each sample of the original
    // moved by the falloff at its place, within a third of a point. Handles
    // moving with their own point instead would be off by 2.5 points.
    auto bezier = [] (qreal a, qreal b, qreal c, qreal e, qreal t) {
        return (1 - t) * (1 - t) * (1 - t) * a + 3 * (1 - t) * (1 - t) * t * b + 3 * (1 - t) * t * t * c + t * t * t * e;
    };
    qreal worst = 0;
    for (int s = 0; s < 4; s++) {
        for (int k = 0; k <= 100; k++) {
            const qreal t = k / 100.0;
            const qreal x0 = bezier(before.positions[s].x(), before.handlesOut[s].x(),
                                    before.handlesIn[s + 1].x(), before.positions[s + 1].x(), t);
            const qreal y = bezier(d.positions[s].y(), d.handlesOut[s].y(), d.handlesIn[s + 1].y(),
                                   d.positions[s + 1].y(), t);
            const qreal ideal = delta.y() * VPointEdit::falloff(VPointEdit::Profile::Smooth, qAbs(x0 - 200), radius);
            worst = qMax(worst, qAbs(y - ideal));
        }
    }
    QVERIFY2(worst < 0.4, qPrintable(QString("off by %1").arg(worst)));
    // the dragged point keeps its tangent: its handles move with it
    QCOMPARE(d.handlesIn[2] - d.positions[2], before.handlesIn[2] - before.positions[2]);
    QCOMPARE(d.handlesOut[2] - d.positions[2], before.handlesOut[2] - before.positions[2]);
}

void VPointEditTest::testFalloffProfiles()
{
    using P = VPointEdit::Profile;
    for (P profile : {P::Smooth, P::Sphere, P::Root, P::InverseSquare, P::Sharp, P::Linear, P::Constant}) {
        QCOMPARE(VPointEdit::falloff(profile, 0.0, 10.0), 1.0);
        QCOMPARE(VPointEdit::falloff(profile, 10.0, 10.0), 0.0);
        QCOMPARE(VPointEdit::falloff(profile, 12.0, 10.0), 0.0);
        const qreal half = VPointEdit::falloff(profile, 5.0, 10.0);
        QVERIFY(half > 0.0 && half <= 1.0);
        // never grows with distance
        QVERIFY(VPointEdit::falloff(profile, 7.0, 10.0) <= half);
    }
    QCOMPARE(VPointEdit::falloff(P::Linear, 5.0, 10.0), 0.5);
    QCOMPARE(VPointEdit::falloff(P::Smooth, 5.0, 10.0), 0.5);
    QCOMPARE(VPointEdit::falloff(P::Sharp, 5.0, 10.0), 0.25);
    // no falloff radius: nothing follows
    QCOMPARE(VPointEdit::falloff(P::Constant, 0.0, 0.0), 0.0);
}

void VPointEditTest::testUndoRestoresExactly()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(denseStroke(300)));
    const VCurveData before = shape->curves();

    QVector<bool> selected(before.pointCount(), false);
    selected[100] = selected[200] = true;
    const VPointEdit::Weights w = VPointEdit::weights(before, selected, shape->absoluteTransformation(), 15.0);
    VCurveData after = before;
    VPointEdit::move(after, w, QPointF(4.25, -3.5));
    VPointEdit::scaleRadii(after, w, 1.7);

    VCurveEditCommand command({shape.data()}, {before}, {after}, kundo2_noi18n("edit"));
    command.redo();
    QVERIFY(shape->curves() == after);
    QVERIFY(shape->curves() != before);
    command.undo();
    QVERIFY(shape->curves() == before);
    command.redo();
    QVERIFY(shape->curves() == after);
}

void VPointEditTest::testRadiusEditWidens()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(testStroke()));
    const QRectF before = shape->outlineRect();

    VCurveData wider = shape->curves();
    VPointEdit::Weights all;
    all.points = QVector<qreal>(wider.pointCount(), 1.0);
    VPointEdit::scaleRadii(wider, all, 2.0);
    shape->setCurves(wider);

    QVERIFY(shape->outlineRect().height() > before.height() + 5);
    // the brush paints the wider line: dabs twice the size
    const QVector<VStrokePrimitive> replay = shape->imagePixelPrimitives();
    QCOMPARE(replay[2].pi1.sizeMultiplier(), 2.0);
}

void VPointEditTest::testPicking()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(50, 0), 1);
    d.addPoint(QPointF(100, 0), 1);
    const QTransform toDocument = QTransform::fromTranslate(10, 20);

    QCOMPARE(VPointEdit::nearestPoint(d, toDocument, QPointF(62, 21), 5.0), 1);
    QCOMPARE(VPointEdit::nearestPoint(d, toDocument, QPointF(62, 30), 5.0), -1);
    QCOMPARE(VPointEdit::pointsIn(d, toDocument, QRectF(40, 10, 100, 20)), QVector<int>({1, 2}));

    // the ellipse and lasso / polygon selection shapes
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(30, 0, 60, 40));
    QCOMPARE(VPointEdit::pointsIn(d, toDocument, ellipse), QVector<int>({1}));
    QPainterPath triangle;
    triangle.addPolygon(QPolygonF({QPointF(0, 0), QPointF(80, 0), QPointF(0, 40)}));
    triangle.closeSubpath();
    QCOMPARE(VPointEdit::pointsIn(d, toDocument, triangle), QVector<int>({0}));
}

void VPointEditTest::benchmarkMovePoint()
{
    const int iterations = 50;
    QScopedPointer<VStrokeShape> shape(strokeShape(denseStroke()));
    VCurveData d = shape->curves();
    QVector<bool> selected(d.pointCount(), false);
    selected[1000] = true;
    const VPointEdit::Weights w = VPointEdit::weights(d, selected, QTransform(), 0.0);

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < iterations; i++) {
        VPointEdit::move(d, w, QPointF(0, 0.1));
        const QPainterPath outline = VCurveOutline::line(d, VCurveStrokeStyle(), 1.0, 0.5, 0.25);
        QVERIFY(!outline.isEmpty());
    }
    const qreal curveMs = timer.nsecsElapsed() / 1e6 / iterations;

    // the same points as a stock path, with its constant-width stroke outline
    QScopedPointer<KoPathShape> path(new KoPathShape());
    path->moveTo(d.positions[0]);
    for (int i = 1; i < d.pointCount(); i++) path->lineTo(d.positions[i]);
    KoPathPoint *point = path->pointByIndex(KoPathPointIndex(0, 1000));
    QPainterPathStroker stroker;
    stroker.setWidth(6);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);

    timer.restart();
    for (int i = 0; i < iterations; i++) {
        point->setPoint(point->point() + QPointF(0, 0.1));
        const QPainterPath outline = stroker.createStroke(path->outline());
        QVERIFY(!outline.isEmpty());
    }
    const qreal pathMs = timer.nsecsElapsed() / 1e6 / iterations;

    fprintf(stderr, "BENCHMARK move one point of 2000 and rebuild the outline: curves %.3f ms, stock path %.3f ms\n",
            curveMs, pathMs);
}

namespace {
/// three points along a line, all line segments
VCurveData threePoints(bool cyclic = false)
{
    VCurveData d;
    d.beginCurve(cyclic);
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(30, 0), 1);
    d.addPoint(QPointF(60, 30), 1);
    return d;
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}
}

void VPointEditTest::testHandlesOfAPoint()
{
    VCurveData d = threePoints();
    using Side = VPointEdit::HandleSide;
    // lines have no handles
    QVERIFY(!VPointEdit::hasHandle(d, 1, Side::In));
    QVERIFY(!VPointEdit::hasHandle(d, 1, Side::Out));
    d.setCurveSegment(1, QPointF(40, 0), QPointF(60, 10));
    QVERIFY(!VPointEdit::hasHandle(d, 1, Side::In));
    QVERIFY(VPointEdit::hasHandle(d, 1, Side::Out));
    QVERIFY(VPointEdit::hasHandle(d, 2, Side::In));
    QCOMPARE(VPointEdit::handle(d, 1, Side::Out), QPointF(40, 0));
    QCOMPARE(VPointEdit::handle(d, 2, Side::In), QPointF(60, 10));
    // the ends of an open line: nothing before the first, nothing after the last
    QVERIFY(!VPointEdit::hasHandle(d, 0, Side::In));
    QVERIFY(!VPointEdit::hasHandle(d, 2, Side::Out));
    // a closed one wraps
    VCurveData c = threePoints(true);
    c.setCurveSegment(2, QPointF(40, 40), QPointF(-10, 10));
    QVERIFY(VPointEdit::hasHandle(c, 0, Side::In));
    QVERIFY(VPointEdit::hasHandle(c, 2, Side::Out));
    QCOMPARE(c.previousIndex(0), 2);
}

void VPointEditTest::testHandlesShowWherePointsAreSparse()
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < 5; i++) d.addPoint(QPointF(10 * i, 0), 1);
    // a middle point: the mean of both neighbours; an end: its one neighbour
    QCOMPARE(VPointEdit::pointSpacing(d, 2, QTransform()), 10.0);
    QCOMPARE(VPointEdit::pointSpacing(d, 0, QTransform::fromScale(2, 2)), 20.0);
    d.positions[3] = QPointF(40, 0);
    QCOMPARE(VPointEdit::pointSpacing(d, 2, QTransform()), 15.0);

    const qreal threshold = VPointEditTool::MinHandleSpacing;
    QVERIFY(!VPointEditTool::showsHandles(d, 1, QTransform::fromScale(threshold / 20, threshold / 20)));
    QVERIFY(VPointEditTool::showsHandles(d, 1, QTransform::fromScale(threshold / 10, threshold / 10)));
}

void VPointEditTest::testMoveHandleSmoothAndSymmetric()
{
    using Side = VPointEdit::HandleSide;
    VCurveData corner = threePoints();
    VPointEdit::setPointType(corner, 1, VPointEdit::PointType::Curve);
    const QPointF inBefore = corner.handlesIn[1];
    VPointEdit::moveHandle(corner, 1, Side::Out, QPointF(30, 20));
    QCOMPARE(corner.handlesOut[1], QPointF(30, 20));
    QCOMPARE(corner.handlesIn[1], inBefore);  // a corner: the other stays

    VCurveData smooth = threePoints();
    VPointEdit::setPointType(smooth, 1, VPointEdit::PointType::Smooth);
    QCOMPARE(VPointEdit::pointType(smooth, 1), VPointEdit::PointType::Smooth);
    const QPointF p = smooth.positions[1];
    // setting the type lines the handles up
    QVERIFY(qAbs(cross(smooth.handlesIn[1] - p, smooth.handlesOut[1] - p)) < 1e-9);
    const qreal inLength = QLineF(p, smooth.handlesIn[1]).length();
    VPointEdit::moveHandle(smooth, 1, Side::Out, QPointF(30, 20));
    QVERIFY(qAbs(cross(smooth.handlesIn[1] - p, smooth.handlesOut[1] - p)) < 1e-9);
    QVERIFY(QPointF::dotProduct(smooth.handlesIn[1] - p, smooth.handlesOut[1] - p) < 0);
    QVERIFY(qAbs(QLineF(p, smooth.handlesIn[1]).length() - inLength) < 1e-9);

    VCurveData symmetric = threePoints();
    VPointEdit::setPointType(symmetric, 1, VPointEdit::PointType::Symmetric);
    QCOMPARE(VPointEdit::pointType(symmetric, 1), VPointEdit::PointType::Symmetric);
    VPointEdit::moveHandle(symmetric, 1, Side::In, QPointF(30, -12));
    QVERIFY(QLineF(symmetric.handlesOut[1], QPointF(30, 12)).length() < 1e-9);

    // back to a corner keeps the handles where they are
    VPointEdit::setPointType(symmetric, 1, VPointEdit::PointType::Corner);
    QCOMPARE(VPointEdit::pointType(symmetric, 1), VPointEdit::PointType::Corner);
    QVERIFY(QLineF(symmetric.handlesOut[1], QPointF(30, 12)).length() < 1e-9);
    QVERIFY(symmetric.flags.isEmpty());
}

void VPointEditTest::testPointTypeCurveTracesTheLine()
{
    VCurveData d = threePoints();
    VPointEdit::setPointType(d, 1, VPointEdit::PointType::Curve);
    QCOMPARE(d.segment(0), VCurveData::CurveSegment);
    QCOMPARE(d.segment(1), VCurveData::CurveSegment);
    // still the same straight lines
    for (int s = 0; s < 2; s++) {
        const VCurvePoint a = d.point(s, QStringList());
        const VCurvePoint b = d.point(s + 1, QStringList());
        for (qreal t : {0.25, 0.5, 0.75}) {
            const QPointF onLine = a.position + t * (b.position - a.position);
            QVERIFY(QLineF(VCurvePoint::segmentPoint(a, b, t), onLine).length() < 1e-9);
        }
    }
}

void VPointEditTest::testPointTypeLineDropsHandles()
{
    VCurveData d = threePoints();
    VPointEdit::setPointType(d, 1, VPointEdit::PointType::Smooth);
    QVERIFY(d.hasCurveSegments());
    VPointEdit::setPointType(d, 1, VPointEdit::PointType::Line);
    // as built from lines: no handle arrays, no flags
    QVERIFY(d == threePoints());
}

namespace {
/// \p count points along x, 10 apart, radius 2
VCurveData row(int count, bool cyclic = false, qreal y = 0)
{
    VCurveData d;
    d.beginCurve(cyclic);
    for (int i = 0; i < count; i++) d.addPoint(QPointF(i * 10, y + (cyclic && i % 2 ? 10 : 0)), 2);
    return d;
}

QVector<bool> mask(int count, const QVector<int> &on)
{
    QVector<bool> m(count, false);
    for (int i : on) m[i] = true;
    return m;
}
}

void VPointEditTest::testSplitSelected()
{
    VCurveData picked, rest;

    // a stretch in the middle: both sides keep its end points, the new ends are flat
    const VCurveData line = row(10);
    VPointEdit::splitSelected(line, mask(10, {3, 4, 5, 6}), &picked, &rest);
    QCOMPARE(picked.curveCount(), 1);
    QCOMPARE(picked.pointCount(), 4);
    QCOMPARE(picked.positions.first(), QPointF(30, 0));
    QCOMPARE(picked.positions.last(), QPointF(60, 0));
    QCOMPARE(int(picked.capFlags(0)), int(VCurveData::StartFlat | VCurveData::EndFlat));
    QCOMPARE(rest.curveCount(), 2);
    QCOMPARE(rest.pointCount(), 4 + 4);
    QCOMPARE(rest.positions[rest.curveEnd(0) - 1], QPointF(30, 0));
    QCOMPARE(rest.positions[rest.curveBegin(1)], QPointF(60, 0));
    QCOMPARE(int(rest.capFlags(0)), int(VCurveData::EndFlat));
    QCOMPARE(int(rest.capFlags(1)), int(VCurveData::StartFlat));

    // from the start: the start keeps its cap
    VPointEdit::splitSelected(line, mask(10, {0, 1, 2}), &picked, &rest);
    QCOMPARE(picked.pointCount(), 3);
    QCOMPARE(int(picked.capFlags(0)), int(VCurveData::EndFlat));
    QCOMPARE(rest.curveCount(), 1);
    QCOMPARE(rest.pointCount(), 8);

    // lone points select no line; two stretches give two pieces
    VPointEdit::splitSelected(line, mask(10, {2, 5}), &picked, &rest);
    QVERIFY(picked.isEmpty());
    QVERIFY(rest == line);
    VPointEdit::splitSelected(line, mask(10, {1, 2, 5, 6, 7}), &picked, &rest);
    QCOMPARE(picked.curveCount(), 2);
    QCOMPARE(picked.pointCount(), 2 + 3);
    QCOMPARE(rest.curveCount(), 3);

    // everything, or a whole sub-path: as it is, a dot too
    VPointEdit::splitSelected(line, mask(10, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), &picked, &rest);
    QVERIFY(picked == line);
    QVERIFY(rest.isEmpty());
    VCurveData twoParts = row(5);
    twoParts.append(row(1, false, 50));
    VPointEdit::splitSelected(twoParts, mask(6, {5}), &picked, &rest);
    QCOMPARE(picked.pointCount(), 1);
    QCOMPARE(picked.positions.first(), QPointF(0, 50));
    QVERIFY(rest == row(5));

    // attributes and existing caps ride along
    VCurveData flat = row(6);
    flat.setCapFlags(0, VCurveData::StartFlat);
    flat.setAttribute("opacity", 4, 0.5, 1.0);
    VPointEdit::splitSelected(flat, mask(6, {0, 1, 2, 3, 4}), &picked, &rest);
    QCOMPARE(int(picked.capFlags(0)), int(VCurveData::StartFlat | VCurveData::EndFlat));
    QCOMPARE(picked.attribute("opacity", 4, 1.0), 0.5);
    QCOMPARE(rest.attribute("opacity", 0, 1.0), 0.5);
    QCOMPARE(int(rest.capFlags(0)), int(VCurveData::StartFlat));
}

void VPointEditTest::testSplitSelectedClosedAndBezier()
{
    VCurveData picked, rest;

    // a closed stretch across the seam stays one piece; the rest opens
    const VCurveData ring = row(8, true);
    VPointEdit::splitSelected(ring, mask(8, {6, 7, 0, 1}), &picked, &rest);
    QCOMPARE(picked.curveCount(), 1);
    QCOMPARE(picked.pointCount(), 4);
    QVERIFY(!picked.isCyclic(0));
    QCOMPARE(picked.positions.first(), ring.positions[6]);
    QCOMPARE(picked.positions.last(), ring.positions[1]);
    QCOMPARE(rest.curveCount(), 1);
    QCOMPARE(rest.pointCount(), 6);
    QVERIFY(!rest.isCyclic(0));
    QCOMPARE(rest.positions.first(), ring.positions[1]);
    QCOMPARE(rest.positions.last(), ring.positions[6]);
    QCOMPARE(int(rest.capFlags(0)), int(VCurveData::StartFlat | VCurveData::EndFlat));

    // a whole ring stays closed
    VPointEdit::splitSelected(ring, QVector<bool>(8, true), &picked, &rest);
    QVERIFY(picked == ring);

    // bezier segments inside a stretch keep their handles, the cut ends lose the outer ones
    VCurveData curve = row(4);
    curve.setCurveSegment(0, QPointF(3, 8), QPointF(7, 8));
    curve.setCurveSegment(1, QPointF(13, 8), QPointF(17, 8));
    curve.setCurveSegment(2, QPointF(23, 8), QPointF(27, 8));
    VPointEdit::splitSelected(curve, mask(4, {1, 2}), &picked, &rest);
    QCOMPARE(picked.pointCount(), 2);
    QCOMPARE(int(picked.segment(0)), int(VCurveData::CurveSegment));
    QCOMPARE(picked.handlesOut[0], QPointF(13, 8));
    QCOMPARE(picked.handlesIn[1], QPointF(17, 8));
    QCOMPARE(picked.handlesIn[0], picked.positions[0]);
    QCOMPARE(picked.handlesOut[1], picked.positions[1]);
    QCOMPARE(rest.curveCount(), 2);
    QCOMPARE(rest.handlesOut[0], QPointF(3, 8));
    QCOMPARE(rest.handlesIn[1], QPointF(7, 8));
    QCOMPARE(rest.handlesOut[rest.curveBegin(1)], QPointF(23, 8));
}

KISTEST_MAIN(VPointEditTest)

#include "VPointEditTest.moc"
