/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QLineF>
#include <cstdio>
#include <QPolygonF>

#include <KoColorSpaceRegistry.h>
#include <KoParameterShape.h>
#include <KoPathPoint.h>
#include <KoPathShape.h>
#include <KisDocument.h>
#include <KisPart.h>
#include <kis_image.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <kundo2command.h>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent>
#include <KoShapeDeleteCommand.h>
#include <testui.h>

#include "VCurveShape.h"
#include "VDeform.h"
#include "VGeomCurveShape.h"
#include "VMoveSession.h"

/**
 * The deforming transform modes on freehand layers (VDeform): where points
 * and handles go, and what the commands of a transform stroke do together.
 */
class VDeformTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testAffineIsExact();
    void testPerspectiveKeepsLines();
    void testBendingLineBecomesBezier();
    void testBezierFollowsMap();
    void testRadii();
    void testPath();
    void testCommandPasses();
    void testSelectionCuts();
    void testPolygonCuts();
    void testCutPieceOwnership();
    void testCommandComputesInWorker();
    void testPreparedOncePerStroke();
    void testIdentityReadsNothing();
    void testNewPolygonCutsAgain();
    void benchmarkPassGuiTime();
};

namespace {
QPointF bezier(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t)
{
    const qreal s = 1 - t;
    return s * s * s * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t * p3;
}

/// the point of segment \p i of \p curves at \p t
QPointF segmentPoint(const VCurveData &curves, int i, qreal t)
{
    const int next = curves.nextIndex(i);
    if (curves.segment(i) == VCurveData::CurveSegment) {
        return bezier(curves.positions[i], curves.handlesOut[i], curves.handlesIn[next], curves.positions[next], t);
    }
    return curves.positions[i] + (curves.positions[next] - curves.positions[i]) * t;
}

/// how far the traced line of \p moved lands, at most, from \p map of the traced line of \p original
qreal traceError(const VCurveData &original, const VCurveData &moved, const VDeform::PointMap &map)
{
    // the map spaces a segment differently: compare against a dense trace of the mapped original
    qreal worst = 0;
    for (int i = 0; i < original.pointCount(); i++) {
        if (original.nextIndex(i) < 0) continue;
        QPolygonF exact;
        for (int k = 0; k <= 200; k++) exact << map(segmentPoint(original, i, k / 200.0));
        for (int k = 0; k <= 20; k++) {
            const QPointF p = segmentPoint(moved, i, k / 20.0);
            qreal nearest = 1e9;
            for (int e = 0; e + 1 < exact.size(); e++) {
                const QLineF line(exact[e], exact[e + 1]);
                const QPointF d = line.p2() - line.p1();
                const qreal len2 = d.x() * d.x() + d.y() * d.y();
                const qreal u = len2 > 0 ? qBound(0.0, QPointF::dotProduct(p - line.p1(), d) / len2, 1.0) : 0;
                nearest = qMin(nearest, QLineF(p, line.p1() + d * u).length());
            }
            worst = qMax(worst, nearest);
        }
    }
    return worst;
}

VCurveData polyline(const QVector<QPointF> &points, qreal radius = 2)
{
    VCurveData curves;
    curves.beginCurve();
    for (const QPointF &p : points) curves.addPoint(p, radius);
    return curves;
}

QTransform perspective()
{
    QTransform t;
    QPolygonF from({QPointF(0, 0), QPointF(400, 0), QPointF(400, 300), QPointF(0, 300)});
    QPolygonF to({QPointF(40, 20), QPointF(360, 60), QPointF(420, 280), QPointF(-10, 310)});
    QTransform::quadToQuad(from, to, t);
    return t;
}

/**
 * A freehand layer for one test, outside the image's graph: nothing
 * renders, the shapes only change. The document is shared and stays to
 * the end (deleting one with no view waits forever in this setup).
 */
struct Fixture {
    Fixture()
    {
        static KisDocument *s_doc = nullptr;
        static KisImageSP s_image;
        if (!s_doc) {
            s_image = new KisImage(0, 600, 400, KoColorSpaceRegistry::instance()->rgb8(), "deform");
            s_doc = KisPart::instance()->createDocument();
            s_doc->setCurrentImage(s_image);
        }
        image = s_image;
        doc = s_doc;
        layer = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
        layer->setNodeProperty("freehandvector", true);
    }

