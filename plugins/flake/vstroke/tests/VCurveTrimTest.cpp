/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QtMath>

#include <algorithm>

#include <kistest.h>

#include "VCurveOutline.h"
#include "VCurveTrim.h"

/**
 * Trim, as Blender's Grease Pencil trim: cuts at centerline crossings,
 * deletes what the drag, lasso or click touches, rejoins what is kept,
 * and makes the cut ends flat.
 */
class VCurveTrimTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testCrossing();
    void testCutEndAccuracy();
    void testKeepCaps();
    void testLassoAndClick();
    void testEndsOnly();
    void testSelfCrossingLoop();
    void testCyclicSeam();
    void testBezierCutIsExact();
    void testFlatCapOutline();
    void testSpeed();
    void testWholeStroke();
    void testByPointDrag();
    void testByPointLassoAndClick();
    void testByPointClosedAndBezier();
    void testByPointDropsShortPieces();
    void testPenPauseIsNoCrossing();
    void testHiddenLoopIsNoCrossing();
    void testVisibleLoopCuts();
    void testShortPiecesGo();
    void testPenLiftTipGoesWithItsEnd_data();
    void testPenLiftTipGoesWithItsEnd();
    void testEndTouchingIsACrossing();
    void testOvershootIsOneCrossing();
    void testDragOverTheInkTouches();
    void testEndsOnlyAtSharpAngles();
    void testZeroSizeBrush();
};

namespace {

VCurveData line(const QPointF &a, const QPointF &b, int points, qreal ra = 2, qreal rb = 2)
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < points; i++) {
        const qreal t = qreal(i) / (points - 1);
        d.addPoint(a + (b - a) * t, ra + (rb - ra) * t);
    }
    return d;
}

VDocStroke stroke(const VCurveData &d, int owner)
{
    return VDocStroke{d, owner, false, 0};
}

VCurveTrim::Selector dragAt(const QPointF &a, const QPointF &b, qreal radius = 2)
{
    VCurveTrim::Selector s;
    s.mode = VCurveTrim::Selector::Drag;
    s.path = QPolygonF(QVector<QPointF>{a, b});
    s.radius = radius;
    return s;
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal l2 = QPointF::dotProduct(ab, ab);
    const qreal t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0.0;
    return QLineF(p, a + t * ab).length();
}

}

void VCurveTrimTest::testCrossing()
{
    // an X: a horizontal and a vertical line crossing at (50, 50)
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 2);
    const VCurveData v = line(QPointF(50, 0), QPointF(50, 100), 2);
    const QVector<QVector<VCurveTrim::Cut>> cuts = VCurveTrim::crossings({stroke(h, 0), stroke(v, 1)}, {}, 0.05);
    QCOMPARE(cuts[0].size(), 1);
    QCOMPARE(cuts[1].size(), 1);
    QCOMPARE(cuts[0][0].segment, 0);
    QVERIFY(std::abs(cuts[0][0].t - 0.5) < 1e-9);

    // a drag over the right arm of the horizontal line
    const QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, dragAt(QPointF(80, 40), QPointF(80, 60)));
    QCOMPARE(result.size(), 2);
    QVERIFY(result[0].changed);
    QVERIFY(!result[1].changed);
    QVERIFY(result[1].curve == v);
    const VCurveData &left = result[0].curve;
    QCOMPARE(left.pointCount(), 2);
    QCOMPARE(left.positions.first(), QPointF(0, 50));
    QVERIFY(QLineF(left.positions.last(), QPointF(50, 50)).length() < 1e-9);
    // the cut end is flat, the original end keeps the shape's cap
    QCOMPARE(left.capFlags(0), quint8(VCurveData::EndFlat));

    // a drag across the middle of the vertical line's top arm: both of v's arms stay apart from h
    const QVector<VDocStroke> both =
        VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, dragAt(QPointF(40, 20), QPointF(60, 20)));
    QCOMPARE(both.size(), 2);
    QVERIFY(!both[0].changed);
    QCOMPARE(both[1].curve.positions.first(), QPointF(50, 50));
    QCOMPARE(both[1].curve.capFlags(0), quint8(VCurveData::StartFlat));

    // nothing touched: nothing changes
    const QVector<VDocStroke> none =
        VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, dragAt(QPointF(200, 200), QPointF(210, 210)));
    QVERIFY(!none[0].changed && !none[1].changed);
}

