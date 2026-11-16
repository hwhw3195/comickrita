/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QDomDocument>
#include <QRandomGenerator>
#include <QtMath>

#include <kistest.h>

#include "VCurveSimplify.h"

/**
 * Subdivide and Simplify, with Simplify's two joins: exact bezier
 * subdivision, thinning within tolerance that keeps pressure peaks, the
 * brush versions leaving everything outside bit-exact, joining ends, and
 * merging overlapping strokes. Also the per-end caps and the builder.
 */
class VCurveSubdivideSimplifyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSubdivideBezierIsExact();
    void testSubdivideMask();
    void testSubdivideCyclic();
    void testSubdivideSteps();
    void testSubdivideStepsMask();
    void testSubdivideSmoothRoundsCorners();
    void testSubdivideSmoothKeepsStraightLines();
    void testThinWithinTolerance();
    void testThinKeepsPressurePeak();
    void testThinMaskKeepsOutside();
    void testProgressiveDownToALine();
    void testProgressiveLeastImportantFirst();
    void testProgressiveBrushKeepsOutside();
    void testProgressiveClosedKeepsThree();
    void testReversed();
    void testJoinEnds();
    void testJoinEndsRejects();
    void testMergeOverlapping();
    void testMergeKeepsSeparateLines();
    void testMergeAdjustWidth();
    void testCapsRoundTrip();
};

namespace {

QPointF bezier(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p1, qreal t)
{
    const qreal u = 1 - t;
    return u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p1;
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal l2 = QPointF::dotProduct(ab, ab);
    const qreal t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / l2, 1.0) : 0.0;
    return QLineF(p, a + t * ab).length();
}

/// samples of every segment of \p d
QVector<QPointF> trace(const VCurveData &d, int perSegment = 20)
{
    QVector<QPointF> out;
    const QStringList none;
    for (int i = 0; i < d.pointCount(); i++) {
        const int next = d.nextIndex(i);
        if (next < 0) continue;
        const VCurvePoint a = d.point(i, none), b = d.point(next, none);
        for (int s = 0; s < perSegment; s++) out << VCurvePoint::segmentPoint(a, b, qreal(s) / perSegment);
    }
    out << d.positions.last();
    return out;
}

qreal distanceToTrace(const QPointF &p, const QVector<QPointF> &line)
{
    qreal best = 1e9;
    for (int i = 0; i + 1 < line.size(); i++) best = qMin(best, distanceToSegment(p, line[i], line[i + 1]));
    return best;
}

VCurveData noisyLine(int n, qreal step, qreal noise, quint32 seed, qreal y = 0, qreal radius = 2)
{
    QRandomGenerator random(seed);
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < n; i++) {
        d.addPoint(QPointF(i * step, y + (random.generateDouble() - 0.5) * 2 * noise), radius);
    }
    return d;
}

VDocStroke stroke(const VCurveData &d, int owner)
{
    return VDocStroke{d, owner, false, 0};
}

VCurveData line(const QPointF &a, const QPointF &b, int points, qreal radius = 1)
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < points; i++) d.addPoint(a + (b - a) * (qreal(i) / (points - 1)), radius);
    return d;
}

}

void VCurveSubdivideSimplifyTest::testSubdivideBezierIsExact()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(100, 0), 3);
    d.setCurveSegment(0, QPointF(30, 60), QPointF(70, -60));
    d.setAttribute("pressure", 0, 0.2, 1.0);
    d.setAttribute("pressure", 1, 0.8, 1.0);

    const VCurveData s = d.subdivided(10, QTransform());
    QVERIFY(s.pointCount() > 10);
    QCOMPARE(s.positions.first(), d.positions.first());
    QCOMPARE(s.positions.last(), d.positions.last());
    QVector<QPointF> original;
    for (int k = 0; k <= 4000; k++) {
        original << bezier(QPointF(0, 0), QPointF(30, 60), QPointF(70, -60), QPointF(100, 0), k / 4000.0);
    }
    for (const QPointF &p : trace(s)) {
        const qreal best = distanceToTrace(p, original);
        QVERIFY2(best < 0.01, qPrintable(QString::number(best)));
    }
    // values interpolated along
    for (int i = 1; i < s.pointCount(); i++) {
        QVERIFY(s.radii[i] > s.radii[i - 1]);
        QVERIFY(s.attributes["pressure"][i] > s.attributes["pressure"][i - 1]);
    }
}