    VGeomCurveShape *addCurve(const VCurveData &curves)
    {
        VGeomCurveShape *shape = new VGeomCurveShape();
        shape->setCurves(curves);
        layer->addShape(shape);
        return shape;
    }

    /// curves of \p shape in image pixels
    QTransform toImage(KoShape *shape) const
    {
        return shape->absoluteTransformation() * QTransform::fromScale(image->xRes(), image->yRes());
    }

    KisImageSP image;
    KisDocument *doc = nullptr;
    KisShapeLayerSP layer;
};
}

void VDeformTest::testAffineIsExact()
{
    VCurveData curves = polyline({QPointF(10, 10), QPointF(110, 10), QPointF(110, 80)});
    curves.setCurveSegment(1, QPointF(130, 30), QPointF(140, 60));
    QTransform affine;
    affine.translate(30, -5).rotate(20).scale(1.5, 0.75).shear(0.2, 0);
    const VCurveData moved = VDeform::mapped(curves, QTransform(), [affine] (const QPointF &p) { return affine.map(p); }, false);

    QCOMPARE(moved.pointCount(), 3);
    QCOMPARE(moved.segment(0), VCurveData::LineSegment);
    QCOMPARE(moved.segment(1), VCurveData::CurveSegment);
    for (int i = 0; i < 3; i++) {
        QVERIFY(QLineF(moved.positions[i], affine.map(curves.positions[i])).length() < 1e-6);
        QVERIFY(QLineF(moved.handlesOut[i], affine.map(curves.handlesOut[i])).length() < 1e-6);
        QVERIFY(QLineF(moved.handlesIn[i], affine.map(curves.handlesIn[i])).length() < 1e-6);
    }
    QCOMPARE(moved.radii, curves.radii);
}

void VDeformTest::testPerspectiveKeepsLines()
{
    // a long line stays a line: perspective only spaces it unevenly
    const VCurveData curves = polyline({QPointF(20, 20), QPointF(380, 30), QPointF(300, 280)});
    const QTransform t = perspective();
    const VDeform::PointMap map = [t] (const QPointF &p) { return t.map(p); };
    const VCurveData moved = VDeform::mapped(curves, QTransform(), map, false);
    QVERIFY(!moved.hasCurveSegments());
    for (int i = 0; i < curves.pointCount(); i++) {
        QVERIFY(QLineF(moved.positions[i], t.map(curves.positions[i])).length() < 1e-6);
    }
    QVERIFY(traceError(curves, moved, map) < 1e-6);
}

void VDeformTest::testBendingLineBecomesBezier()
{
    // a map that bends: a line becomes the bezier along the bend; a parabola is exact
    const VDeform::PointMap bend = [] (const QPointF &p) { return QPointF(p.x(), p.y() + 0.002 * p.x() * p.x()); };
    const VCurveData curves = polyline({QPointF(0, 50), QPointF(300, 50)});
    const VCurveData moved = VDeform::mapped(curves, QTransform(), bend, false);
    QCOMPARE(moved.segment(0), VCurveData::CurveSegment);
    const qreal error = traceError(curves, moved, bend);
    QVERIFY2(error < 0.05, qPrintable(QString::number(error)));

    // short segments it hardly bends stay lines
    QVector<QPointF> dense;
    for (int x = 0; x <= 300; x += 3) dense << QPointF(x, 50);
    const VCurveData denseMoved = VDeform::mapped(polyline(dense), QTransform(), bend, false);
    QVERIFY(!denseMoved.hasCurveSegments());

    // in a shape's own units: the tolerance is in image pixels
    const QTransform toImage = QTransform::fromScale(4, 4);
    const VDeform::PointMap bendImage = [] (const QPointF &p) { return QPointF(p.x(), p.y() + 0.0005 * p.x() * p.x()); };
    const VCurveData unitCurves = polyline({QPointF(0, 10), QPointF(75, 10)});
    const VCurveData unitMoved = VDeform::mapped(unitCurves, toImage, bendImage, false);
    QCOMPARE(unitMoved.segment(0), VCurveData::CurveSegment);
    const VDeform::PointMap local = [toImage, bendImage] (const QPointF &p) { return toImage.inverted().map(bendImage(toImage.map(p))); };
    QVERIFY(traceError(unitCurves, unitMoved, local) < 0.05 / 4);
}