void VCurveTrimTest::testCutEndAccuracy()
{
    // a slanted line with a radius ramp, cut by an oblique one
    const VCurveData a = line(QPointF(0, 0), QPointF(100, 37), 5, 1, 5);
    const VCurveData b = line(QPointF(30, -20), QPointF(61, 80), 3);
    const QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(a, 0)}, {b}, dragAt(QPointF(95, 30), QPointF(95, 40)));
    QCOMPARE(result.size(), 1);
    const VCurveData &kept = result[0].curve;
    const QPointF end = kept.positions.last();
    QVERIFY(distanceToSegment(end, b.positions.first(), b.positions.last()) < 0.01);
    QVERIFY(distanceToSegment(end, a.positions.first(), a.positions.last()) < 0.01);
    // the radius there is interpolated along a
    const qreal t = QLineF(a.positions.first(), end).length() / QLineF(a.positions.first(), a.positions.last()).length();
    QVERIFY(std::abs(kept.radii.last() - (1 + 4 * t)) < 1e-6);
}

void VCurveTrimTest::testKeepCaps()
{
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 2);
    const VCurveData v = line(QPointF(50, 0), QPointF(50, 100), 2);
    VCurveTrim::Options options;
    options.keepCaps = true;
    const QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, dragAt(QPointF(80, 40), QPointF(80, 60)), options);
    QCOMPARE(result[0].curve.capFlags(0), quint8(0));
}

void VCurveTrimTest::testLassoAndClick()
{
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 2);
    const VCurveData v = line(QPointF(50, 0), QPointF(50, 100), 2);

    VCurveTrim::Selector lasso;
    lasso.mode = VCurveTrim::Selector::Lasso;
    lasso.path = QPolygonF(QVector<QPointF>{QPointF(10, 40), QPointF(30, 40), QPointF(30, 60), QPointF(10, 60)});
    const QVector<VDocStroke> lassoed = VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, lasso);
    QCOMPARE(lassoed[0].curve.positions.first(), QPointF(50, 50));
    QCOMPARE(lassoed[0].curve.positions.last(), QPointF(100, 50));

    VCurveTrim::Selector click;
    click.mode = VCurveTrim::Selector::Click;
    click.point = QPointF(52, 90);
    click.radius = 4;
    const QVector<VDocStroke> clicked = VCurveTrim::trim({stroke(h, 0), stroke(v, 1)}, {}, click);
    QVERIFY(!clicked[0].changed);
    QVERIFY(clicked[1].changed);
    QCOMPARE(clicked[1].curve.positions.last(), QPointF(50, 50));
}

void VCurveTrimTest::testEndsOnly()
{
    // a line crossed twice; a drag over its middle piece
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 2);
    const VCurveData v1 = line(QPointF(30, 0), QPointF(30, 100), 2);
    const VCurveData v2 = line(QPointF(70, 0), QPointF(70, 100), 2);
    VCurveTrim::Options options;
    options.endsOnly = true;
    const QVector<VDocStroke> middle =
        VCurveTrim::trim({stroke(h, 0)}, {v1, v2}, dragAt(QPointF(50, 40), QPointF(50, 60)), options);
    QVERIFY(!middle[0].changed);
    const QVector<VDocStroke> end =
        VCurveTrim::trim({stroke(h, 0)}, {v1, v2}, dragAt(QPointF(90, 40), QPointF(90, 60)), options);
    QVERIFY(end[0].changed);
    QCOMPARE(end[0].curve.positions.last(), QPointF(70, 50));
}

void VCurveTrimTest::testSelfCrossingLoop()
{
    // a stroke that loops over itself: in at (0, 50), around, out at (100, 50)
    VCurveData d;
    d.beginCurve();
    for (const QPointF &p : {QPointF(0, 50), QPointF(60, 50), QPointF(60, 10), QPointF(40, 10), QPointF(40, 80),
                             QPointF(100, 80)}) {
        d.addPoint(p, 2);
    }
    const QVector<QVector<VCurveTrim::Cut>> cuts = VCurveTrim::crossings({stroke(d, 0)}, {}, 0.05);
    QCOMPARE(cuts[0].size(), 2);  // the crossing, found from both of its edges
    // drag over the loop's top: the loop goes, the two tails stay as two pieces
    const QVector<VDocStroke> result = VCurveTrim::trim({stroke(d, 0)}, {}, dragAt(QPointF(50, 5), QPointF(50, 15)));
    QCOMPARE(result.size(), 2);
    QCOMPARE(result[0].curve.positions.first(), QPointF(0, 50));
    QVERIFY(QLineF(result[0].curve.positions.last(), QPointF(40, 50)).length() < 1e-9);
    QVERIFY(QLineF(result[1].curve.positions.first(), QPointF(40, 50)).length() < 1e-9);
    QCOMPARE(result[1].curve.positions.last(), QPointF(100, 80));
}

