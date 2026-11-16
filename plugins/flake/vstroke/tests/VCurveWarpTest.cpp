/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QtMath>
#include <QTransform>

#include <kistest.h>

#include <KisSpatialContainer.h>
#include <kis_four_point_interpolator_backward.h>
#include <kis_four_point_interpolator_forward.h>
#include <kis_datamanager.h>
#include <kis_liquify_transform_worker.h>
#include <tiles3/kis_memento.h>
#include <kis_paint_device.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <QElapsedTimer>
#include <QImage>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "VCurveWarp.h"

/**
 * Smooth: jagged strokes get smoother with their ends in place, radius-only
 * smoothing leaves positions alone, the brush weights limit it to the
 * points under the brush, and corners stay.
 */
class VCurveWarpTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSmoothReducesJitter();
    void testRadiusOnlyKeepsPositions();
    void testBrushLimitsSmooth();
    void testZeroSizeBrushTouchesNothing();
    void testCornersStay();
    void testSparseBeziersSmoothWithoutRidges();
    void testOldSmoothDependsOnDensity();
    void testSmoothAlongIndependentOfDensity();
    void testSmoothAlongKeepsStraightLinesAndEnds();
    void testSmoothAlongBrushLimits();
    void testSmoothAlongKeepShape();
    void testSmoothAlongClosedAndCorners();
    void testSmoothAlongRadiusAndPressure();
    void benchmarkSmoothAlongDense();
    void testTranslateMatchesLiquifyWorker_data();
    void testTranslateMatchesLiquifyWorker();
    void testTranslateLeavesFarPointsAlone();
    void testTranslateFromContinues_data();
    void testTranslateFromContinues();
    void testWorkerRunInRectMatchesRun();
    void testWorkerScaleToLodKeepsQueries_data();
    void testWorkerScaleToLodKeepsQueries();
    void testContainerMovePointsMatchesMovePoint();
    void testWorkerRunInRectBands();
    void testWorkerRunInRectStaysInItsTileRows();
    void testWorkerRunInRectBandsInThreads();
    void testBackwardInterpolatorSecondRootUsesY();
};

namespace {

VCurveData jagged(int n)
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < n; i++) {
        const int p = d.addPoint(QPointF(i * 4.0, (i % 2) ? 3.0 : -3.0), 2.0 + (i % 3));
        d.setAttribute("pressure", p, 0.3 + 0.4 * (i % 2), 1.0);
    }
    return d;
}

/// the mean squared second difference of the positions
qreal roughness(const VCurveData &d)
{
    qreal sum = 0;
    for (int i = 1; i + 1 < d.pointCount(); i++) {
        const QPointF dd = d.positions[i - 1] - 2 * d.positions[i] + d.positions[i + 1];
        sum += dd.x() * dd.x() + dd.y() * dd.y();
    }
    return sum / (d.pointCount() - 2);
}

/// a straight line along x, 200 long, with a sine wobble of \p amplitude and \p wavelength, points \p spacing apart
VCurveData wobble(qreal spacing, qreal wavelength = 20.0, qreal amplitude = 3.0, qreal length = 200.0)
{
    VCurveData d;
    d.beginCurve();
    const int n = qRound(length / spacing) + 1;
    for (int i = 0; i < n; i++) {
        const qreal x = i * spacing;
        const int p = d.addPoint(QPointF(x, amplitude * std::sin(2 * M_PI * x / wavelength)), 2.0);
        d.setAttribute("pressure", p, 0.5, 1.0);
    }
    return d;
}

/// the root mean square of y over the middle of a wobble (away from its ends)
qreal wobbleAmplitude(const VCurveData &d, qreal margin = 40.0)
{
    const qreal length = d.positions.last().x();
    qreal sum = 0;
    int count = 0;
    for (const QPointF &p : d.positions) {
        if (p.x() < margin || p.x() > length - margin) continue;
        sum += p.y() * p.y();
        count++;
    }
    return count ? std::sqrt(2.0 * sum / count) : 0.0;
}

/// a wavy line of few points with smooth (Catmull-Rom) bezier handles, as a converted shape has
VCurveData sparseWave(int n)
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < n; i++) d.addPoint(QPointF(i * 40.0, (i % 2) ? 12.0 : -12.0), 2.0);
    for (int i = 0; i + 1 < n; i++) {
        const QPointF t0 = (d.positions[qMin(i + 1, n - 1)] - d.positions[qMax(i - 1, 0)]) / 6;
        const QPointF t1 = (d.positions[qMin(i + 2, n - 1)] - d.positions[i]) / 6;
        d.setCurveSegment(i, d.positions[i] + t0, d.positions[i + 1] - t1);
    }
    return d;
}

/// the total absolute turning of the line, sampled, in radians
qreal turning(const VCurveData &d)
{
    QVector<QPointF> line;
    const int n = d.pointCount();
    const bool cyclic = d.isCyclic(0);
    for (int i = 0; i < (cyclic ? n : n - 1); i++) {
        const VCurvePoint a = d.point(i, QStringList());
        const VCurvePoint b = d.point((i + 1) % n, QStringList());
        for (int s = 0; s < 32; s++) line << VCurvePoint::segmentPoint(a, b, s / 32.0);
    }
    line << d.positions[cyclic ? 0 : n - 1];
    if (cyclic) line << line[1];
    qreal sum = 0;
    for (int i = 1; i + 1 < line.size(); i++) {
        const QPointF u = line[i] - line[i - 1];
        const QPointF v = line[i + 1] - line[i];
        sum += std::abs(std::atan2(u.x() * v.y() - u.y() * v.x(), u.x() * v.x() + u.y() * v.y()));
    }
    return sum;
}

}