void VDeformTest::testBezierFollowsMap()
{
    VCurveData curves = polyline({QPointF(50, 200), QPointF(200, 60), QPointF(350, 200)});
    curves.setCurveSegment(0, QPointF(60, 120), QPointF(140, 60));
    curves.setCurveSegment(1, QPointF(260, 60), QPointF(340, 120));
    curves.setFlag(1, VCurveData::SmoothPoint);
    const QTransform t = perspective();
    const VDeform::PointMap map = [t] (const QPointF &p) { return t.map(p); };
    const VCurveData moved = VDeform::mapped(curves, QTransform(), map, false);
    const qreal error = traceError(curves, moved, map);
    QVERIFY2(error < 1.0, qPrintable(QString::number(error)));

    // the smooth point stays smooth: its handles stay on one line through it
    const QPointF in = moved.handlesIn[1] - moved.positions[1];
    const QPointF out = moved.handlesOut[1] - moved.positions[1];
    QVERIFY(qAbs(in.x() * out.y() - in.y() * out.x()) < 1e-6 * QLineF(QPointF(), in).length() * QLineF(QPointF(), out).length());
}

void VDeformTest::testRadii()
{
    const VCurveData curves = polyline({QPointF(10, 10), QPointF(20, 10)}, 3);
    const VDeform::PointMap twice = [] (const QPointF &p) { return p * 2; };
    QCOMPARE(VDeform::mapped(curves, QTransform(), twice, false).radii, curves.radii);
    const VCurveData scaled = VDeform::mapped(curves, QTransform(), twice, true);
    QVERIFY(qAbs(scaled.radii[0] - 6) < 1e-6);
    QVERIFY(qAbs(scaled.radii[1] - 6) < 1e-6);
}

void VDeformTest::testPath()
{
    Fixture f;
    KoPathShape *path = new KoPathShape();
    path->moveTo(QPointF(20, 20));
    path->lineTo(QPointF(80, 20));
    path->curveTo(QPointF(90, 40), QPointF(90, 60), QPointF(80, 80));
    path->close();
    path->normalize();
    f.layer->addShape(path);

    const QTransform toImage = f.toImage(path);
    QVector<QPointF> before;
    for (int i = 0; i < path->pointCount(); i++) before << toImage.map(path->pointByIndex(KoPathPointIndex(0, i))->point());

    const QTransform t = perspective();
    VDeform::mapPath(path, QTransform::fromScale(f.image->xRes(), f.image->yRes()), [t] (const QPointF &p) { return t.map(p); });
    const QTransform after = f.toImage(path);
    for (int i = 0; i < path->pointCount(); i++) {
        const QPointF now = after.map(path->pointByIndex(KoPathPointIndex(0, i))->point());
        QVERIFY2(QLineF(now, t.map(before[i])).length() < 1e-3, qPrintable(QString("point %1").arg(i)));
    }
    QVERIFY(path->isClosedSubpath(0));
}

void VDeformTest::testCommandPasses()
{
    Fixture f;
    const VCurveData original = polyline({QPointF(20, 20), QPointF(120, 40), QPointF(200, 100)});
    VGeomCurveShape *curve = f.addCurve(original);
    KoPathShape *path = new KoPathShape();
    path->moveTo(QPointF(30, 30));
    path->lineTo(QPointF(90, 30));
    path->lineTo(QPointF(90, 90));
    path->normalize();
    f.layer->addShape(path);
    const QTransform pathTransform = path->transformation();
    const QPointF pathPoint = path->pointByIndex(KoPathPointIndex(0, 1))->point();

    const QTransform t1 = QTransform::fromTranslate(10, 0);
    const QTransform t2 = perspective();
    std::shared_ptr<void> session;
    auto map = [] (const QTransform &t) {
        KisShapeLayerHooks::DeformMap m;
        m.map = [t] (const QPointF &p) { return t.map(p); };
        return m;
    };
    const QTransform toImage = f.toImage(curve);
    auto expected = [&] (const QTransform &t) {
        return VDeform::mapped(original, toImage, [t] (const QPointF &p) { return t.map(p); }, false);
    };

    // the stroke's passes: each maps from where the shapes were before the stroke
    QScopedPointer<KUndo2Command> first(VDeform::command(f.layer.data(), map(t1), session));
    QVERIFY(first);
    first->redo();
    QCOMPARE(curve->curves(), expected(t1));
    QScopedPointer<KUndo2Command> second(VDeform::command(f.layer.data(), map(t2), session));
    second->redo();
    QCOMPARE(curve->curves(), expected(t2));
    // the earlier command's undo does nothing now
    first->undo();
    QCOMPARE(curve->curves(), expected(t2));
    const QPointF moved = f.toImage(path).map(path->pointByIndex(KoPathPointIndex(0, 1))->point());
    QVERIFY(QLineF(moved, t2.map((pathTransform * QTransform::fromScale(f.image->xRes(), f.image->yRes())).map(pathPoint))).length() < 1e-3);

    // while the stroke runs, the last pass's undo leaves things to the next pass
    second->undo();
    QCOMPARE(curve->curves(), expected(t2));
    // the stroke ends keeping its commands: the last one holds the change
    VDeform::done(f.layer.data(), session, true);
    second->undo();
    QCOMPARE(curve->curves(), original);
    QCOMPARE(path->transformation(), pathTransform);
    QCOMPARE(path->pointByIndex(KoPathPointIndex(0, 1))->point(), pathPoint);
    QVERIFY(!path->pointByIndex(KoPathPointIndex(0, 1))->activeControlPoint1());

    second->redo();
    QCOMPARE(curve->curves(), expected(t2));
    second->undo();
    QCOMPARE(curve->curves(), original);
}