void VCurveTrimTest::testCyclicSeam()
{
    // a closed square, crossed by a line through its left and right sides
    VCurveData square;
    square.beginCurve(true);
    for (const QPointF &p : {QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)}) square.addPoint(p, 2);
    const VCurveData cutter = line(QPointF(-20, 50), QPointF(120, 50), 2);

    // drag over the bottom half: the top half stays as one open curve across the seam at (0, 0)
    const QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(square, 0)}, {cutter}, dragAt(QPointF(50, 95), QPointF(50, 105)));
    QCOMPARE(result.size(), 1);
    const VCurveData &kept = result[0].curve;
    QVERIFY(!kept.isCyclic(0));
    QCOMPARE(kept.pointCount(), 4);
    QVERIFY(QLineF(kept.positions.first(), QPointF(0, 50)).length() < 1e-9);
    QCOMPARE(kept.positions[1], QPointF(0, 0));
    QCOMPARE(kept.positions[2], QPointF(100, 0));
    QVERIFY(QLineF(kept.positions.last(), QPointF(100, 50)).length() < 1e-9);
    QCOMPARE(kept.capFlags(0), quint8(VCurveData::StartFlat | VCurveData::EndFlat));
}

void VCurveTrimTest::testBezierCutIsExact()
{
    VCurveData curve;
    curve.beginCurve();
    curve.addPoint(QPointF(0, 0), 2);
    curve.addPoint(QPointF(100, 0), 2);
    curve.setCurveSegment(0, QPointF(20, 80), QPointF(80, 80));
    const VCurveData cutter = line(QPointF(50, -10), QPointF(50, 100), 2);
    const QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(curve, 0)}, {cutter}, dragAt(QPointF(90, -5), QPointF(90, 30)));
    QCOMPARE(result.size(), 1);
    const VCurveData &kept = result[0].curve;
    QCOMPARE(kept.pointCount(), 2);
    QCOMPARE(kept.segment(0), VCurveData::CurveSegment);
    // the cut lies on the cutter, within the flattening of the crossing search
    QVERIFY(std::abs(kept.positions.last().x() - 50) < 0.05);
    // what is left traces the original curve
    const QStringList none;
    const VCurvePoint a = kept.point(0, none), b = kept.point(1, none);
    const VCurvePoint oa = curve.point(0, none), ob = curve.point(1, none);
    for (int s = 0; s <= 20; s++) {
        const QPointF p = VCurvePoint::segmentPoint(a, b, s / 20.0);
        qreal best = 1e9;
        for (int k = 0; k < 4000; k++) {
            best = qMin(best, distanceToSegment(p, VCurvePoint::segmentPoint(oa, ob, k / 4000.0),
                                                VCurvePoint::segmentPoint(oa, ob, (k + 1) / 4000.0)));
        }
        QVERIFY2(best < 0.01, qPrintable(QString::number(best)));
    }
}

void VCurveTrimTest::testFlatCapOutline()
{
    VCurveData d = line(QPointF(0, 0), QPointF(100, 0), 2, 10, 10);
    const QPainterPath round = VCurveOutline::line(d, VCurveStrokeStyle());
    d.setCapFlags(0, VCurveData::EndFlat);
    const QPainterPath flat = VCurveOutline::line(d, VCurveStrokeStyle());
    QVERIFY(round.contains(QPointF(105, 0)));
    QVERIFY(!flat.contains(QPointF(105, 0)));
    QVERIFY(flat.contains(QPointF(-5, 0)));
}

void VCurveTrimTest::testSpeed()
{
    // 1000 random strokes of 20 points
    QRandomGenerator random(1);
    QVector<VDocStroke> strokes;
    for (int s = 0; s < 1000; s++) {
        VCurveData d;
        d.beginCurve();
        QPointF p(random.bounded(2000.0), random.bounded(2000.0));
        for (int i = 0; i < 20; i++) {
            d.addPoint(p, 2);
            p += QPointF(random.bounded(20.0) - 10, random.bounded(20.0) - 10);
        }
        strokes << stroke(d, s);
    }
    QElapsedTimer timer;
    timer.start();
    const QVector<VDocStroke> result = VCurveTrim::trim(strokes, {}, dragAt(QPointF(900, 900), QPointF(1100, 1100), 5));
    qDebug() << "trim of 1000 strokes:" << timer.elapsed() << "ms," << result.size() << "strokes out";
    QVERIFY(timer.elapsed() < 1500);
}

/// the tip left past the area, shorter than it is wide, would only show as a dot
void VCurveTrimTest::testByPointDropsShortPieces()
{
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 11);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::Area;
    QVector<VDocStroke> result =
        VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(92, 0), QPointF(92, 100), 5), options);
    QCOMPARE(result.size(), 1);
    QCOMPARE(result[0].curve.positions.first(), h.positions.first());

    // as wide as long is still a line
    result = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(88, 0), QPointF(88, 100), 5), options);
    QCOMPARE(result.size(), 2);
}