void VCurveSubdivideSimplifyTest::testSubdivideMask()
{
    const VCurveData d = line(QPointF(0, 0), QPointF(100, 0), 5);
    QVector<bool> mask(5, false);
    mask[1] = true;  // the segment from 25 to 50
    const VCurveData s = d.subdivided(5, QTransform(), mask);
    QCOMPARE(s.pointCount(), 5 + 4);
    QCOMPARE(s.positions[0], d.positions[0]);
    QCOMPARE(s.positions[1], d.positions[1]);
    QCOMPARE(s.positions[6], d.positions[2]);
    QCOMPARE(s.positions[8], d.positions[4]);
}

void VCurveSubdivideSimplifyTest::testSubdivideCyclic()
{
    VCurveData d;
    d.beginCurve(true);
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(40, 0), 1);
    d.addPoint(QPointF(40, 40), 1);
    d.setCurveSegment(2, QPointF(20, 60), QPointF(-20, 20));  // the closing segment
    const VCurveData s = d.subdivided(10, QTransform());
    QVERIFY(s.isCyclic(0));
    QVERIFY(s.pointCount() > 10);
    // the closing bezier still ends on the first point with its tangent
    QCOMPARE(s.positions.first(), QPointF(0, 0));
    const QPointF handle = s.handlesIn.first() - s.positions.first();
    QVERIFY(QLineF(QPointF(), handle).length() > 0.1);
    QVERIFY(std::abs(handle.x() * 20 - handle.y() * (-20)) < 1e-9);
}

void VCurveSubdivideSimplifyTest::testSubdivideSteps()
{
    const VCurveData d = line(QPointF(0, 0), QPointF(90, 0), 4);  // 3 segments
    QCOMPARE(d.subdividedSteps(1).pointCount(), 3 * 2 + 1);
    QCOMPARE(d.subdividedSteps(3).pointCount(), 3 * 8 + 1);
    // capped at the most steps
    QCOMPARE(d.subdividedSteps(9).pointCount(), 3 * 32 + 1);
    QCOMPARE(VCurveData::MaxSubdivideSteps, 5);
    // halving: evenly spaced, the old points kept
    const VCurveData s = d.subdividedSteps(2);
    for (int i = 0; i < s.pointCount(); i++) QVERIFY(QLineF(s.positions[i], QPointF(7.5 * i, 0)).length() < 1e-9);
}

void VCurveSubdivideSimplifyTest::testSubdivideStepsMask()
{
    const VCurveData d = line(QPointF(0, 0), QPointF(90, 0), 4);
    QVector<bool> mask(4, false);
    mask[1] = true;  // the middle segment, and every piece it becomes
    const VCurveData s = d.subdividedSteps(3, mask);
    QCOMPARE(s.pointCount(), 4 + 7);
    QCOMPARE(s.positions[1], QPointF(30, 0));
    QCOMPARE(s.positions[9], QPointF(60, 0));
}

void VCurveSubdivideSimplifyTest::testSubdivideSmoothRoundsCorners()
{
    // a closed square: smoothing bulges the new points out toward a circle
    VCurveData d;
    d.beginCurve(true);
    for (const QPointF &p : {QPointF(-50, -50), QPointF(50, -50), QPointF(50, 50), QPointF(-50, 50)}) d.addPoint(p, 1);
    for (bool steps : {false, true}) {
        const VCurveData s = steps ? d.subdividedSteps(1, QVector<bool>(), true)
                                   : d.subdivided(60, QTransform(), QVector<bool>(), true);
        QCOMPARE(s.pointCount(), 8);
        for (int i = 0; i < 8; i++) {
            const qreal r = QLineF(QPointF(), s.positions[i]).length();
            if (i % 2 == 0) {
                QCOMPARE(s.positions[i], d.positions[i / 2]);  // old points stay
            } else {
                // the four-point scheme: (-1, 9, 9, -1) / 16 puts the midpoint at 62.5 from the middle
                QVERIFY2(qAbs(r - 62.5) < 1e-9, qPrintable(QString::number(r)));
            }
        }
        QVERIFY(!s.hasCurveSegments());
        // without smoothing the midpoints stay on the edges
        const VCurveData raw = steps ? d.subdividedSteps(1) : d.subdivided(60, QTransform());
        QCOMPARE(QLineF(QPointF(), raw.positions[1]).length(), 50.0);
    }
}