void VDeformTest::testSelectionCuts()
{
    Fixture f;
    const qreal res = f.image->xRes();
    QVector<QPointF> points;
    for (int x = 20; x <= 300; x += 10) points << QPointF(x, 100) / res;
    const VCurveData original = polyline(points, 1 / res);
    VGeomCurveShape *curve = f.addCurve(original);

    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 150, 400));

    std::shared_ptr<void> session;
    KisShapeLayerHooks::DeformMap map;
    map.map = [] (const QPointF &p) { return p + QPointF(0, 50); };
    map.selection = selection;
    QScopedPointer<KUndo2Command> command(VDeform::command(f.layer.data(), map, session));
    command->redo();

    // the part inside moved as a new shape; the rest stayed
    QCOMPARE(f.layer->shapes().size(), 2);
    VCurveShape *piece = nullptr;
    Q_FOREACH (KoShape *shape, f.layer->shapes()) {
        if (shape != curve) piece = dynamic_cast<VCurveShape*>(shape);
    }
    QVERIFY(piece);
    const QTransform toImage = f.toImage(curve);
    Q_FOREACH (const QPointF &p, curve->curves().positions) {
        QCOMPARE(toImage.map(p).y(), 100.0);
        QVERIFY(toImage.map(p).x() >= 148);
    }
    Q_FOREACH (const QPointF &p, piece->curves().positions) {
        QVERIFY(qAbs(toImage.map(p).y() - 150.0) < 1e-6);
        QVERIFY(toImage.map(p).x() <= 152);
    }

    VDeform::done(f.layer.data(), session, true);
    command->undo();
    QCOMPARE(f.layer->shapes().size(), 1);
    QCOMPARE(curve->curves(), original);
    command->redo();
    QCOMPARE(f.layer->shapes().size(), 2);
    command->undo();
}

void VDeformTest::testPolygonCuts()
{
    Fixture f;
    const qreal res = f.image->xRes();
    QVector<QPointF> points;
    for (int x = 20; x <= 300; x += 10) points << QPointF(x, 100) / res;
    VGeomCurveShape *curve = f.addCurve(polyline(points, 1 / res));
    // a path all inside moves; one reaching outside stays
    KoPathShape *inside = new KoPathShape();
    inside->moveTo(QPointF(30, 150) / res);
    inside->lineTo(QPointF(60, 150) / res);
    inside->normalize();
    f.layer->addShape(inside);
    KoPathShape *across = new KoPathShape();
    across->moveTo(QPointF(30, 200) / res);
    across->lineTo(QPointF(300, 200) / res);
    across->normalize();
    f.layer->addShape(across);
    const QTransform insideBefore = inside->transformation();
    const QTransform acrossBefore = across->transformation();

    std::shared_ptr<void> session;
    KisShapeLayerHooks::DeformMap map;
    map.map = [] (const QPointF &p) { return p + QPointF(0, 30); };
    map.inside = QPolygonF(QRectF(0, 0, 150, 400));
    QScopedPointer<KUndo2Command> command(VDeform::command(f.layer.data(), map, session));
    command->redo();
    QCOMPARE(f.layer->shapes().size(), 4);
    Q_FOREACH (const QPointF &p, curve->curves().positions) QVERIFY(f.toImage(curve).map(p).x() >= 148);
    QVERIFY(inside->transformation() != insideBefore);
    QCOMPARE(across->transformation(), acrossBefore);
    VDeform::done(f.layer.data(), session, true);
    command->undo();
    QCOMPARE(f.layer->shapes().size(), 3);
    QCOMPARE(inside->transformation(), insideBefore);
}