void VCurveTrimTest::testPenPauseIsNoCrossing()
{
    // a zero-length curve segment at a corner, as a pen that stood still leaves
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(30, 0), 1);
    d.addPoint(QPointF(30, 0), 1);
    d.addPoint(QPointF(30, 30), 1);
    d.addPoint(QPointF(60, 30), 1);
    d.setCurveSegment(1, QPointF(30, 0), QPointF(30, 0));

    const QVector<VDocStroke> targets{stroke(d, 0)};
    QVERIFY(VCurveTrim::crossings(targets, {}, 0.25)[0].isEmpty());

    const QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, dragAt(QPointF(10, -5), QPointF(10, 5)), VCurveTrim::Options());
    QCOMPARE(result.size(), 1);
    QVERIFY(result[0].curve.isEmpty());
}

namespace {

/// a long line whose end hooks back over itself: the loop is 8 long and crosses at x = 98
VCurveData hooked(qreal radius)
{
    VCurveData d;
    d.beginCurve();
    for (const QPointF &p : {QPointF(0, 0), QPointF(96, 0), QPointF(100, 0), QPointF(100, 2), QPointF(98, 2), QPointF(98, -3)}) {
        d.addPoint(p, radius);
    }
    return d;
}

}

void VCurveTrimTest::testHiddenLoopIsNoCrossing()
{
    const QVector<VDocStroke> targets{stroke(hooked(5), 0)};
    QVERIFY(VCurveTrim::crossings(targets, {}, 0.25)[0].isEmpty());

    const QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, dragAt(QPointF(50, -5), QPointF(50, 5)), VCurveTrim::Options());
    QCOMPARE(result.size(), 1);
    QVERIFY(result[0].curve.isEmpty());
}

void VCurveTrimTest::testVisibleLoopCuts()
{
    const QVector<VDocStroke> targets{stroke(hooked(1), 0)};
    QCOMPARE(VCurveTrim::crossings(targets, {}, 0.25)[0].size(), 2);

    // the line goes up to the crossing, and the loop and its tail stay
    const QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, dragAt(QPointF(50, -1), QPointF(50, 1), 1), VCurveTrim::Options());
    QCOMPARE(result.size(), 1);
    QVERIFY(!result[0].curve.isEmpty());
    QVERIFY(result[0].curve.controlBounds().left() > 97);
}

void VCurveTrimTest::testPenLiftTipGoesWithItsEnd_data()
{
    QTest::addColumn<int>("erase");
    QTest::newRow("to crossings") << int(VCurveTrim::Options::ToCrossings);
    QTest::newRow("whole stroke") << int(VCurveTrim::Options::WholeStroke);
    QTest::newRow("by point") << int(VCurveTrim::Options::Area);
}

/// repeated dabs where the pen lifted are sub-paths of their own, too small to touch on purpose
void VCurveTrimTest::testPenLiftTipGoesWithItsEnd()
{
    QFETCH(int, erase);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::Erase(erase);

    const VCurveData body = line(QPointF(0, 50), QPointF(100, 50), 11);
    const VCurveData tip = line(QPointF(100, 50), QPointF(100.05, 50), 2, 0.3, 0.3);
    const VCurveData cutter = line(QPointF(70, 0), QPointF(70, 100), 2);
    const QVector<VDocStroke> targets = {stroke(body, 0), stroke(tip, 0), stroke(cutter, 1)};

    auto survives = [] (const QVector<VDocStroke> &result, const VCurveData &curve) {
        return std::any_of(result.begin(), result.end(), [&] (const VDocStroke &s) { return s.curve == curve; });
    };

    // the end of the body goes, but the drag stays away from the tip;
    // by point only takes what the drag covers, and leaves a piece too short to keep
    const VCurveTrim::Selector overEnd = options.erase == VCurveTrim::Options::Area
        ? dragAt(QPointF(95, 0), QPointF(95, 100), 3) : dragAt(QPointF(85, 0), QPointF(85, 100));
    QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, overEnd, options);
    QVERIFY(!survives(result, tip));
    QVERIFY(survives(result, cutter));

    if (options.erase == VCurveTrim::Options::WholeStroke) return;

    // the part before the crossing goes: the end stays, and the tip with it
    result = VCurveTrim::trim(targets, {}, dragAt(QPointF(30, 0), QPointF(30, 100)), options);
    QVERIFY(survives(result, tip));

    // another stroke's tip stays
    const QVector<VDocStroke> otherOwner = {stroke(body, 0), stroke(tip, 2), stroke(cutter, 1)};
    result = VCurveTrim::trim(otherOwner, {}, overEnd, options);
    QVERIFY(survives(result, tip));
}