void VCurveSubdivideSimplifyTest::testSubdivideSmoothKeepsStraightLines()
{
    // a straight line, its ends mirrored and repeated points skipped, stays straight
    VCurveData d = line(QPointF(0, 0), QPointF(90, 30), 4);
    d.addPoint(QPointF(90, 30), 1);  // a zero-length segment at the end
    const VCurveData s = d.subdividedSteps(3, QVector<bool>(), true);
    for (const QPointF &p : s.positions) QVERIFY(qAbs(p.y() - p.x() / 3.0) < 1e-9);
    for (int i = 1; i < s.pointCount(); i++) QVERIFY(s.positions[i].x() >= s.positions[i - 1].x() - 1e-9);
}

void VCurveSubdivideSimplifyTest::testThinWithinTolerance()
{
    // a dense, gently curving stroke
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i <= 400; i++) {
        const qreal a = i / 400.0 * M_PI;
        d.addPoint(QPointF(200 * std::cos(a), 200 * std::sin(a)), 2);
    }
    const VCurveData t = VCurveSimplify::thin(d, QTransform(), 0.5);
    QVERIFY2(t.pointCount() * 2 <= d.pointCount(), qPrintable(QString::number(t.pointCount())));
    QCOMPARE(t.positions.first(), d.positions.first());
    QCOMPARE(t.positions.last(), d.positions.last());
    const QVector<QPointF> simplified = trace(t);
    for (const QPointF &p : d.positions) QVERIFY(distanceToTrace(p, simplified) <= 0.5 + 1e-9);
}

void VCurveSubdivideSimplifyTest::testThinKeepsPressurePeak()
{
    VCurveData d = line(QPointF(0, 0), QPointF(100, 0), 101);
    for (int i = 0; i <= 100; i++) d.setAttribute("pressure", i, i == 50 ? 1.0 : 0.4, 1.0);
    const VCurveData t = VCurveSimplify::thin(d, QTransform(), 0.5, 0.1);
    QVERIFY(t.pointCount() < 10);
    QVERIFY(t.positions.contains(QPointF(50, 0)));
    const int peak = t.positions.indexOf(QPointF(50, 0));
    QCOMPARE(t.attributes["pressure"][peak], 1.0);

    // with the width/pressure check off, only the position counts
    const VCurveData off = VCurveSimplify::thin(d, QTransform(), 0.5, 0.0);
    QCOMPARE(off.pointCount(), 2);
}

void VCurveSubdivideSimplifyTest::testThinMaskKeepsOutside()
{
    const VCurveData d = noisyLine(100, 1.0, 0.05, 11);
    QVector<bool> mask(100, false);
    for (int i = 40; i < 60; i++) mask[i] = true;
    const VCurveData t = VCurveSimplify::thin(d, QTransform(), 0.5, 0.1, mask);
    QVERIFY(t.pointCount() < d.pointCount());
    // everything up to the run's first point and from its last one on is bit-exact
    for (int i = 0; i <= 40; i++) QCOMPARE(t.positions[i], d.positions[i]);
    const int removed = d.pointCount() - t.pointCount();
    for (int i = 59; i < 100; i++) QCOMPARE(t.positions[i - removed], d.positions[i]);

    // the same for subdivide
    QVector<bool> segments(100, false);
    for (int i = 40; i < 60; i++) segments[i] = true;
    const VCurveData s = d.subdivided(0.3, QTransform(), segments);
    for (int i = 0; i <= 40; i++) QCOMPARE(s.positions[i], d.positions[i]);
    const int added = s.pointCount() - d.pointCount();
    QVERIFY(added > 0);
    for (int i = 60; i < 100; i++) QCOMPARE(s.positions[i + added], d.positions[i]);
}

void VCurveSubdivideSimplifyTest::testProgressiveDownToALine()
{
    // a dense wavy polyline and a few beziers: each pass takes some
    // points, until only the ends are left and the line is straight
    VCurveData wave;
    wave.beginCurve();
    for (int i = 0; i <= 100; i++) wave.addPoint(QPointF(i * 3.0, 20 * std::sin(i / 8.0)), 2);
    wave.setCurveSegment(50, QPointF(152, 20), QPointF(154, 22));

    VCurveData d = wave;
    int passes = 0;
    while (passes < 100 && (d.pointCount() > 2 || d.hasCurveSegments())) {
        const VCurveData next = VCurveSimplify::progressive(d, QTransform(), 0.3);
        QVERIFY(next != d);
        QVERIFY(next.pointCount() <= d.pointCount());
        if (d.pointCount() > 2) QVERIFY(next.pointCount() < d.pointCount());
        d = next;
        passes++;
    }
    QVERIFY2(passes < 100, qPrintable(QString::number(d.pointCount())));
    QVERIFY(passes > 5);  // gradual, not all at once
    QCOMPARE(d.pointCount(), 2);
    QVERIFY(!d.hasCurveSegments());
    QCOMPARE(d.positions.first(), wave.positions.first());
    QCOMPARE(d.positions.last(), wave.positions.last());
    // nothing changes once it is a line
    QVERIFY(VCurveSimplify::progressive(d, QTransform(), 0.3) == d);
}