namespace {
/// counts the deletions of the shapes it listens to
struct DeleteCounter : public KoShape::ShapeChangeListener {
    int deleted = 0;
    void notifyShapeChanged(KoShape::ChangeType type, KoShape *) override
    {
        if (type == KoShape::Deleted) deleted++;
    }
};
}

void VDeformTest::testCutPieceOwnership()
{
    // the piece a cut moves is the session's only while it's out of the
    // layer: deleted by the user after the stroke, it's the delete command's,
    // whichever of the two commands goes first (audit F3)
    enum Order { SessionFirst, DeleteFirst, UndoneBeforeDelete };
    for (const Order order : {SessionFirst, DeleteFirst, UndoneBeforeDelete}) {
        Fixture f;
        const qreal res = f.image->xRes();
        QVector<QPointF> points;
        for (int x = 20; x <= 300; x += 10) points << QPointF(x, 100) / res;
        VGeomCurveShape *curve = f.addCurve(polyline(points, 1 / res));

        std::shared_ptr<void> session;
        KisShapeLayerHooks::DeformMap map;
        map.map = [] (const QPointF &p) { return p + QPointF(0, 30); };
        map.inside = QPolygonF(QRectF(0, 0, 150, 400));
        QScopedPointer<KUndo2Command> command(VDeform::command(f.layer.data(), map, session));
        command->redo();
        VDeform::done(f.layer.data(), session, true);
        QCOMPARE(f.layer->shapes().size(), 2);
        KoShape *piece = nullptr;
        Q_FOREACH (KoShape *shape, f.layer->shapes()) {
            if (shape != curve) piece = shape;
        }
        QVERIFY(piece);
        DeleteCounter counter;
        piece->addShapeChangeListener(&counter);

        if (order == UndoneBeforeDelete) {
            // back out of the layer: the session's again, and gone with it
            command->undo();
            QCOMPARE(f.layer->shapes().size(), 1);
            command.reset();
            session.reset();
            QCOMPARE(counter.deleted, 1);
            continue;
        }

        QScopedPointer<KUndo2Command> remove(new KoShapeDeleteCommand(f.doc->shapeController(), piece));
        remove->redo();
        QCOMPARE(f.layer->shapes().size(), 1);
        if (order == SessionFirst) {
            command.reset();
            session.reset();
            QCOMPARE(counter.deleted, 0);
            remove.reset();
        } else {
            remove.reset();
            QCOMPARE(counter.deleted, 1);
            command.reset();
            session.reset();
        }
        QCOMPARE(counter.deleted, 1);
    }
}

namespace {

/// runs \p command's redo in a worker thread, keeping this (the GUI) thread's events going meanwhile
void redoInWorker(KUndo2Command *command)
{
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([command] () { command->redo(); }));
    if (!watcher.isFinished()) loop.exec();
}

}