void VCurveTrimTest::testShortPiecesGo()
{
    // crossed near both ends, nearer than the inks are wide: the ends would hide in the cutters' ink, so no crossing
    const VCurveData d = line(QPointF(0, 0), QPointF(100, 0), 11);
    const QVector<VCurveData> cutters{line(QPointF(1, -10), QPointF(1, 10), 2), line(QPointF(99, -10), QPointF(99, 10), 2)};
    const QVector<VDocStroke> targets{stroke(d, 0)};
    QVERIFY(VCurveTrim::crossings(targets, cutters, 0.25)[0].isEmpty());

    QVector<VDocStroke> result = VCurveTrim::trim(targets, cutters, dragAt(QPointF(50, -5), QPointF(50, 5)), VCurveTrim::Options());
    QCOMPARE(result.size(), 1);
    QVERIFY(result[0].curve.isEmpty());

    // an end as long as it is wide stays
    const QVector<VCurveData> further{line(QPointF(10, -10), QPointF(10, 10), 2)};
    result = VCurveTrim::trim(targets, further, dragAt(QPointF(50, -5), QPointF(50, 5)), VCurveTrim::Options());
    QCOMPARE(result.size(), 1);
    QVERIFY(!result[0].curve.isEmpty());
    QVERIFY(result[0].curve.controlBounds().right() < 11);
}

/// a stroke that ends on another, its centerline short of the other's but its ink on it, as a T
void VCurveTrimTest::testEndTouchingIsACrossing()
{
    const VCurveData bar = line(QPointF(0, 50), QPointF(100, 50), 11, 4, 4);
    const VCurveData stem = line(QPointF(50, 100), QPointF(50, 55), 5, 4, 4);
    const QVector<VDocStroke> targets{stroke(bar, 0), stroke(stem, 1)};
    const QVector<QVector<VCurveTrim::Cut>> cuts = VCurveTrim::crossings(targets, {}, 0.25);
    QCOMPARE(cuts[0].size(), 1);
    QVERIFY(std::abs(cuts[0][0].segment + cuts[0][0].t - 5) < 1e-6);
    QVERIFY(cuts[1].isEmpty());

    // the bar's right arm goes, up to the stem
    QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, dragAt(QPointF(80, 40), QPointF(80, 60)));
    QCOMPARE(result.size(), 2);
    QVERIFY(result[0].changed);
    QVERIFY(QLineF(result[0].curve.positions.last(), QPointF(50, 50)).length() < 1e-6);
    QVERIFY(!result[1].changed);

    // as a cutter from another layer too
    result = VCurveTrim::trim({stroke(bar, 0)}, {stem}, dragAt(QPointF(80, 40), QPointF(80, 60)));
    QVERIFY(QLineF(result[0].curve.positions.last(), QPointF(50, 50)).length() < 1e-6);

    // an end whose ink stays off the other's doesn't cut
    const VCurveData apart = line(QPointF(50, 100), QPointF(50, 59), 5, 4, 4);
    QVERIFY(VCurveTrim::crossings({stroke(bar, 0), stroke(apart, 1)}, {}, 0.25)[0].isEmpty());
}

/// a stroke that runs a little past another cuts it once, where the centerlines cross
void VCurveTrimTest::testOvershootIsOneCrossing()
{
    const VCurveData bar = line(QPointF(0, 50), QPointF(100, 50), 11, 4, 4);
    const VCurveData stem = line(QPointF(60, 100), QPointF(50, 46), 5, 4, 4);
    const QVector<QVector<VCurveTrim::Cut>> cuts = VCurveTrim::crossings({stroke(bar, 0), stroke(stem, 1)}, {}, 0.25);
    QCOMPARE(cuts[0].size(), 1);
}

/// the drag touches a piece where it reaches the stroke's ink, not only its centerline
void VCurveTrimTest::testDragOverTheInkTouches()
{
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 11, 4, 4);
    const VCurveData v = line(QPointF(70, 0), QPointF(70, 100), 2);
    // over the round cap past the end: 3 from the centerline's end, beyond the drag's radius of 2
    for (bool endsOnly : {false, true}) {
        VCurveTrim::Options options;
        options.endsOnly = endsOnly;
        const QVector<VDocStroke> result =
            VCurveTrim::trim({stroke(h, 0)}, {v}, dragAt(QPointF(103, 40), QPointF(103, 60)), options);
        QVERIFY2(result[0].changed, endsOnly ? "ends only" : "to crossings");
        QVERIFY(QLineF(result[0].curve.positions.last(), QPointF(70, 50)).length() < 1e-6);
    }
    // a click on the ink
    VCurveTrim::Selector click;
    click.mode = VCurveTrim::Selector::Click;
    click.point = QPointF(90, 55);
    click.radius = 2;
    const QVector<VDocStroke> clicked = VCurveTrim::trim({stroke(h, 0)}, {v}, click);
    QVERIFY(clicked[0].changed);
    // whole stroke: the same
    VCurveTrim::Options whole;
    whole.erase = VCurveTrim::Options::WholeStroke;
    QVERIFY(VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(50, 55), QPointF(60, 55)), whole)[0].curve.isEmpty());
}