void VCurveSubdivideSimplifyTest::testProgressiveLeastImportantFirst()
{
    // a straight line with one spike: the spike goes last
    VCurveData d = line(QPointF(0, 0), QPointF(100, 0), 21);
    d.positions[10] = QPointF(50, 30);
    d = VCurveSimplify::progressive(d, QTransform(), 0.5);
    QVERIFY(d.pointCount() < 21);
    QVERIFY(d.positions.contains(QPointF(50, 30)));
    for (int pass = 0; pass < 20 && d.pointCount() > 3; pass++) d = VCurveSimplify::progressive(d, QTransform(), 0.5);
    QCOMPARE(d.pointCount(), 3);
    QCOMPARE(d.positions[1], QPointF(50, 30));

    // the width doesn't hold points back
    VCurveData w = line(QPointF(0, 0), QPointF(100, 0), 11);
    w.radii[5] = 9;
    for (int pass = 0; pass < 20 && w.pointCount() > 2; pass++) w = VCurveSimplify::progressive(w, QTransform(), 0.5);
    QCOMPARE(w.pointCount(), 2);
}

void VCurveSubdivideSimplifyTest::testProgressiveBrushKeepsOutside()
{
    const VCurveData d = noisyLine(100, 1.0, 0.05, 11);
    QVector<qreal> weights(100, 0.0);
    for (int i = 40; i < 60; i++) weights[i] = 0.5;
    const VCurveData t = VCurveSimplify::progressive(d, QTransform(), 0.2, weights);
    // the share of the summed weights: 0.2 * 10 = 2 points
    QCOMPARE(t.pointCount(), d.pointCount() - 2);
    for (int i = 0; i <= 40; i++) QCOMPARE(t.positions[i], d.positions[i]);
    for (int i = 59; i < 100; i++) QCOMPARE(t.positions[i - 2], d.positions[i]);
    // at least one point, however light the brush
    for (qreal &w : weights) w = w > 0 ? 0.01 : 0.0;
    QCOMPARE(VCurveSimplify::progressive(d, QTransform(), 0.2, weights).pointCount(), d.pointCount() - 1);
}

void VCurveSubdivideSimplifyTest::testProgressiveClosedKeepsThree()
{
    VCurveData d;
    d.beginCurve(true);
    for (int i = 0; i < 24; i++) {
        const qreal a = i * M_PI / 12;
        d.addPoint(50 * QPointF(std::cos(a), std::sin(a)), 2);
    }
    for (int pass = 0; pass < 50; pass++) d = VCurveSimplify::progressive(d, QTransform(), 0.5);
    QCOMPARE(d.pointCount(), 3);
    QVERIFY(d.isCyclic(0));
}

void VCurveSubdivideSimplifyTest::testReversed()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(10, 0), 2);
    d.addPoint(QPointF(20, 10), 3);
    d.setCurveSegment(1, QPointF(15, 0), QPointF(20, 5));
    d.setCapFlags(0, VCurveData::StartFlat);
    const VCurveData r = VCurveSimplify::reversed(d);
    QCOMPARE(r.positions.first(), QPointF(20, 10));
    QCOMPARE(r.segment(0), VCurveData::CurveSegment);
    QCOMPARE(r.handlesOut[0], QPointF(20, 5));
    QCOMPARE(r.handlesIn[1], QPointF(15, 0));
    QCOMPARE(r.segment(1), VCurveData::LineSegment);
    QCOMPARE(r.capFlags(0), quint8(VCurveData::EndFlat));
    QVERIFY(VCurveSimplify::reversed(r) == d);
}