void VDeformTest::testCommandComputesInWorker()
{
    Fixture f;
    const VCurveData original = polyline({QPointF(20, 20), QPointF(120, 40), QPointF(200, 100), QPointF(260, 60)});
    VGeomCurveShape *curve = f.addCurve(original);
    const QTransform toImage = f.toImage(curve);

    // the map is asked in the worker only, all at once
    QAtomicInt guiCalls = 0;
    QAtomicInt batches = 0;
    QThread *gui = QThread::currentThread();
    auto map = [&] (const QTransform &t) {
        KisShapeLayerHooks::DeformMap m;
        m.map = [t, gui, &guiCalls] (const QPointF &p) {
            if (QThread::currentThread() == gui) guiCalls.ref();
            return t.map(p);
        };
        m.mapAll = [t, gui, &guiCalls, &batches] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
            if (QThread::currentThread() == gui) guiCalls.ref();
            batches.ref();
            mapped.resize(points.size());
            for (int i = 0; i < points.size(); i++) mapped[i] = t.map(points[i]);
        };
        m.changed = std::make_shared<KisShapeLayerHooks::DeformMap::ChangedArea>();
        return m;
    };
    auto expected = [&] (const QTransform &t) {
        return VDeform::mapped(original, toImage, [t] (const QPointF &p) { return t.map(p); }, false);
    };

    std::shared_ptr<void> session;
    const QTransform t1 = QTransform::fromTranslate(30, 10);
    const QRect before = toImage.mapRect(original.controlBounds()).toAlignedRect();
    KisShapeLayerHooks::DeformMap first = map(t1);
    QScopedPointer<KUndo2Command> one(VDeform::command(f.layer.data(), first, session));
    redoInWorker(one.data());
    QCOMPARE(curve->curves(), expected(t1));
    QCOMPARE(int(guiCalls), 0);
    QCOMPARE(int(batches), 1);
    // the changed area holds where the line was and is
    QVERIFY(first.changed->reported);
    QVERIFY(first.changed->rect.contains(before));
    QVERIFY(first.changed->rect.contains(t1.mapRect(QRectF(before)).toAlignedRect()));

    // the second pass reuses the samples and maps from the start again
    const QTransform t2 = perspective();
    KisShapeLayerHooks::DeformMap second = map(t2);
    QScopedPointer<KUndo2Command> two(VDeform::command(f.layer.data(), second, session));
    redoInWorker(two.data());
    QCOMPARE(curve->curves(), expected(t2));
    QCOMPARE(int(guiCalls), 0);
    QCOMPARE(int(batches), 2);
    QVERIFY(second.changed->reported);

    VDeform::done(f.layer.data(), session, true);
    two->undo();
    QCOMPARE(curve->curves(), original);
}

void VDeformTest::benchmarkPassGuiTime()
{
    // 1000 strokes of 60 points: a pass's GUI-thread time, mapping there (before Step A) and
    // only setting curves there (the compute in the worker)
    Fixture f;
    for (int k = 0; k < 1000; k++) {
        QVector<QPointF> points;
        for (int i = 0; i < 60; i++) points << QPointF(10 + (k % 40) * 14 + i * 0.2, 10 + (k / 40) * 14 + 4 * std::sin(i * 0.3));
        f.addCurve(polyline(points));
    }
    std::shared_ptr<void> sessionData;
    KisShapeLayerHooks::DeformMap begin;
    begin.map = [] (const QPointF &p) { return p; };
    QScopedPointer<KUndo2Command> first(VDeform::command(f.layer.data(), begin, sessionData));
    first->redo();
    VMoveSession *session = static_cast<VMoveSession*>(sessionData.get());
    QVERIFY(session->hasBegun());

    const QTransform t = perspective();
    const VDeform::PointMap pointMap = [t] (const QPointF &p) { return t.map(p); };
    const VDeform::BatchMap batchMap = [t] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
        mapped.resize(points.size());
        for (int i = 0; i < points.size(); i++) mapped[i] = t.map(points[i]);
    };

    // before: every curve mapped point by point inside the GUI call
    QElapsedTimer timer;
    timer.start();
    session->apply(nullptr,
                   [&] (const VCurveData &curves, const VCurveData &, const QTransform &toImage) {
                       return VDeform::mapped(curves, toImage, pointMap, false);
                   },
                   VMoveSession::PathTarget(), QTransform());
    const qint64 oldGui = timer.elapsed();

    // after: the worker's share (samples once per stroke, then the batch map), then the GUI's
    const QTransform t3 = QTransform::fromTranslate(5, 5) * t;
    const VDeform::BatchMap batch3 = [t3] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
        mapped.resize(points.size());
        for (int i = 0; i < points.size(); i++) mapped[i] = t3.map(points[i]);
    };
    timer.restart();
    const VMoveSession::Snapshot snapshot = session->snapshot();
    QVector<VDeform::Samples> samples;
    for (int i = 0; i < snapshot.curves.size(); i++) samples << VDeform::samples(snapshot.curves[i], snapshot.toImage[i]);
    const qint64 samplesMs = timer.restart();
    QVector<VCurveData> targets(snapshot.curves.size());
    for (int i = 0; i < snapshot.curves.size(); i++) targets[i] = VDeform::mapped(snapshot.curves[i], samples[i], batch3, false);
    const qint64 computeMs = timer.restart();
    const QRect changed = session->commit(nullptr, targets, VMoveSession::PathTarget(), QTransform());
    const qint64 commitMs = timer.elapsed();
    Q_UNUSED(batchMap);

    printf("1000 strokes x 60 points, one pass: GUI thread %lld ms before; now worker %lld ms "
           "(samples, first pass only) + %lld ms (map), GUI thread %lld ms\n",
           oldGui, samplesMs, computeMs, commitMs);
    fflush(stdout);
    QVERIFY(!changed.isEmpty());
    VDeform::done(f.layer.data(), sessionData, false);
}

