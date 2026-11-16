/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QRandomGenerator>
#include <QtMath>

#include <algorithm>
#include <numeric>
#include <random>

#include <kistest.h>

#include "VCurveFit.h"

/**
 * The fitting helpers: the point grid against brute force, one spanning
 * tree for scattered parts, the longest path of points along a line, and
 * Schneider's fit within its error.
 */
class VCurveFitTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testGridMatchesBruteForce();
    void testSpanningTreeIsOneTree();
    void testLongestPathOrdersLine();
    void testFitWithinError();
};

void VCurveFitTest::testGridMatchesBruteForce()
{
    QRandomGenerator random(7);
    QVector<QPointF> points;
    for (int i = 0; i < 500; i++) points << QPointF(random.bounded(200.0), random.bounded(100.0));
    const VCurveFit::PointGrid grid(points, 7.0);
    for (int q = 0; q < 50; q++) {
        const QPointF center(random.bounded(200.0), random.bounded(100.0));
        QVector<int> found = grid.near(center, 12.0);
        std::sort(found.begin(), found.end());
        QVector<int> expected;
        for (int i = 0; i < points.size(); i++) {
            if (QLineF(points[i], center).length() <= 12.0) expected << i;
        }
        QCOMPARE(found, expected);
    }
    const QVector<int> nearest = grid.nearest(0, 5);
    QCOMPARE(nearest.size(), 5);
    for (int i = 1; i < points.size(); i++) {
        if (nearest.contains(i)) continue;
        QVERIFY(QLineF(points[i], points[0]).length() >= QLineF(points[nearest.last()], points[0]).length() - 1e-9);
    }
}

void VCurveFitTest::testSpanningTreeIsOneTree()
{
    // two far apart clumps: the neighbour graph leaves them apart
    QVector<QPointF> points;
    for (int i = 0; i < 30; i++) points << QPointF(i, 0) << QPointF(500 + i, 300);
    const QVector<QVector<int>> tree = VCurveFit::spanningTree(points, 4);
    int edges = 0;
    for (const QVector<int> &adjacent : tree) edges += adjacent.size();
    QCOMPARE(edges / 2, points.size() - 1);
    // connected
    QVector<bool> seen(points.size(), false);
    QVector<int> stack{0};
    seen[0] = true;
    while (!stack.isEmpty()) {
        const int i = stack.takeLast();
        for (int j : tree[i]) {
            if (!seen[j]) {
                seen[j] = true;
                stack << j;
            }
        }
    }
    QVERIFY(!seen.contains(false));
}

void VCurveFitTest::testLongestPathOrdersLine()
{
    QVector<QPointF> points;
    QRandomGenerator random(3);
    QVector<int> order(50);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(5));
    for (int i : order) points << QPointF(i * 3.0, random.bounded(0.5));
    const QVector<int> path = VCurveFit::longestPath(VCurveFit::spanningTree(points), points);
    QCOMPARE(path.size(), points.size());
    // monotonic along x, either way round
    const bool increasing = points[path.last()].x() > points[path.first()].x();
    for (int k = 1; k < path.size(); k++) {
        QVERIFY((points[path[k]].x() > points[path[k - 1]].x()) == increasing);
    }
}

void VCurveFitTest::testFitWithinError()
{
    // half a circle of radius 50, densely sampled
    QVector<QPointF> points;
    for (int i = 0; i <= 200; i++) {
        const qreal a = M_PI * i / 200;
        points << QPointF(50 * std::cos(a), 50 * std::sin(a));
    }
    const VCurveFit::FitResult fit = VCurveFit::fitCubics(points, 0.2);
    QVERIFY(!fit.cubics.isEmpty());
    QVERIFY(fit.cubics.size() <= 4);
    QCOMPARE(fit.knots.size(), fit.cubics.size() + 1);
    QCOMPARE(fit.knots.first(), 0);
    QCOMPARE(fit.knots.last(), points.size() - 1);
    for (const VCurveFit::Cubic &c : fit.cubics) {
        for (int s = 0; s <= 20; s++) {
            const qreal t = s / 20.0, u = 1 - t;
            const QPointF p = u * u * u * c.p0 + 3 * u * u * t * c.c1 + 3 * u * t * t * c.c2 + t * t * t * c.p1;
            QVERIFY2(std::abs(std::hypot(p.x(), p.y()) - 50) < 0.3, qPrintable(QString::number(std::hypot(p.x(), p.y()))));
        }
    }
}

KISTEST_MAIN(VCurveFitTest)

#include "VCurveFitTest.moc"