void VCurveWarpTest::testSmoothReducesJitter()
{
    const VCurveData original = jagged(40);
    VCurveData d = original;
    VCurveWarp::smooth(d, QVector<qreal>(), 0.5, 4);
    QVERIFY(roughness(d) < 0.1 * roughness(original));
    QCOMPARE(d.positions.first(), original.positions.first());
    QCOMPARE(d.positions.last(), original.positions.last());
    QCOMPARE(d.pointCount(), original.pointCount());
    // only positions by default
    QCOMPARE(d.radii, original.radii);
    QCOMPARE(d.attributes, original.attributes);
}

void VCurveWarpTest::testRadiusOnlyKeepsPositions()
{
    const VCurveData original = jagged(30);
    VCurveData d = original;
    VCurveWarp::smooth(d, QVector<qreal>(), 1.0, 3, VCurveWarp::Affect::Radius | VCurveWarp::Affect::Pressure);
    QCOMPARE(d.positions, original.positions);
    QVERIFY(d.radii != original.radii);
    QVERIFY(d.attributes["pressure"] != original.attributes["pressure"]);
    // jitter in width evens out
    qreal spreadBefore = 0, spreadAfter = 0;
    for (int i = 1; i + 1 < d.pointCount(); i++) {
        spreadBefore += std::abs(original.radii[i] - original.radii[i - 1]);
        spreadAfter += std::abs(d.radii[i] - d.radii[i - 1]);
    }
    QVERIFY(spreadAfter < 0.5 * spreadBefore);
}

void VCurveWarpTest::testBrushLimitsSmooth()
{
    const VCurveData original = jagged(60);
    VCurveData d = original;
    const QPointF center(120, 0);
    const qreal radius = 20;
    const QVector<qreal> weights = VCurveWarp::brushWeights(d, QTransform(), center, radius);
    VCurveWarp::smooth(d, weights, 1.0, 1);
    int moved = 0;
    for (int i = 0; i < d.pointCount(); i++) {
        if (QLineF(original.positions[i], center).length() >= radius) {
            QCOMPARE(d.positions[i], original.positions[i]);
        } else if (d.positions[i] != original.positions[i]) {
            moved++;
        }
    }
    QVERIFY(moved > 3);
}

void VCurveWarpTest::testZeroSizeBrushTouchesNothing()
{
    // a brush of size 0, even right on a point: no weights, nothing smoothed
    const VCurveData original = wobble(0.5);
    VCurveData d = original;
    const QVector<qreal> weights = VCurveWarp::brushWeights(d, QTransform(), original.positions[10], 0.0);
    QCOMPARE(weights.size(), d.pointCount());
    for (qreal w : weights) QCOMPARE(w, 0.0);
    VCurveWarp::smooth(d, weights, 1.0, 1);
    QVERIFY(d == original);

    // a small one takes the point under it only
    const QVector<qreal> small = VCurveWarp::brushWeights(d, QTransform(), original.positions[10], 0.05);
    QVERIFY(small[10] > 0);
    QCOMPARE(std::count_if(small.begin(), small.end(), [] (qreal w) { return w > 0; }), 1);
}

void VCurveWarpTest::testCornersStay()
{
    // two bezier segments meeting at a sharp corner, plus a smooth joint
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < 4; i++) d.addPoint(QPointF(i * 20, i == 1 ? 20 : 0), 1);
    d.setCurveSegment(0, QPointF(5, 0), QPointF(10, 25));      // arrives at 1 going up
    d.setCurveSegment(1, QPointF(30, 25), QPointF(35, 0));     // leaves 1 going down: a corner
    d.setCurveSegment(2, QPointF(45, 0), QPointF(55, 0));
    QVERIFY(VCurveWarp::isCorner(d, 1));
    QVERIFY(!VCurveWarp::isCorner(d, 2));
    const VCurveData original = d;
    VCurveWarp::smooth(d, QVector<qreal>(), 1.0, 2);
    QCOMPARE(d.positions[1], original.positions[1]);
    QVERIFY(d.positions[2] != original.positions[2]);
    // the corner's handles keep their directions
    auto angle = [] (const QPointF &v) { return std::atan2(v.y(), v.x()); };
    QVERIFY(std::abs(angle(d.handlesOut[1] - d.positions[1]) - angle(original.handlesOut[1] - original.positions[1])) < 1e-9);
    QVERIFY(std::abs(angle(d.positions[1] - d.handlesIn[1]) - angle(original.positions[1] - original.handlesIn[1])) < 1e-9);
}

void VCurveWarpTest::testOldSmoothDependsOnDensity()
{
    // the two-neighbour smooth (kept for Simplify's merge): one call barely
    // touches a wobble drawn with dense points, and flattens the same
    // wobble drawn with sparse ones. Why the tools use smoothAlong()
    VCurveData dense = wobble(0.5);
    VCurveData sparse = wobble(5.0);
    VCurveWarp::smooth(dense, QVector<qreal>(), 0.5, 2);
    VCurveWarp::smooth(sparse, QVector<qreal>(), 0.5, 2);
    const qreal keptDense = wobbleAmplitude(dense) / 3.0;
    const qreal keptSparse = wobbleAmplitude(sparse) / 3.0;
    qDebug() << "old smooth keeps" << keptDense << "of the wobble at 0.5 spacing," << keptSparse << "at 5";
    QVERIFY2(keptDense > 0.95, qPrintable(QString::number(keptDense)));
    QVERIFY2(keptSparse < 0.5, qPrintable(QString::number(keptSparse)));
}