/// one stroke with sharp turns is still one stroke: its ends are its first and last piece
void VCurveTrimTest::testEndsOnlyAtSharpAngles()
{
    // a zigzag, thick, crossed once on each outer arm
    VCurveData z;
    z.beginCurve();
    const QVector<QPointF> corners{QPointF(0, 0), QPointF(20, 100), QPointF(40, 0), QPointF(60, 100), QPointF(80, 0)};
    for (int i = 0; i + 1 < corners.size(); i++) {
        for (int k = 0; k < 20; k++) z.addPoint(corners[i] + (corners[i + 1] - corners[i]) * (k / 20.0), 5);
    }
    z.addPoint(corners.last(), 5);
    const VCurveData cutter = line(QPointF(-10, 30), QPointF(90, 30), 2);
    const QVector<VDocStroke> targets{stroke(z, 0)};
    // the cutter crosses all four arms; the turns themselves don't cross
    QCOMPARE(VCurveTrim::crossings(targets, {cutter}, 0.25)[0].size(), 4);

    VCurveTrim::Options options;
    options.endsOnly = true;
    // the last end goes
    QVector<VDocStroke> result = VCurveTrim::trim(targets, {cutter}, dragAt(QPointF(70, 5), QPointF(90, 5)), options);
    QVERIFY(result[0].changed);
    QVERIFY(std::abs(result[0].curve.positions.last().y() - 30) < 1e-6);
    // the first end goes
    result = VCurveTrim::trim(targets, {cutter}, dragAt(QPointF(-10, 5), QPointF(10, 5)), options);
    QVERIFY(result[0].changed);
    QVERIFY(std::abs(result[0].curve.positions.first().y() - 30) < 1e-6);
    // a turn in the middle stays
    result = VCurveTrim::trim(targets, {cutter}, dragAt(QPointF(30, 5), QPointF(50, 5)), options);
    QVERIFY(!result[0].changed);
}

KISTEST_MAIN(VCurveTrimTest)

#include "VCurveTrimTest.moc"

void VCurveTrimTest::testZeroSizeBrush()
{
    // an X of lines 4 wide; the brush has size 0, so only the pointer's own path counts
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 2);
    const VCurveData v = line(QPointF(50, 0), QPointF(50, 100), 2);
    const QVector<VDocStroke> strokes{stroke(h, 0), stroke(v, 1)};

    // to crossings: over the right arm, as with a wider brush
    QVector<VDocStroke> result = VCurveTrim::trim(strokes, {}, dragAt(QPointF(80, 40), QPointF(80, 60), 0));
    QCOMPARE(result.size(), 2);
    QVERIFY(result[0].changed && !result[1].changed);
    QVERIFY(QLineF(result[0].curve.positions.last(), QPointF(50, 50)).length() < 1e-9);
    // into the ink without reaching the middle of the line: still taken
    result = VCurveTrim::trim(strokes, {}, dragAt(QPointF(80, 40), QPointF(80, 49), 0));
    QVERIFY(result[0].changed);
    // beside the ink: not
    result = VCurveTrim::trim(strokes, {}, dragAt(QPointF(80, 40), QPointF(80, 47), 0));
    QVERIFY(!result[0].changed && !result[1].changed);
    // a click with size 0 on the ink
    VCurveTrim::Selector click;
    click.mode = VCurveTrim::Selector::Click;
    click.point = QPointF(80, 51);
    click.radius = 0;
    result = VCurveTrim::trim(strokes, {}, click);
    QVERIFY(result[0].changed && !result[1].changed);

    // whole stroke: the one the pointer goes over
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::WholeStroke;
    result = VCurveTrim::trim(strokes, {}, dragAt(QPointF(80, 40), QPointF(80, 60), 0), options);
    QVERIFY(result[0].changed && result[0].curve.isEmpty());
    QVERIFY(!result[1].changed);

    // by point: a brush with no area erases nothing, also where the pointer crosses the line or runs along it
    options.erase = VCurveTrim::Options::Area;
    for (const VCurveTrim::Selector &s : {dragAt(QPointF(80, 40), QPointF(80, 60), 0), dragAt(QPointF(20, 50), QPointF(80, 50), 0),
                                         dragAt(QPointF(50, 50), QPointF(50, 50), 0)}) {
        result = VCurveTrim::trim({stroke(h, 0)}, {}, s, options);
        QCOMPARE(result.size(), 1);
        QVERIFY(!result[0].changed && result[0].curve == h);
    }
    // the smallest size above 0 (0.1 pt) does erase: a gap of that width
    result = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(80, 40), QPointF(80, 60), 0.05), options);
    QCOMPARE(result.size(), 2);
    QVERIFY(qAbs(result[0].curve.positions.last().x() - 79.95) < 1e-3);
    QVERIFY(qAbs(result[1].curve.positions.first().x() - 80.05) < 1e-3);
}

