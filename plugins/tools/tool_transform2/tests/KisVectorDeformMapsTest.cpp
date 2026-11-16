/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QLineF>
#include <QtMath>

#include <KisBezierPatch.h>
#include <KisBezierTransformMesh.h>
#include <kis_warptransform_worker.h>

#include "kis_transform_utils.h"
#include "kis_vector_deform_maps.h"

/**
 * The maps vector layers move by in Warp, Cage and Mesh send points where
 * the stock workers send pixels, and the modes are on.
 */
class KisVectorDeformMapsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testModesOn();
    void testWarp();
    void testMesh();
    void testCage();
};

namespace {
qreal distance(const QPointF &a, const QPointF &b)
{
    return QLineF(a, b).length();
}

QVector<QPointF> gridOver(const QRectF &rect, int n)
{
    QVector<QPointF> result;
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= n; j++) {
            result << QPointF(rect.left() + rect.width() * i / n, rect.top() + rect.height() * j / n);
        }
    }
    return result;
}
}

void KisVectorDeformMapsTest::testModesOn()
{
    QVERIFY(KisTransformUtils::isVectorDeformMode(ToolTransformArgs::PERSPECTIVE_4POINT));
    QVERIFY(KisTransformUtils::isVectorDeformMode(ToolTransformArgs::WARP));
    QVERIFY(KisTransformUtils::isVectorDeformMode(ToolTransformArgs::CAGE));
    QVERIFY(KisTransformUtils::isVectorDeformMode(ToolTransformArgs::MESH));
    QVERIFY(!KisTransformUtils::isVectorDeformMode(ToolTransformArgs::FREE_TRANSFORM));
    QVERIFY(!KisTransformUtils::isVectorDeformMode(ToolTransformArgs::LIQUIFY));
}

void KisVectorDeformMapsTest::testWarp()
{
    const QVector<QPointF> orig = {QPointF(0, 0), QPointF(100, 0), QPointF(0, 100), QPointF(100, 100), QPointF(50, 50)};
    const QVector<QPointF> transf = {QPointF(5, 3), QPointF(110, -10), QPointF(-4, 96), QPointF(120, 115), QPointF(60, 40)};
    const QVector<QPointF> points = gridOver(QRectF(-20, -20, 140, 140), 7);
    using Worker = KisWarpTransformWorker;
    for (const Worker::WarpType type : {Worker::AFFINE_TRANSFORM, Worker::SIMILITUDE_TRANSFORM, Worker::RIGID_TRANSFORM}) {
        const KisShapeLayerHooks::DeformMap map = KisVectorDeformMaps::warp(type, orig, transf, 1.0);
        QVERIFY(!map.identity);
        QVERIFY(map.mapAll);
        QVector<QPointF> mapped;
        map.mapAll(points, mapped);
        QCOMPARE(mapped.size(), points.size());
        for (int i = 0; i < points.size(); i++) {
            const QPointF expected = type == Worker::AFFINE_TRANSFORM ? Worker::affineTransformMath(points[i], orig, transf, 1.0)
                                   : type == Worker::SIMILITUDE_TRANSFORM ? Worker::similitudeTransformMath(points[i], orig, transf, 1.0)
                                   : Worker::rigidTransformMath(points[i], orig, transf, 1.0);
            QCOMPARE(map.map(points[i]), expected);
            QCOMPARE(mapped[i], expected);
        }
    }

    // nothing set, or nothing moved: nothing moves
    QVERIFY(KisVectorDeformMaps::warp(Worker::RIGID_TRANSFORM, {}, {}, 1.0).identity);
    QVERIFY(KisVectorDeformMaps::warp(Worker::RIGID_TRANSFORM, orig, orig, 1.0).identity);
    // one point: a translation, as the worker does
    const KisShapeLayerHooks::DeformMap one = KisVectorDeformMaps::warp(Worker::RIGID_TRANSFORM, {QPointF(10, 10)}, {QPointF(15, 7)}, 1.0);
    QCOMPARE(one.map(QPointF(100, 50)), QPointF(105, 47));
}