void VCurveWarpTest::testSmoothAlongIndependentOfDensity()
{
    // the same wobble, drawn with points 0.5 and 2 apart: one call takes about as much off both
    QVector<qreal> kept;
    for (qreal spacing : {0.5, 2.0}) {
        VCurveData d = wobble(spacing);
        VCurveWarp::smoothAlong(d, QVector<qreal>(), 0.5, 10.0, 1);
        kept << wobbleAmplitude(d) / 3.0;
    }
    qDebug() << "smoothAlong keeps" << kept[0] << "of the wobble at 0.5 spacing," << kept[1] << "at 2";
    // a Gaussian of sigma 5 on a 20-long wobble (a bit longer along the line), half the way
    for (qreal k : kept) QVERIFY2(k > 0.5 && k < 0.8, qPrintable(QString::number(k)));
    QVERIFY2(qAbs(kept[0] - kept[1]) < 0.05, qPrintable(QString("%1 vs %2").arg(kept[0]).arg(kept[1])));
}

void VCurveWarpTest::testSmoothAlongKeepsStraightLinesAndEnds()
{
    // a straight line, points unevenly apart, tilted: it stays on its line, and its ends stay
    VCurveData d;
    d.beginCurve();
    const QPointF direction(0.8, 0.6);
    qreal t = 0;
    for (int i = 0; i < 60; i++) {
        d.addPoint(t * direction, 2.0);
        t += (i % 3 == 0) ? 4.0 : 0.5;
    }
    const VCurveData original = d;
    VCurveWarp::smoothAlong(d, QVector<qreal>(), 1.0, 20.0, 3, VCurveWarp::Affect::Position, false);
    for (int i = 0; i < d.pointCount(); i++) {
        const QPointF p = d.positions[i];
        const qreal off = p.x() * direction.y() - p.y() * direction.x();
        QVERIFY2(qAbs(off) < 1e-9, qPrintable(QString("point %1 off the line by %2").arg(i).arg(off)));
    }
    QCOMPARE(d.positions.first(), original.positions.first());
    QCOMPARE(d.positions.last(), original.positions.last());

    // a wobble's ends stay too, and the points next to them smooth (the mirror, not a pull inward)
    VCurveData w = wobble(0.5);
    const VCurveData before = w;
    VCurveWarp::smoothAlong(w, QVector<qreal>(), 1.0, 10.0, 2);
    QCOMPARE(w.positions.first(), before.positions.first());
    QCOMPARE(w.positions.last(), before.positions.last());
    QVERIFY(w.positions[10] != before.positions[10]);
}

void VCurveWarpTest::testSmoothAlongBrushLimits()
{
    const VCurveData original = wobble(0.5);
    VCurveData d = original;
    const QVector<qreal> weights = VCurveWarp::brushWeights(d, QTransform(), QPointF(100, 0), 15);
    VCurveWarp::smoothAlong(d, weights, 1.0, 10.0, 2, VCurveWarp::Affect::Position | VCurveWarp::Affect::Radius |
                                                       VCurveWarp::Affect::Pressure);
    int moved = 0;
    for (int i = 0; i < d.pointCount(); i++) {
        if (weights[i] > 0) {
            moved += d.positions[i] != original.positions[i];
            continue;
        }
        QCOMPARE(d.positions[i], original.positions[i]);
        QCOMPARE(d.radii[i], original.radii[i]);
    }
    QVERIFY(moved > 10);
}

void VCurveWarpTest::testSmoothAlongKeepShape()
{
    // a dense closed circle of radius 50: plain smoothing shrinks it, keep shape far less;
    // a short wobble on a line goes either way
    auto circle = [] () {
        VCurveData c;
        c.beginCurve(true);
        for (int i = 0; i < 600; i++) {
            const qreal a = 2 * M_PI * i / 600;
            c.addPoint(50 * QPointF(std::cos(a), std::sin(a)), 2.0);
        }
        return c;
    };
    auto meanRadius = [] (const VCurveData &c) {
        qreal sum = 0;
        for (const QPointF &p : c.positions) sum += std::hypot(p.x(), p.y());
        return sum / c.pointCount();
    };
    VCurveData plain = circle();
    VCurveData kept = circle();
    VCurveWarp::smoothAlong(plain, QVector<qreal>(), 1.0, 40.0, 5, VCurveWarp::Affect::Position, false);
    VCurveWarp::smoothAlong(kept, QVector<qreal>(), 1.0, 40.0, 5, VCurveWarp::Affect::Position, true);
    const qreal plainShrink = 50 - meanRadius(plain);
    const qreal keptShrink = qAbs(50 - meanRadius(kept));
    qDebug() << "circle of 50 shrinks by" << plainShrink << "plain," << keptShrink << "keeping its shape";
    QVERIFY2(plainShrink > 0.5, qPrintable(QString::number(plainShrink)));
    QVERIFY2(keptShrink < 0.25 * plainShrink, qPrintable(QString::number(keptShrink)));

    VCurveData w = wobble(0.5, 6.0);
    VCurveWarp::smoothAlong(w, QVector<qreal>(), 1.0, 10.0, 1, VCurveWarp::Affect::Position, true);
    QVERIFY2(wobbleAmplitude(w) < 0.2 * 3.0, qPrintable(QString::number(wobbleAmplitude(w))));
}