void VCurveTrimTest::testWholeStroke()
{
    // two crossing lines and one apart; a drag over the end of the first takes all of it
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 5);
    const VCurveData v = line(QPointF(50, 0), QPointF(50, 100), 5);
    const VCurveData apart = line(QPointF(0, 200), QPointF(100, 200), 5);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::WholeStroke;
    const QVector<VDocStroke> targets{stroke(h, 0), stroke(v, 1), stroke(apart, 2)};

    QVector<VDocStroke> result = VCurveTrim::trim(targets, {}, dragAt(QPointF(5, 40), QPointF(5, 60)), options);
    QCOMPARE(result.size(), 3);
    QVERIFY(result[0].changed && result[0].curve.isEmpty());
    QVERIFY(!result[1].changed && result[1].curve == v);
    QVERIFY(!result[2].changed && result[2].curve == apart);

    // a lasso around the crossing touches both crossing lines
    VCurveTrim::Selector lasso;
    lasso.mode = VCurveTrim::Selector::Lasso;
    lasso.path = QPolygonF(QVector<QPointF>{{45, 45}, {55, 45}, {55, 55}, {45, 55}});
    result = VCurveTrim::trim(targets, {}, lasso, options);
    QVERIFY(result[0].curve.isEmpty() && result[1].curve.isEmpty());
    QVERIFY(result[2].curve == apart);

    // a click takes the nearest only
    VCurveTrim::Selector click;
    click.mode = VCurveTrim::Selector::Click;
    click.point = QPointF(52, 70);
    click.radius = 5;
    result = VCurveTrim::trim(targets, {}, click, options);
    QVERIFY(result[0].curve == h);
    QVERIFY(result[1].curve.isEmpty());
    click.point = QPointF(52, 150);
    result = VCurveTrim::trim(targets, {}, click, options);
    QVERIFY(std::none_of(result.begin(), result.end(), [] (const VDocStroke &s) { return s.changed; }));
}

void VCurveTrimTest::testByPointDrag()
{
    // a drag of radius 5 across the middle of a line with points every 10
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 11, 1, 3);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::Area;
    const QVector<VDocStroke> result = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(53, 0), QPointF(53, 100), 5),
                                                        options);
    QCOMPARE(result.size(), 2);
    const VCurveData &left = result[0].curve;
    const VCurveData &right = result[1].curve;
    QVERIFY(result[0].changed && result[1].changed);
    // new points where the line leaves the area; the points inside (50) are gone
    QVERIFY2(qAbs(left.positions.last().x() - 48) < 1e-3, qPrintable(QString::number(left.positions.last().x())));
    QVERIFY2(qAbs(right.positions.first().x() - 58) < 1e-3, qPrintable(QString::number(right.positions.first().x())));
    QCOMPARE(left.pointCount(), 6);   // 0..40, 48
    QCOMPARE(right.pointCount(), 6);  // 58, 60..100
    for (const VCurveData *piece : {&left, &right}) {
        for (const QPointF &p : piece->positions) QVERIFY(qAbs(p.x() - 53) >= 5 - 1e-3);
    }
    // the radius is interpolated at the cut, and the cut ends are flat
    QVERIFY(qAbs(left.radii.last() - (1 + 2 * 0.48)) < 1e-3);
    QVERIFY(left.capFlags(0) & VCurveData::EndFlat);
    QVERIFY(!(left.capFlags(0) & VCurveData::StartFlat));
    QVERIFY(right.capFlags(0) & VCurveData::StartFlat);
    QCOMPARE(left.positions.first(), h.positions.first());
    QCOMPARE(right.positions.last(), h.positions.last());

    // over the end: one piece left; over all of it: nothing
    QVector<VDocStroke> end = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(100, 0), QPointF(100, 100), 15), options);
    QCOMPARE(end.size(), 1);
    QVERIFY(qAbs(end[0].curve.positions.last().x() - 85) < 1e-3);
    QVector<VDocStroke> all = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(0, 50), QPointF(100, 50), 2), options);
    QCOMPARE(all.size(), 1);
    QVERIFY(all[0].changed && all[0].curve.isEmpty());
    // away from it: untouched
    QVector<VDocStroke> away = VCurveTrim::trim({stroke(h, 0)}, {}, dragAt(QPointF(0, 80), QPointF(100, 80), 5), options);
    QVERIFY(!away[0].changed && away[0].curve == h);
}