void KisVectorDeformMapsTest::testMesh()
{
    // a 3 x 3 mesh, its nodes and handles moved: the edges curve, patch by patch
    KisBezierTransformMesh mesh(QRectF(50, 40, 300, 200), QSize(3, 3));
    // untouched: nothing moves
    QVERIFY(KisVectorDeformMaps::mesh(mesh).identity);
    mesh.node(1, 1).translate(QPointF(30, -20));
    mesh.node(2, 0).translate(QPointF(-15, 10));
    mesh.node(0, 2).translate(QPointF(10, 25));
    mesh.node(1, 0).setRightControlRelative(QPointF(60, 35));
    mesh.node(1, 2).setTopControlRelative(QPointF(-20, -45));
    mesh.node(2, 1).setLeftControlRelative(QPointF(-70, 30));

    const KisShapeLayerHooks::DeformMap map = KisVectorDeformMaps::mesh(mesh);
    QVERIFY(!map.identity);
    QVERIFY(map.mapAll);

    // where the raster worker samples its patches, its source points go to its targets
    qreal worst = 0;
    int samples = 0;
    for (auto it = mesh.beginPatches(); it != mesh.endPatches(); ++it) {
        const KisBezierPatch patch = *it;
        QSize gridSize;
        QVector<QPointF> origPoints;
        QVector<QPointF> transfPoints;
        patch.sampleRegularGrid(gridSize, origPoints, transfPoints, QPointF(8, 8));
        QVector<QPointF> mapped;
        map.mapAll(origPoints, mapped);
        for (int i = 0; i < origPoints.size(); i++) {
            worst = qMax(worst, distance(mapped[i], transfPoints[i]));
            samples++;
        }
    }
    QVERIFY(samples > 100);
    // the edge lengths are measured a bit differently (sampled here, adaptive there)
    QVERIFY2(worst < 0.3, qPrintable(QString("worst %1 px").arg(worst)));
    qDebug() << "mesh: worst distance from the raster worker's samples" << worst << "px over" << samples;

    // beyond the mesh it goes on smoothly: no jump at the edge
    const QPointF in = map.map(QPointF(50.01, 140));
    const QPointF out = map.map(QPointF(49.99, 140));
    QVERIFY(distance(in, out) < 0.1);
}

void KisVectorDeformMapsTest::testCage()
{
    const QVector<QPointF> cage = {QPointF(100, 100), QPointF(300, 80), QPointF(320, 260), QPointF(140, 300), QPointF(90, 200)};
    // a similarity: Green coordinates keep it, inside and (by the edges) outside
    QTransform similarity;
    similarity.translate(40, -10);
    similarity.rotate(20);
    similarity.scale(1.3, 1.3);
    QVector<QPointF> moved;
    for (const QPointF &p : cage) moved << similarity.map(p);

    const KisShapeLayerHooks::DeformMap map = KisVectorDeformMaps::cage(cage, moved, false);
    QVERIFY(!map.identity);
    QCOMPARE(map.inside, QPolygonF(cage));
    QVERIFY(map.prepare && map.mapPrepared && map.mapAll);
    const QVector<QPointF> points = gridOver(QRectF(80, 70, 260, 240), 12) + QVector<QPointF>{QPointF(200, 90)};
    for (const QPointF &p : points) {
        QVERIFY2(distance(map.map(p), similarity.map(p)) < 5e-3, qPrintable(QString("%1,%2").arg(p.x()).arg(p.y())));
    }

    // a bend: the three ways to ask agree
    QVector<QPointF> bent = cage;
    bent[1] += QPointF(40, -30);
    bent[3] += QPointF(-20, 35);
    const KisShapeLayerHooks::DeformMap bend = KisVectorDeformMaps::cage(cage, bent, false);
    std::shared_ptr<void> prepared = bend.prepare(points);
    QVector<QPointF> viaPrepared;
    QVERIFY(bend.mapPrepared(prepared, viaPrepared));
    QVector<QPointF> viaAll;
    bend.mapAll(points, viaAll);
    for (int i = 0; i < points.size(); i++) {
        QVERIFY(distance(viaPrepared[i], bend.map(points[i])) < 1e-6);
        QVERIFY(distance(viaAll[i], viaPrepared[i]) < 1e-6);
    }
    // the same prepared points follow the cage as it moves on
    QVector<QPointF> bent2 = bent;
    bent2[0] += QPointF(-10, -10);
    const KisShapeLayerHooks::DeformMap bend2 = KisVectorDeformMaps::cage(cage, bent2, false);
    QVERIFY(bend2.mapPrepared(prepared, viaPrepared));
    QVERIFY(distance(viaPrepared[0], bend2.map(points[0])) < 1e-6);
    // prepared for another cage: refused
    QVector<QPointF> otherCage = cage;
    otherCage[2] += QPointF(5, 5);
    const KisShapeLayerHooks::DeformMap other = KisVectorDeformMaps::cage(otherCage, bent, false);
    QVERIFY(!other.mapPrepared(prepared, viaPrepared));

    // a point on the line of an edge: no division by zero
    const QPointF onLine(200, 90); // on the line from (100, 100) to (300, 80)
    const QPointF result = bend.map(onLine);
    QVERIFY(qIsFinite(result.x()) && qIsFinite(result.y()));

    // the cage being placed or edited moves nothing, and says what it would cut
    const KisShapeLayerHooks::DeformMap editing = KisVectorDeformMaps::cage(cage, bent, true);
    QVERIFY(editing.identity);
    QCOMPARE(editing.inside, QPolygonF(cage));
    const KisShapeLayerHooks::DeformMap open = KisVectorDeformMaps::cage(cage.mid(0, 2), bent.mid(0, 2), false);
    QVERIFY(open.identity);
    QVERIFY(open.inside.isEmpty());
}

SIMPLE_TEST_MAIN(KisVectorDeformMapsTest)

#include "KisVectorDeformMapsTest.moc"