void VCurveWarpTest::testSmoothAlongClosedAndCorners()
{
    // a closed square with corners (beziers whose handles aren't in line) and a wobble along each side:
    // the corners stay put, the sides smooth
    VCurveData d;
    d.beginCurve(true);
    const QPointF corners[] = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    QVector<int> cornerIndex;
    for (int side = 0; side < 4; side++) {
        const QPointF a = corners[side], b = corners[(side + 1) % 4];
        const QPointF normal(-(b - a).y() / 100, (b - a).x() / 100);
        for (int k = 0; k < 50; k++) {
            if (k == 0) cornerIndex << d.pointCount();
            const qreal t = k / 50.0;
            const QPointF wobbleOffset = (k == 0 ? 0.0 : 2.0 * std::sin(2 * M_PI * t * 4)) * normal;
            d.addPoint(a + t * (b - a) + wobbleOffset, 2.0);
        }
    }
    // the sides are lines; the segments at each corner are straight beziers, at right angles there
    const int n = d.pointCount();
    for (int c : cornerIndex) {
        for (int i : {(c - 1 + n) % n, c}) {
            const QPointF p = d.positions[i], q = d.positions[(i + 1) % n];
            d.setCurveSegment(i, p + (q - p) / 3, q - (q - p) / 3);
        }
    }
    for (int c : cornerIndex) QVERIFY(VCurveWarp::isCorner(d, c));
    const VCurveData original = d;
    VCurveWarp::smoothAlong(d, QVector<qreal>(), 1.0, 10.0, 2);
    for (int c : cornerIndex) QCOMPARE(d.positions[c], original.positions[c]);
    qreal before = 0, after = 0;
    for (int i = 0; i < n; i++) {
        if (cornerIndex.contains(i)) continue;
        const QPointF e = original.positions[i], f = d.positions[i];
        const qreal offBefore = qMin(qMin(qAbs(e.x()), qAbs(e.x() - 100)), qMin(qAbs(e.y()), qAbs(e.y() - 100)));
        const qreal offAfter = qMin(qMin(qAbs(f.x()), qAbs(f.x() - 100)), qMin(qAbs(f.y()), qAbs(f.y() - 100)));
        before += offBefore;
        after += offAfter;
    }
    QVERIFY2(after < 0.5 * before, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
}

void VCurveWarpTest::testSmoothAlongRadiusAndPressure()
{
    VCurveData d = jagged(40);
    const VCurveData original = d;
    VCurveWarp::smoothAlong(d, QVector<qreal>(), 1.0, 12.0, 2, VCurveWarp::Affect::Radius | VCurveWarp::Affect::Pressure);
    QCOMPARE(d.positions, original.positions);
    auto spread = [] (const QVector<qreal> &v) {
        qreal sum = 0;
        for (int i = 1; i < v.size(); i++) sum += qAbs(v[i] - v[i - 1]);
        return sum;
    };
    QVERIFY(spread(d.radii) < 0.5 * spread(original.radii));
    const QVector<qreal> pressure = d.attributes.value("pressure");
    QVERIFY(spread(pressure) < 0.5 * spread(original.attributes.value("pressure")));
    for (qreal p : pressure) QVERIFY(p >= 0 && p <= 1);
}

void VCurveWarpTest::benchmarkSmoothAlongDense()
{
    // a 10000-point stroke, points 0.5 apart, smoothed whole as Edit Shapes' Apply does
    const VCurveData original = wobble(0.5, 20.0, 3.0, 5000.0);
    QElapsedTimer timer;
    timer.start();
    VCurveData d = original;
    VCurveWarp::smoothAlong(d, QVector<qreal>(), 0.5, 10.0, 1, VCurveWarp::Affect::Position | VCurveWarp::Affect::Radius);
    const qint64 ms = timer.elapsed();
    qDebug() << original.pointCount() << "points smoothed in" << ms << "ms";
    QVERIFY(ms < 2000);
}

void VCurveWarpTest::testSparseBeziersSmoothWithoutRidges()
{
    // handles left as they were overshoot once the points pull together:
    // each segment bends into a ridge and the line turns more, not less
    const VCurveData original = sparseWave(7);
    VCurveData d = original;
    for (int pass = 0; pass < 3; pass++) VCurveWarp::smooth(d, QVector<qreal>(), 0.5, 2);
    QVERIFY2(turning(d) < 0.5 * turning(original),
             qPrintable(QString("turning %1 -> %2").arg(turning(original)).arg(turning(d))));

    // a circle of four beziers shrinks as it smooths; the handles shrink
    // with it instead of looping out past the points
    VCurveData circle;
    circle.beginCurve(true);
    const qreal k = 0.5523 * 50;
    for (int i = 0; i < 4; i++) {
        const qreal a = i * M_PI / 2;
        circle.addPoint(50 * QPointF(std::cos(a), std::sin(a)), 2.0);
    }
    for (int i = 0; i < 4; i++) {
        const qreal a = i * M_PI / 2, b = (i + 1) * M_PI / 2;
        circle.setCurveSegment(i, circle.positions[i] + k * QPointF(-std::sin(a), std::cos(a)),
                               circle.positions[(i + 1) % 4] - k * QPointF(-std::sin(b), std::cos(b)));
    }
    QVERIFY(std::abs(turning(circle) - 2 * M_PI) < 0.05);
    VCurveData shrunk = circle;
    for (int pass = 0; pass < 3; pass++) VCurveWarp::smooth(shrunk, QVector<qreal>(), 0.5, 2);
    QVERIFY(QLineF(QPointF(), shrunk.positions[0]).length() < 40);
    QVERIFY2(std::abs(turning(shrunk) - 2 * M_PI) < 0.05, qPrintable(QString::number(turning(shrunk))));

    // with the brush, points outside it stay, and so do the handles of
    // points whose neighbours stay; next to a moved point only the length
    // of a handle may change
    VCurveData brushed = original;
    const QVector<qreal> weights = VCurveWarp::brushWeights(brushed, QTransform(), QPointF(120, 12), 30);
    VCurveWarp::smooth(brushed, weights, 1.0, 1);
    auto angle = [] (const QPointF &v) { return std::atan2(v.y(), v.x()); };
    for (int i = 0; i < brushed.pointCount(); i++) {
        if (weights[i] > 0) continue;
        QCOMPARE(brushed.positions[i], original.positions[i]);
        const bool nearMoved = (i > 0 && weights[i - 1] > 0) || (i + 1 < weights.size() && weights[i + 1] > 0);
        if (!nearMoved) {
            QCOMPARE(brushed.handlesIn[i], original.handlesIn[i]);
            QCOMPARE(brushed.handlesOut[i], original.handlesOut[i]);
        } else {
            QVERIFY(std::abs(angle(brushed.handlesIn[i] - brushed.positions[i]) - angle(original.handlesIn[i] - original.positions[i])) < 1e-9 || i == 0);
            QVERIFY(std::abs(angle(brushed.handlesOut[i] - brushed.positions[i]) - angle(original.handlesOut[i] - original.positions[i])) < 1e-9 || i == brushed.pointCount() - 1);
        }
    }
    QVERIFY(brushed.positions[3] != original.positions[3]);
}

void VCurveWarpTest::testTranslateMatchesLiquifyWorker_data()
{
    QTest::addColumn<bool>("wash");
    QTest::newRow("build up") << false;
    QTest::newRow("wash") << true;
}

void VCurveWarpTest::testTranslateMatchesLiquifyWorker()
{
    QFETCH(bool, wash);
    // the worker's grid points as curve points, in image pixels, moved by the same dabs
    KisLiquifyTransformWorker worker(QRect(0, 0, 200, 120), 0, 8);
    VCurveData d;
    d.beginCurve();
    for (const QPointF &p : worker.originalPoints()) d.addPoint(p, 1);
    const VCurveData snapshot = d;

    QVector<VCurveWarp::Dab> dabs;
    for (int i = 0; i < 12; i++) {
        dabs << VCurveWarp::Dab{QPointF(40 + 10 * i, 60 + 3 * std::sin(i)), QPointF(6, 2 - i * 0.3), 25.0, wash, 0.3};
    }
    for (const VCurveWarp::Dab &dab : dabs) {
        worker.translatePoints(dab.position, dab.offset, dab.sigma, dab.wash, dab.flow);
        VCurveWarp::translate(d, dab, QTransform(), snapshot);
    }
    const QVector<QPointF> &expected = worker.transformedPoints();
    QCOMPARE(d.pointCount(), expected.size());
    qreal worst = 0;
    int moved = 0;
    for (int i = 0; i < expected.size(); i++) {
        worst = qMax(worst, QLineF(d.positions[i], expected[i]).length());
        if (d.positions[i] != snapshot.positions[i]) moved++;
    }
    QVERIFY2(worst < 0.5, qPrintable(QString::number(worst)));
    QVERIFY(moved > 20);

    // the same through translated(), in another unit: points (2 px each)
    VCurveData points = snapshot;
    points.transform(QTransform::fromScale(0.5, 0.5));
    const VCurveData result = VCurveWarp::translated(points, dabs, QTransform::fromScale(2, 2));
    for (int i = 0; i < expected.size(); i++) {
        QVERIFY(QLineF(result.positions[i] * 2, expected[i]).length() < 0.5);
    }
}

void VCurveWarpTest::testTranslateLeavesFarPointsAlone()
{
    VCurveData d = jagged(50);
    d.setCurveSegment(10, d.positions[10] + QPointF(1, 1), d.positions[11] - QPointF(1, 1));
    const VCurveData snapshot = d;
    const VCurveWarp::Dab dab{QPointF(20, 0), QPointF(0, 10), 4.0, false, 1.0};
    VCurveWarp::translate(d, dab, QTransform(), snapshot);
    QCOMPARE(d.pointCount(), snapshot.pointCount());
    for (int i = 0; i < d.pointCount(); i++) {
        if (QLineF(snapshot.positions[i], dab.position).length() > 12) QCOMPARE(d.positions[i], snapshot.positions[i]);
    }
    QVERIFY(d.positions[5] != snapshot.positions[5]);
    // the handles move by their own distance
    QCOMPARE(d.handlesOut[10], snapshot.handlesOut[10]);
    QCOMPARE(d.radii, snapshot.radii);
}

void VCurveWarpTest::testTranslateFromContinues_data()
{
    QTest::addColumn<bool>("wash");
    QTest::newRow("build up") << false;
    QTest::newRow("wash") << true;
}

void VCurveWarpTest::testTranslateFromContinues()
{
    // the live Liquify preview applies the dabs as they come, a few per
    // update: the same curves as one replay of all of them, exactly
    QFETCH(bool, wash);
    VCurveData snapshot;
    snapshot.beginCurve();
    for (int i = 0; i <= 60; i++) snapshot.addPoint(QPointF(5 * i, 50 + 10 * std::sin(i * 0.3)), 1 + 0.02 * i);
    QVector<VCurveWarp::Dab> dabs;
    for (int i = 0; i < 25; i++) {
        dabs << VCurveWarp::Dab{QPointF(30 + 9 * i, 55), QPointF(2, 7 - 0.4 * i), 18.0, wash, 0.4};
    }
    const QTransform toImage = QTransform::fromScale(1.5, 1.5);
    const VCurveData whole = VCurveWarp::translated(snapshot, dabs, toImage);

    VCurveData live = snapshot;
    for (int from : {0, 3, 4, 11, 19}) {
        const int to = from == 19 ? dabs.size() : (from == 0 ? 3 : (from == 3 ? 4 : (from == 4 ? 11 : 19)));
        VCurveWarp::translateFrom(live, dabs.mid(0, to), from, toImage, snapshot);
    }
    QVERIFY(whole != snapshot);
    QVERIFY(live == whole);
}

void VCurveWarpTest::testWorkerRunInRectMatchesRun()
{
    // the incremental Liquify preview ([custom] KisLiquifyTransformWorker::
    // changedRect() and runInRect()): repainting only what the new dabs
    // changed gives exactly what a full run() gives
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 400, 300);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    for (int y = 0; y < bounds.height(); y += 10) {
        for (int x = 0; x < bounds.width(); x += 10) {
            src->fill(QRect(x, y, 10, 10), KoColor(QColor((x * 7) % 256, (y * 5) % 256, (x + y) % 256, 128 + (x % 128)), cs));
        }
    }

    std::unique_ptr<KisLiquifyTransformWorker> previous(new KisLiquifyTransformWorker(bounds, 0, 8));
    previous->translatePoints(QPointF(150, 150), QPointF(20, 5), 30, false, 1.0);
    previous->rotatePoints(QPointF(250, 120), 0.4, 25, false, 1.0);

    KisPaintDeviceSP incremental = new KisPaintDevice(cs);
    previous->run(src, incremental);

    using Step = std::function<void(KisLiquifyTransformWorker&)>;
    const QList<QPair<QString, Step>> steps = {
        {"build up", [] (KisLiquifyTransformWorker &w) { w.translatePoints(QPointF(170, 150), QPointF(25, -8), 30, false, 1.0); }},
        {"a fold", [] (KisLiquifyTransformWorker &w) { w.translatePoints(QPointF(180, 160), QPointF(80, 40), 20, false, 1.0); }},
        {"wash", [] (KisLiquifyTransformWorker &w) { w.translatePoints(QPointF(120, 100), QPointF(-15, 10), 35, true, 0.4); }},
        {"scale", [] (KisLiquifyTransformWorker &w) { w.scalePoints(QPointF(300, 200), 0.3, 30, false, 1.0); }},
        {"undo", [] (KisLiquifyTransformWorker &w) { w.undoPoints(QPointF(170, 150), 0.7, 40); }},
        {"outside the earlier subgrid", [] (KisLiquifyTransformWorker &w) { w.translatePoints(QPointF(40, 260), QPointF(10, -10), 15, false, 1.0); }},
    };
    KisLiquifyTransformWorker current(*previous);
    for (const auto &step : steps) {
        step.second(current);
        QRect rect;
        QVERIFY(current.changedRect(*previous, &rect));
        QVERIFY2(!rect.isEmpty(), qPrintable(step.first));
        current.runInRect(src, incremental, rect);

        KisPaintDeviceSP full = new KisPaintDevice(cs);
        current.run(src, full);
        const QImage a = full->convertToQImage(0, bounds);
        const QImage b = incremental->convertToQImage(0, bounds);
        int differing = 0;
        for (int y = 0; y < a.height(); y++) {
            for (int x = 0; x < a.width(); x++) differing += a.pixel(x, y) != b.pixel(x, y);
        }
        QVERIFY2(differing == 0, qPrintable(QString("%1: %2 pixels differ, rect %3,%4 %5x%6").arg(step.first).arg(differing)
                                            .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())));
        QVERIFY2(rect.width() * rect.height() < bounds.width() * bounds.height() / 2, qPrintable(step.first));
        previous.reset(new KisLiquifyTransformWorker(current));
    }

    // nothing changed: an empty rect; another grid: no rect at all
    QRect rect(1, 1, 1, 1);
    QVERIFY(current.changedRect(*previous, &rect));
    QVERIFY(rect.isEmpty());
    KisLiquifyTransformWorker other(QRect(0, 0, 200, 300), 0, 8);
    QVERIFY(!current.changedRect(other, &rect));
}