void VCurveTrimTest::testByPointLassoAndClick()
{
    const VCurveData h = line(QPointF(0, 50), QPointF(100, 50), 3);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::Area;

    VCurveTrim::Selector lasso;
    lasso.mode = VCurveTrim::Selector::Lasso;
    lasso.path = QPolygonF(QVector<QPointF>{{20, 0}, {30, 0}, {30, 100}, {20, 100}});
    QVector<VDocStroke> result = VCurveTrim::trim({stroke(h, 0)}, {}, lasso, options);
    QCOMPARE(result.size(), 2);
    QVERIFY(qAbs(result[0].curve.positions.last().x() - 20) < 1e-6);
    QVERIFY(qAbs(result[1].curve.positions.first().x() - 30) < 1e-6);

    // a click erases under its radius, as a drag of one point
    VCurveTrim::Selector click;
    click.mode = VCurveTrim::Selector::Click;
    click.point = QPointF(70, 53);
    click.radius = 5;
    result = VCurveTrim::trim({stroke(h, 0)}, {}, click, options);
    QCOMPARE(result.size(), 2);
    QVERIFY(qAbs(result[0].curve.positions.last().x() - 66) < 1e-3);
    QVERIFY(qAbs(result[1].curve.positions.first().x() - 74) < 1e-3);

    // a single dab goes when inside
    VCurveData dab;
    dab.beginCurve();
    dab.addPoint(QPointF(70, 50), 2);
    result = VCurveTrim::trim({stroke(dab, 0)}, {}, click, options);
    QCOMPARE(result.size(), 1);
    QVERIFY(result[0].changed && result[0].curve.isEmpty());
}

void VCurveTrimTest::testByPointClosedAndBezier()
{
    // a closed square: one bite out of a side leaves one open stroke around the rest
    VCurveData square;
    square.beginCurve(true);
    for (const QPointF &p : {QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)}) square.addPoint(p, 2);
    VCurveTrim::Options options;
    options.erase = VCurveTrim::Options::Area;
    QVector<VDocStroke> result = VCurveTrim::trim({stroke(square, 0)}, {}, dragAt(QPointF(50, -10), QPointF(50, 10), 5),
                                                  options);
    QCOMPARE(result.size(), 1);
    const VCurveData &open = result[0].curve;
    QVERIFY(!open.isCyclic(0));
    QVERIFY(qAbs(open.positions.first().x() - 55) < 1e-3 && qAbs(open.positions.first().y()) < 1e-6);
    QVERIFY(qAbs(open.positions.last().x() - 45) < 1e-3 && qAbs(open.positions.last().y()) < 1e-6);
    QCOMPARE(open.pointCount(), 6);  // 55, the four corners, 45

    // a bite over a corner (where the seam is)
    result = VCurveTrim::trim({stroke(square, 0)}, {}, dragAt(QPointF(0, 0), QPointF(0, 0), 10), options);
    QCOMPARE(result.size(), 1);
    QVERIFY(qAbs(QLineF(result[0].curve.positions.first(), QPointF(10, 0)).length()) < 1e-3);
    QVERIFY(qAbs(QLineF(result[0].curve.positions.last(), QPointF(0, 10)).length()) < 1e-3);

    // a bezier arc keeps its shape on both sides of the cut
    VCurveData arc;
    arc.beginCurve();
    arc.addPoint(QPointF(0, 0), 2);
    arc.addPoint(QPointF(100, 0), 2);
    arc.setCurveSegment(0, QPointF(0, 60), QPointF(100, 60));
    result = VCurveTrim::trim({stroke(arc, 0)}, {}, dragAt(QPointF(50, 0), QPointF(50, 100), 4), options);
    QCOMPARE(result.size(), 2);
    const VCurvePoint a = arc.point(0, QStringList());
    const VCurvePoint b = arc.point(1, QStringList());
    for (const VDocStroke &piece : result) {
        const VCurveData &c = piece.curve;
        QCOMPARE(c.pointCount(), 2);
        for (int s = 0; s <= 10; s++) {
            const QPointF p = VCurvePoint::segmentPoint(c.point(0, QStringList()), c.point(1, QStringList()), s / 10.0);
            qreal best = 1e9;
            for (int k = 0; k <= 2000; k++) {
                best = qMin(best, QLineF(p, VCurvePoint::segmentPoint(a, b, k / 2000.0)).length());
            }
            QVERIFY(best < 0.05);
            QVERIFY(qAbs(p.x() - 50) >= 4 - 0.01);
        }
    }
}