void VCurveSubdivideSimplifyTest::testJoinEnds()
{
    // collinear, 2 apart; the second drawn backwards
    const VCurveData a = line(QPointF(0, 0), QPointF(50, 0), 6, 2);
    const VCurveData b = VCurveSimplify::reversed(line(QPointF(52, 0), QPointF(100, 0), 6, 4));
    VCurveSimplify::JoinOptions options;
    options.gap = 5;

    for (qreal transition : {0.0, 0.1}) {
        options.transition = transition;
        const QVector<VDocStroke> joined = VCurveSimplify::joinEnds({stroke(a, 0), stroke(b, 1)}, options, {});
        QCOMPARE(joined.size(), 1);
        QVERIFY(joined[0].changed);
        QCOMPARE(joined[0].owner, 0);
        const VCurveData &c = joined[0].curve;
        QCOMPARE(c.curveCount(), 1);
        QCOMPARE(c.pointCount(), transition > 0 ? 11 : 12);
        QCOMPARE(c.positions.first(), QPointF(0, 0));
        QCOMPARE(c.positions.last(), QPointF(100, 0));
        for (const QPointF &p : c.positions) QVERIFY(std::abs(p.y()) < 1e-9);
        if (transition > 0) {
            QCOMPARE(c.positions[5], QPointF(51, 0));
            QCOMPARE(c.radii[5], 3.0);
        }
    }

    // a chain of three, in any order
    const VCurveData m = line(QPointF(102, 0), QPointF(150, 0), 4, 2);
    const QVector<VDocStroke> chain = VCurveSimplify::joinEnds({stroke(m, 2), stroke(a, 0), stroke(b, 1)}, options, {});
    QCOMPARE(chain.size(), 1);
    QCOMPARE(chain[0].curve.pointCount(), 6 + 6 + 4 - 2);
}

void VCurveSubdivideSimplifyTest::testJoinEndsRejects()
{
    const VCurveData a = line(QPointF(0, 0), QPointF(50, 0), 6);
    VCurveSimplify::JoinOptions options;
    options.gap = 5;

    // at 90 degrees
    const VCurveData up = line(QPointF(52, 0), QPointF(52, 50), 6);
    QCOMPARE(VCurveSimplify::joinEnds({stroke(a, 0), stroke(up, 1)}, options, {}).size(), 2);
    // too far
    const VCurveData far = line(QPointF(60, 0), QPointF(100, 0), 6);
    QCOMPARE(VCurveSimplify::joinEnds({stroke(a, 0), stroke(far, 1)}, options, {}).size(), 2);
    // different brushes
    const VCurveData near = line(QPointF(52, 0), QPointF(100, 0), 6);
    auto differ = [] (int x, int y) { return x == y; };
    const QVector<VDocStroke> kept = VCurveSimplify::joinEnds({stroke(a, 0), stroke(near, 1)}, options, differ);
    QCOMPARE(kept.size(), 2);
    QVERIFY(!kept[0].changed && !kept[1].changed);
    // only ends inside an area
    auto outside = [] (const QPointF &p) { return p.x() > 200; };
    QCOMPARE(VCurveSimplify::joinEnds({stroke(a, 0), stroke(near, 1)}, options, {}, outside).size(), 2);
}

void VCurveSubdivideSimplifyTest::testMergeOverlapping()
{
    // three noisy copies of the line y = 20, with radii 2, 3 and 4
    QVector<VDocStroke> strokes;
    strokes << stroke(noisyLine(60, 2.0, 1.0, 1, 20, 2), 0);
    strokes << stroke(VCurveSimplify::reversed(noisyLine(55, 2.2, 1.0, 2, 20, 3)), 1);
    strokes << stroke(noisyLine(40, 2.5, 1.0, 3, 20, 4), 2);
    VCurveSimplify::MergeOptions options;
    options.distance = 4;
    options.fitError = 0.5;
    const QVector<VDocStroke> merged = VCurveSimplify::mergeOverlapping(strokes, options, {});
    QCOMPARE(merged.size(), 1);
    QVERIFY(merged[0].changed);
    const VCurveData &c = merged[0].curve;
    QCOMPARE(c.curveCount(), 1);
    QVERIFY(c.pointCount() < 30);
    QVERIFY(c.hasCurveSegments());
    for (const QPointF &p : trace(c)) {
        QVERIFY2(std::abs(p.y() - 20) < 1.0, qPrintable(QString("%1,%2").arg(p.x()).arg(p.y())));
    }
    // it spans the overlap
    const qreal x0 = qMin(c.positions.first().x(), c.positions.last().x());
    const qreal x1 = qMax(c.positions.first().x(), c.positions.last().x());
    QVERIFY(x0 < 5);
    QVERIFY(x1 > 110);
    // the radius is a mean of the strokes there
    for (int i = 0; i < c.pointCount(); i++) QVERIFY(c.radii[i] > 2.0 && c.radii[i] < 4.0);
}