void VCurveWarpTest::testWorkerScaleToLodKeepsQueries_data()
{
    QTest::addColumn<bool>("wash");
    QTest::addColumn<qreal>("scale");
    QTest::newRow("build up, LOD 2") << false << 0.25;
    QTest::newRow("wash, LOD 2") << true << 0.25;
    QTest::newRow("build up, LOD 1") << false << 0.5;
    // not a power of two (0.3) takes the per-point movePoint() path, after
    // which upstream's container misses points in range queries: not tested
}

void VCurveWarpTest::testWorkerScaleToLodKeepsQueries()
{
    // the worker's rescale to a level of detail scales its point containers
    // in place ([custom] in kis_liquify_transform_worker.cpp): dabs after the
    // rescale find the same points as dabs before it, scaled
    QFETCH(bool, wash);
    QFETCH(qreal, scale);
    const QRect bounds(0, 0, 1000, 700);
    const QVector<QPointF> positions = {QPointF(300, 200), QPointF(340, 230), QPointF(700, 500), QPointF(512, 344)};
    const QPointF offset(25, -10);
    const qreal sigma = 60;

    KisLiquifyTransformWorker before(bounds, 0, 8);
    KisLiquifyTransformWorker after(bounds, 0, 8);
    // a first dab on both, so the containers hold moved points
    before.translatePoints(QPointF(500, 350), QPointF(-30, 20), sigma, wash, 0.7);
    after.translatePoints(QPointF(500, 350), QPointF(-30, 20), sigma, wash, 0.7);

    const QTransform t = QTransform::fromScale(scale, scale);
    after.transformSrcAndDst(t);
    for (const QPointF &pos : positions) {
        before.translatePoints(pos, offset, sigma, wash, 0.7);
        after.translatePoints(pos * scale, offset * scale, sigma * scale, wash, 0.7);
    }
    before.transformSrcAndDst(t);

    QCOMPARE(after.transformedPoints().size(), before.transformedPoints().size());
    int moved = 0;
    for (int i = 0; i < before.transformedPoints().size(); i++) {
        const QPointF a = before.transformedPoints()[i];
        const QPointF b = after.transformedPoints()[i];
        QVERIFY2(qAbs(a.x() - b.x()) < 1e-6 && qAbs(a.y() - b.y()) < 1e-6,
                 qPrintable(QString("point %1: %2,%3 against %4,%5").arg(i).arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())));
        moved += before.transformedPoints()[i] != before.originalPoints()[i];
    }
    QVERIFY(moved > 100);
}