namespace {
/// the shape of \p layer other than \p shape (a cut's moving piece)
VCurveShape *otherCurve(KisShapeLayer *layer, KoShape *shape)
{
    Q_FOREACH (KoShape *candidate, layer->shapes()) {
        if (candidate != shape) return dynamic_cast<VCurveShape*>(candidate);
    }
    return nullptr;
}
}

void VDeformTest::testPreparedOncePerStroke()
{
    // a map that precalculates per point (Cage) is prepared once per part and
    // stroke, and again only when it refuses what it prepared (another cage)
    Fixture f;
    const VCurveData a = polyline({QPointF(20, 20), QPointF(120, 40), QPointF(200, 100)});
    const VCurveData b = polyline({QPointF(30, 150), QPointF(160, 170)});
    VGeomCurveShape *curveA = f.addCurve(a);
    VGeomCurveShape *curveB = f.addCurve(b);

    QAtomicInt prepares = 0;
    QAtomicInt mapsPrepared = 0;
    using Prepared = QPair<int, QVector<QPointF>>;
    auto makeMap = [&] (const QPointF &offset, int cage) {
        KisShapeLayerHooks::DeformMap m;
        m.map = [offset] (const QPointF &p) { return p + offset; };
        m.prepare = [&prepares, cage] (const QVector<QPointF> &points) -> std::shared_ptr<void> {
            prepares.ref();
            return std::make_shared<Prepared>(cage, points);
        };
        m.mapPrepared = [&mapsPrepared, offset, cage] (const std::shared_ptr<void> &prepared, QVector<QPointF> &mapped) {
            const Prepared *p = static_cast<const Prepared*>(prepared.get());
            if (p->first != cage) return false;
            mapsPrepared.ref();
            mapped = p->second;
            for (QPointF &point : mapped) point += offset;
            return true;
        };
        return m;
    };
    auto expected = [&] (VGeomCurveShape *curve, const VCurveData &original, const QPointF &offset) {
        return VDeform::mapped(original, f.toImage(curve), [offset] (const QPointF &p) { return p + offset; }, false);
    };

    std::shared_ptr<void> session;
    QScopedPointer<KUndo2Command> one(VDeform::command(f.layer.data(), makeMap(QPointF(10, 0), 1), session));
    one->redo();
    QCOMPARE(int(prepares), 2);
    QCOMPARE(int(mapsPrepared), 2);
    QCOMPARE(curveA->curves(), expected(curveA, a, QPointF(10, 0)));
    QCOMPARE(curveB->curves(), expected(curveB, b, QPointF(10, 0)));

    QScopedPointer<KUndo2Command> two(VDeform::command(f.layer.data(), makeMap(QPointF(20, 5), 1), session));
    two->redo();
    QCOMPARE(int(prepares), 2);
    QCOMPARE(int(mapsPrepared), 4);
    QCOMPARE(curveA->curves(), expected(curveA, a, QPointF(20, 5)));

    QScopedPointer<KUndo2Command> three(VDeform::command(f.layer.data(), makeMap(QPointF(-5, 5), 2), session));
    three->redo();
    QCOMPARE(int(prepares), 4);
    QCOMPARE(curveB->curves(), expected(curveB, b, QPointF(-5, 5)));

    VDeform::done(f.layer.data(), session, true);
    three->undo();
    QCOMPARE(curveA->curves(), a);
    QCOMPARE(curveB->curves(), b);
}