void VCurveSubdivideSimplifyTest::testMergeAdjustWidth()
{
    QVector<VDocStroke> strokes;
    strokes << stroke(noisyLine(60, 2.0, 1.0, 1, 20, 2), 0);
    strokes << stroke(VCurveSimplify::reversed(noisyLine(55, 2.2, 1.0, 2, 20, 3)), 1);
    strokes << stroke(noisyLine(40, 2.5, 1.0, 3, 20, 4), 2);
    // and one far away that merges with nothing
    strokes << stroke(noisyLine(30, 2.0, 1.0, 4, 200, 2), 3);
    VCurveSimplify::MergeOptions options;
    options.distance = 4;
    options.fitError = 0.5;
    const VCurveData averaged = VCurveSimplify::mergeOverlapping(strokes, options, {})[0].curve;

    // all the way to the radius
    options.adjustWidth = true;
    options.radius = 6;
    options.bias = 1;
    QVector<VDocStroke> merged = VCurveSimplify::mergeOverlapping(strokes, options, {});
    QCOMPARE(merged.size(), 2);
    for (qreal r : merged[0].curve.radii) QCOMPARE(r, 6.0);
    QVERIFY(merged[1].curve == strokes[3].curve);  // a line no merge made keeps its width

    // half way
    options.bias = 0.5;
    merged = VCurveSimplify::mergeOverlapping(strokes, options, {});
    QCOMPARE(merged[0].curve.pointCount(), averaged.pointCount());
    for (int i = 0; i < averaged.pointCount(); i++) {
        QVERIFY(qAbs(merged[0].curve.radii[i] - (averaged.radii[i] + 6) / 2) < 1e-9);
    }

    // tapered from full at the start to nothing at the end, with points enough to show it
    options.bias = 1;
    options.taper = [] (qreal t) { return 1 - t; };
    const VCurveData tapered = VCurveSimplify::mergeOverlapping(strokes, options, {})[0].curve;
    QVERIFY(tapered.pointCount() >= 30);
    QVERIFY(qAbs(tapered.radii.first() - 6) < 1e-9);
    QVERIFY(tapered.radii.last() < 1e-9);
    for (int i = 1; i < tapered.pointCount(); i++) QVERIFY(tapered.radii[i] < tapered.radii[i - 1]);
    // the subdivision is exact: the line itself is the same
    for (const QPointF &p : trace(tapered)) QVERIFY(distanceToTrace(p, trace(averaged)) < 0.05);
}

void VCurveSubdivideSimplifyTest::testMergeKeepsSeparateLines()
{
    QVector<VDocStroke> strokes;
    strokes << stroke(noisyLine(50, 2.0, 0.5, 1, 0), 0);
    strokes << stroke(noisyLine(50, 2.0, 0.5, 2, 30), 1);
    // and one crossing them at a right angle
    strokes << stroke(line(QPointF(50, -20), QPointF(50, 50), 30), 2);
    VCurveSimplify::MergeOptions options;
    options.distance = 4;
    const QVector<VDocStroke> merged = VCurveSimplify::mergeOverlapping(strokes, options, {});
    QCOMPARE(merged.size(), 3);
    for (const VDocStroke &s : merged) QVERIFY(!s.changed);
}

void VCurveSubdivideSimplifyTest::testCapsRoundTrip()
{
    VCurveData d = line(QPointF(0, 0), QPointF(10, 0), 3);
    d.beginCurve();
    d.addPoint(QPointF(0, 5), 1);
    d.addPoint(QPointF(10, 5), 1);
    QCOMPARE(d.capFlags(1), quint8(0));
    d.setCapFlags(1, VCurveData::EndFlat);
    QCOMPARE(d.caps.size(), 2);

    QDomDocument doc;
    QDomElement e = doc.createElement(VCurveData::XmlElement);
    d.toXML(doc, e, QTransform());
    VCurveData loaded;
    QVERIFY(VCurveData::fromXML(e, &loaded));
    QVERIFY(loaded == d);
    QCOMPARE(loaded.capFlags(1), quint8(VCurveData::EndFlat));

    // a builder round trip changes nothing
    VCurveBuilder builder(d.attributeNames());
    for (int c = 0; c < d.curveCount(); c++) builder.addCurve(d, c);
    QVERIFY(builder.take() == d);
}

KISTEST_MAIN(VCurveSubdivideSimplifyTest)

#include "VCurveSubdivideSimplifyTest.moc"