void VCurveWarpTest::testContainerMovePointsMatchesMovePoint()
{
    // the liquify worker moves a dab's points with movePoints() ([custom] in
    // KisSpatialContainer): the same tree as movePoint() for each in turn,
    // so range queries give the same points in the same order
    const QRect bounds(0, 0, 2000, 1500);
    KisSpatialContainer sequential(QRectF(bounds), 100);
    KisSpatialContainer bulk(QRectF(bounds), 100);
    sequential.initializeWithGridPoints(QRectF(bounds), 8);
    bulk.initializeWithGridPoints(QRectF(bounds), 8);
    QVector<QPointF> points = sequential.toVector();
    QCOMPARE(bulk.toVector(), points);

    struct Dab { QPointF base; QPointF offset; qreal sigma; };
    const QVector<Dab> dabs = {
        {QPointF(1000, 750), QPointF(120, 40), 250},   // many points cross partitions
        {QPointF(1100, 780), QPointF(120, 40), 250},
        {QPointF(300, 200), QPointF(-60, -90), 80},    // out of the bounds at the corner
        {QPointF(1600, 1200), QPointF(5, 3), 300},     // small moves: few crossings
        {QPointF(1000, 750), QPointF(-200, -60), 250}, // back over the moved points
        {QPointF(700, 400), QPointF(0, 300), 400},
    };
    qint64 sequentialTime = 0;
    qint64 bulkTime = 0;
    QElapsedTimer timer;
    for (const Dab &dab : dabs) {
        const qreal maxDist = 3 * dab.sigma;
        QVector<int> indexes;
        sequential.findAllInRange(indexes, dab.base, maxDist);
        QVector<int> bulkIndexes;
        bulk.findAllInRange(bulkIndexes, dab.base, maxDist);
        QCOMPARE(bulkIndexes, indexes);

        QVector<QPointF> before;
        QVector<QPointF> after;
        for (int index : indexes) {
            const qreal dist = QLineF(points[index], dab.base).length();
            before << points[index];
            after << points[index] + dab.offset * std::exp(-0.5 * dist * dist / (dab.sigma * dab.sigma));
            points[index] = after.last();
        }
        timer.start();
        for (int i = 0; i < indexes.size(); i++) {
            sequential.movePoint(indexes[i], before[i], after[i]);
        }
        sequentialTime += timer.nsecsElapsed();
        timer.start();
        bulk.movePoints(indexes, before, after);
        bulkTime += timer.nsecsElapsed();
    }
    qDebug() << "movePoint" << sequentialTime / 1e6 << "ms, movePoints" << bulkTime / 1e6 << "ms";

    // upstream's count grows by each move that crosses a partition (removePoint() doesn't lower it)
    QCOMPARE(sequential.toVector().mid(0, points.size()), points);
    QCOMPARE(bulk.toVector().mid(0, points.size()), points);
    for (int y = -200; y <= 1700; y += 190) {
        for (int x = -200; x <= 2200; x += 240) {
            const QPointF center(x, y);
            for (qreal range : {30.0, 170.0, 600.0}) {
                QVector<int> a;
                QVector<int> b;
                sequential.findAllInRange(a, center, range);
                bulk.findAllInRange(b, center, range);
                QCOMPARE(b, a);
                QVector<int> expected;
                for (int i = 0; i < points.size(); i++) {
                    if (QLineF(points[i], center).length() <= range) expected << i;
                }
                std::sort(b.begin(), b.end());
                QCOMPARE(b, expected);
            }
        }
    }
}