void VDeformTest::testIdentityReadsNothing()
{
    // a map moving nothing (Cage while the cage is placed) leaves the layer
    // unread; once things moved, it takes them back where they were
    Fixture f;
    const qreal res = f.image->xRes();
    QVector<QPointF> points;
    for (int x = 20; x <= 300; x += 10) points << QPointF(x, 100) / res;
    const VCurveData original = polyline(points, 1 / res);
    VGeomCurveShape *curve = f.addCurve(original);
    const QPolygonF cage(QRectF(0, 0, 150, 400));

    std::shared_ptr<void> session;
    KisShapeLayerHooks::DeformMap identity;
    identity.map = [] (const QPointF &p) { return p; };
    identity.identity = true;
    identity.inside = cage;
    identity.changed = std::make_shared<KisShapeLayerHooks::DeformMap::ChangedArea>();
    QScopedPointer<KUndo2Command> one(VDeform::command(f.layer.data(), identity, session));
    one->redo();
    QVERIFY(!static_cast<VMoveSession*>(session.get())->hasBegun());
    QVERIFY(identity.changed->reported);
    QVERIFY(identity.changed->rect.isEmpty());
    QCOMPARE(f.layer->shapes().size(), 1);
    QCOMPARE(curve->curves(), original);

    KisShapeLayerHooks::DeformMap down;
    down.map = [] (const QPointF &p) { return p + QPointF(0, 30); };
    down.inside = cage;
    QScopedPointer<KUndo2Command> two(VDeform::command(f.layer.data(), down, session));
    two->redo();
    QCOMPARE(f.layer->shapes().size(), 2);
    VCurveShape *piece = otherCurve(f.layer.data(), curve);
    QVERIFY(piece);
    QVERIFY(qAbs(f.toImage(piece).mapRect(piece->curves().controlBounds()).top() - 130) < 0.5);

    identity.changed = std::make_shared<KisShapeLayerHooks::DeformMap::ChangedArea>();
    QScopedPointer<KUndo2Command> three(VDeform::command(f.layer.data(), identity, session));
    three->redo();
    QVERIFY(qAbs(f.toImage(piece).mapRect(piece->curves().controlBounds()).top() - 100) < 0.5);
    QVERIFY(identity.changed->rect.contains(QPoint(60, 130)));

    VDeform::done(f.layer.data(), session, false);
    QCOMPARE(f.layer->shapes().size(), 1);
    QCOMPARE(curve->curves(), original);
}

void VDeformTest::testNewPolygonCutsAgain()
{
    // the cage edited after it moved things: what the old cage cut goes back
    // and the new one cuts; one undo step still holds the stroke
    Fixture f;
    const qreal res = f.image->xRes();
    QVector<QPointF> points;
    for (int x = 20; x <= 300; x += 10) points << QPointF(x, 100) / res;
    const VCurveData original = polyline(points, 1 / res);
    VGeomCurveShape *curve = f.addCurve(original);

    std::shared_ptr<void> session;
    KisShapeLayerHooks::DeformMap map;
    map.map = [] (const QPointF &p) { return p + QPointF(0, 30); };
    map.inside = QPolygonF(QRectF(0, 0, 150, 400));
    QScopedPointer<KUndo2Command> one(VDeform::command(f.layer.data(), map, session));
    one->redo();
    QCOMPARE(f.layer->shapes().size(), 2);
    const std::shared_ptr<void> first = session;

    map.inside = QPolygonF(QRectF(0, 0, 250, 400));
    map.changed = std::make_shared<KisShapeLayerHooks::DeformMap::ChangedArea>();
    QScopedPointer<KUndo2Command> two(VDeform::command(f.layer.data(), map, session));
    QVERIFY(session != first);
    two->redo();
    QVERIFY(!static_cast<VMoveSession*>(first.get())->isLive());
    QCOMPARE(f.layer->shapes().size(), 2);
    VCurveShape *piece = otherCurve(f.layer.data(), curve);
    QVERIFY(piece);
    const QRectF moved = f.toImage(piece).mapRect(piece->curves().controlBounds());
    QVERIFY2(moved.right() > 240 && moved.right() < 260, qPrintable(QString::number(moved.right())));
    QVERIFY(qAbs(moved.top() - 130) < 0.5);
    const QRectF stays = f.toImage(curve).mapRect(curve->curves().controlBounds());
    QVERIFY2(stays.left() > 240, qPrintable(QString::number(stays.left())));
    QVERIFY(qAbs(stays.top() - 100) < 0.5);
    // the area reported holds where the first cut's piece was
    QVERIFY(map.changed->reported);
    QVERIFY(map.changed->rect.contains(QPoint(60, 130)));

    VDeform::done(f.layer.data(), session, true);
    two->undo();
    one->undo();
    QCOMPARE(f.layer->shapes().size(), 1);
    QCOMPARE(curve->curves(), original);
    one->redo();
    two->redo();
    QCOMPARE(f.layer->shapes().size(), 2);
}

KISTEST_MAIN(VDeformTest)
#include "VDeformTest.moc"