void VCurveWarpTest::testWorkerRunInRectBands()
{
    // the incremental Liquify preview splits the changed rect into bands that
    // threads paint at once ([custom] in inplace_transform_stroke_strategy.cpp):
    // a band changes nothing outside it, so the bands in any order give what
    // one runInRect() gives
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 400, 300);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    for (int y = 0; y < bounds.height(); y += 10) {
        for (int x = 0; x < bounds.width(); x += 10) {
            src->fill(QRect(x, y, 10, 10), KoColor(QColor((x * 7) % 256, (y * 5) % 256, (x + y) % 256, 128 + (x % 128)), cs));
        }
    }

    KisLiquifyTransformWorker previous(bounds, 0, 8);
    previous.translatePoints(QPointF(150, 150), QPointF(20, 5), 30, false, 1.0);
    KisLiquifyTransformWorker current(previous);
    current.translatePoints(QPointF(180, 140), QPointF(60, 30), 40, false, 1.0);
    current.rotatePoints(QPointF(200, 180), 0.5, 30, false, 1.0);
    QRect rect;
    QVERIFY(current.changedRect(previous, &rect));
    QVERIFY(rect.height() > 60);

    KisPaintDeviceSP whole = new KisPaintDevice(cs);
    previous.run(src, whole);
    KisPaintDeviceSP banded = new KisPaintDevice(cs);
    previous.run(src, banded);
    current.runInRect(src, whole, rect);

    // uneven bands, not on tile boundaries, the last one first
    QVector<QRect> bands;
    for (int top = rect.top(); top <= rect.bottom(); top += 23) {
        bands << QRect(rect.left(), top, rect.width(), qMin(23, rect.bottom() + 1 - top));
    }
    const KoColor sentinel(QColor(255, 0, 255), cs);
    for (int i = bands.size() - 1; i >= 0; i--) {
        current.runInRect(src, banded, bands[i]);

        KisPaintDeviceSP alone = new KisPaintDevice(cs);
        alone->fill(bounds, sentinel);
        current.runInRect(src, alone, bands[i]);
        const QImage image = alone->convertToQImage(0, bounds);
        int outside = 0;
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) {
                outside += !bands[i].contains(x, y) && image.pixel(x, y) != qRgb(255, 0, 255);
            }
        }
        QVERIFY2(outside == 0, qPrintable(QString("band %1: %2 pixels changed outside").arg(i).arg(outside)));
    }

    const QImage a = whole->convertToQImage(0, bounds);
    const QImage b = banded->convertToQImage(0, bounds);
    int differing = 0;
    for (int y = 0; y < a.height(); y++) {
        for (int x = 0; x < a.width(); x++) differing += a.pixel(x, y) != b.pixel(x, y);
    }
    QCOMPARE(differing, 0);
}

void VCurveWarpTest::testWorkerRunInRectStaysInItsTileRows()
{
    // bands are whole tile rows so threads never share a tile: a tile a band
    // only locks for writing counts too, because two threads registering one
    // tile in the undo history lose a reference, and it's freed while in use
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 400, 400);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    src->fill(bounds, KoColor(QColor(10, 200, 30), cs));

    KisLiquifyTransformWorker previous(bounds, 0, 8);
    KisLiquifyTransformWorker current(previous);
    current.translatePoints(QPointF(200, 200), QPointF(40, 37), 120, false, 1.0);
    QRect rect;
    QVERIFY(current.changedRect(previous, &rect));

    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    previous.run(src, dst);

    const int tile = 64;
    const int firstRow = rect.top() / tile;
    const int lastRow = rect.bottom() / tile;
    QVERIFY(lastRow - firstRow >= 2);
    for (int row = firstRow; row <= lastRow; row++) {
        const QRect band = rect & QRect(rect.left(), row * tile, rect.width(), tile);
        KisMementoSP memento = dst->dataManager()->getMemento();
        current.runInRect(src, dst, band);
        dst->dataManager()->commit();

        const QRect touched = memento->extent();
        QVERIFY2(touched.top() >= row * tile && touched.bottom() < (row + 1) * tile,
                 qPrintable(QString("tile row %1 (%2..%3) touched y %4..%5").arg(row).arg(row * tile)
                            .arg((row + 1) * tile - 1).arg(touched.top()).arg(touched.bottom())));
    }
}

void VCurveWarpTest::testWorkerRunInRectBandsInThreads()
{
    // what the preview does: passes of bands painted at once, each pass in the undo history
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 1024, 1024);
    KisPaintDeviceSP src = new KisPaintDevice(cs);
    src->fill(bounds, KoColor(QColor(10, 200, 30), cs));

    KisLiquifyTransformWorker current(bounds, 0, 8);
    KisPaintDeviceSP dst = new KisPaintDevice(cs);
    current.run(src, dst);

    const int tile = 64;
    QVector<KisMementoSP> history;
    for (int pass = 0; pass < 100; pass++) {
        KisLiquifyTransformWorker previous(current);
        current.translatePoints(QPointF(300 + (pass % 60) * 7, 500), QPointF(9, 7), 400, false, 1.0);
        QRect rect;
        QVERIFY(current.changedRect(previous, &rect));

        history << dst->dataManager()->getMemento();
        std::vector<std::thread> threads;
        for (int row = rect.top() / tile; row <= rect.bottom() / tile; row++) {
            const QRect band = rect & QRect(rect.left(), row * tile, rect.width(), tile);
            threads.emplace_back([&current, src, dst, band] () { current.runInRect(src, dst, band); });
        }
        for (std::thread &t : threads) t.join();
        dst->dataManager()->commit();
    }
    QVERIFY(!dst->exactBounds().isEmpty());
}

void VCurveWarpTest::testBackwardInterpolatorSecondRootUsesY()
{
    // AB is vertical, so x can't give mu, and the second root of the
    // quadratic is the good one: mu has to come from y
    const QPolygonF src({QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)});
    const QPolygonF dst({QPointF(0, 0), QPointF(0, 100), QPointF(100, 150), QPointF(100, 0)});

    KisFourPointInterpolatorForward forward(src, dst);
    KisFourPointInterpolatorBackward backward(src, dst);

    QCOMPARE(forward.map(QPointF(60, 50)), QPointF(50, 75));
    QCOMPARE(backward.map(QPointF(50, 75)), QPointF(60, 50));

    for (int mu = 10; mu <= 90; mu += 20) {
        for (int nu = 10; nu <= 90; nu += 20) {
            const QPointF expected(mu, nu);
            const QPointF result = backward.map(forward.map(expected));
            QVERIFY2(QLineF(result, expected).length() < 1e-6,
                     qPrintable(QString("%1,%2 came back as %3,%4").arg(mu).arg(nu).arg(result.x()).arg(result.y())));
        }
    }
}

KISTEST_MAIN(VCurveWarpTest)

#include "VCurveWarpTest.moc"
