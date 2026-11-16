/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QMimeData>
#include <QTimer>
#include <KConfigGroup>
#include <KSharedConfig>

#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoPathShape.h>
#include <KoShapeGroup.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <commands/kis_selection_commands.h>
#include <kis_canvas2.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <kis_tool.h>
#include <kundo2stack.h>
#include <testui.h>

#include "VCurveConverter.h"
#include "VGeomCurveShape.h"
#include "VStrokePrimitive.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * Free Transform with a selection on a freehand layer: the strokes are cut at
 * the selection's edge, only what is inside moves, and undo brings back the
 * layer as it was. Each line goes from x = 100 to 300 pt; the selection ends at
 * x = 180 pt.
 */
class VSelectionTransformTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testMovesTheSelectedPart_data();
    void testMovesTheSelectedPart();
};

namespace {
using VWindowTest::settle;

template <typename T>
T *optionChild(KisTool *transform, const QString &name)
{
    Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
        if (!widget) continue;
        if (T *child = widget->findChild<T*>(name)) return child;
    }
    return nullptr;
}

void collect(const QList<KoShape*> &shapes, QList<VCurveShape*> *curves)
{
    for (KoShape *shape : shapes) {
        if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) {
            *curves << curve;
        } else if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
            collect(group->shapes(), curves);
        }
    }
}

/// where a line is, in document points
struct Piece {
    qreal left, right, y, radius;
};

QVector<Piece> pieces(KisShapeLayer *layer)
{
    QList<VCurveShape*> curves;
    collect(layer->shapes(), &curves);
    QVector<Piece> result;
    for (VCurveShape *curve : curves) {
        const QTransform t = curve->absoluteTransformation();
        QPolygonF points;
        Q_FOREACH (const QPointF &p, curve->curves().positions) points << t.map(p);
        const QRectF bounds = points.boundingRect();
        result << Piece{bounds.left(), bounds.right(), bounds.center().y(), curve->curves().radii.first()};
    }
    std::sort(result.begin(), result.end(), [] (const Piece &a, const Piece &b) {
        return a.y != b.y ? a.y < b.y : a.left < b.left;
    });
    return result;
}

QString describe(const QVector<Piece> &list)
{
    QStringList lines;
    for (const Piece &p : list) {
        lines << QString("x %1-%2 y %3 r %4").arg(p.left, 0, 'f', 1).arg(p.right, 0, 'f', 1).arg(p.y, 0, 'f', 1).arg(p.radius, 0, 'f', 2);
    }
    return lines.join("; ");
}

bool hasPiece(const QVector<Piece> &list, qreal left, qreal right, qreal y, qreal tolerance = 1.5)
{
    for (const Piece &p : list) {
        if (qAbs(p.left - left) < tolerance && qAbs(p.right - right) < tolerance && qAbs(p.y - y) < tolerance) return true;
    }
    return false;
}

const Piece *pieceAt(const QVector<Piece> &list, qreal left, qreal y)
{
    for (const Piece &p : list) {
        if (qAbs(p.left - left) < 1.5 && qAbs(p.y - y) < 1.5) return &p;
    }
    return nullptr;
}

VGeomCurveShape *geomLine(qreal y)
{
    KoPathShape path;
    path.moveTo(QPointF(100, y));
    path.lineTo(QPointF(300, y));
    path.normalize();
    return VCurveConverter::fromPath(&path);
}

VStrokeShape *brushLine(qreal y, qreal ppp, KisPaintOpPresetSP preset)
{
    VStrokeData data;
    data.preset = preset;
    data.brushSize = 12;
    QVector<VStrokePrimitive> line;
    KisPaintInformation prev(QPointF(100, y) * ppp, 1.0);
    for (int x = 104; x <= 300; x += 4) {
        KisPaintInformation next(QPointF(x, y) * ppp, 1.0);
        line << VStrokePrimitive::line(prev, next);
        prev = next;
    }
    VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, ppp, data);
    stroke->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
    return stroke;
}
}

void VSelectionTransformTest::testMovesTheSelectedPart_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::addColumn<bool>("overlay");
    QTest::addColumn<bool>("lod");
    QTest::newRow("72 dpi") << 72.0 << false << false;
    QTest::newRow("300 dpi") << 300.0 << false << false;
    QTest::newRow("300 dpi, level of detail") << 300.0 << false << true;
    QTest::newRow("72 dpi, overlay preview") << 72.0 << true << false;
    QTest::newRow("300 dpi, overlay preview") << 300.0 << true << false;
}

void VSelectionTransformTest::testMovesTheSelectedPart()
{
    QFETCH(qreal, dpi);
    QFETCH(bool, overlay);
    QFETCH(bool, lod);

    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("scaleBrushSizes", false);
    transformConfig.writeEntry("useOverlayPreviewStyle", overlay);
    transformConfig.writeEntry("forceLodMode", lod);

    const qreal ppp = dpi / 72.0;
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600 * ppp, 600 * ppp, cs, "selection transform");
    image->setResolution(ppp, ppp);
    KisDocument *doc = VWindowTest::addDocument(image);

    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    image->addNode(raster);
    KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
    freehand->setNodeProperty("freehandvector", true);
    image->addNode(freehand);
    KisShapeLayerSP second = new KisShapeLayer(doc->shapeController(), image, "second", OPACITY_OPAQUE_U8);
    second->setNodeProperty("freehandvector", true);
    image->addNode(second);
    KisShapeLayerSP stock = new KisShapeLayer(doc->shapeController(), image, "stock", OPACITY_OPAQUE_U8);
    image->addNode(stock);

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();

    KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"),
                                                                              KisGlobalResourcesInterface::instance());
    QVERIFY(preset);
    view->canvasBase()->resourceManager()->setResource(KoCanvasResource::CurrentPaintOpPreset,
                                                       QVariant::fromValue(preset));

    // crossing the selection: a brush stroke and a poly-tool line. Outside it: one more
    freehand->addShape(brushLine(100, ppp, preset));
    VGeomCurveShape *geom = geomLine(150);
    QVERIFY(geom);
    freehand->addShape(geom);
    freehand->addShape(brushLine(350, ppp, preset));
    // one in a shape group
    second->addShape(brushLine(120, ppp, preset));
    KoShapeGroup *group = new KoShapeGroup();
    group->addShape(brushLine(170, ppp, preset));
    second->addShape(group);
    settle(image, 300);

    KisSelectionSP selection = new KisSelection();
    const QRect selected(QPoint(50 * ppp, 50 * ppp), QPoint(180 * ppp, 200 * ppp));
    selection->pixelSelection()->select(selected);
    KisSetGlobalSelectionCommand(image, selection).redo();
    settle(image, 300);
    const QVector<Piece> before = pieces(freehand.data());
    QCOMPARE(before.size(), 3);
    const qreal radius = before[0].radius;

    auto start = [&] (KisNodeSP active, const KisNodeList &list) -> KisTool* {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        settle(image, 200);
        nodes->slotUiActivatedNode(active);
        nodes->slotSetSelectedNodes(list);
        settle(image, 200);
        tools->switchToolRequested("KisToolTransform");
        // settle() would end the stroke: waitForDone() asks the tool to
        QTest::qWait(600);
        return dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
    };
    auto recalculate = [&] (KisTool *transform) {
        QVERIFY(QMetaObject::invokeMethod(transform, "slotEditingFinished"));
        QVERIFY(QMetaObject::invokeMethod(transform, "requestImageRecalculation"));
        QTest::qWait(500);
    };
    auto translate = [&] (KisTool *transform, qreal dx, qreal dy) {
        transform->setProperty("translateX", transform->property("translateX").toDouble() + dx * ppp);
        transform->setProperty("translateY", transform->property("translateY").toDouble() + dy * ppp);
        recalculate(transform);
    };
    auto finish = [&] (KisTool *transform) {
        QMetaObject::invokeMethod(transform, "requestStrokeEnd");
        settle(image, 400);
        Q_FOREACH (KoShape *shape, freehand->shapes()) {
            if (VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(shape)) stroke->waitForRendering();
        }
        freehand->forceUpdateTimedNode();
        settle(image, 200);
    };
    auto undo = [&] () {
        doc->undoStack()->undo();
        settle(image, 400);
    };
    auto expectBefore = [&] (const char *when) {
        const QVector<Piece> now = pieces(freehand.data());
        QVERIFY2(now.size() == 3, qPrintable(QString("%1: %2").arg(when, describe(now))));
        QVERIFY2(hasPiece(now, 100, 300, 100, 0.01) && hasPiece(now, 100, 300, 150, 0.01) && hasPiece(now, 100, 300, 350, 0.01),
                 qPrintable(QString("%1: %2").arg(when, describe(now))));
    };
    auto expectMoved = [&] (qreal dx, qreal dy, const char *when) {
        const QVector<Piece> now = pieces(freehand.data());
        const QString text = QString("%1: %2").arg(when, describe(now));
        QVERIFY2(now.size() == 5, qPrintable(text));
        // what stays, cut at the edge, and the stroke outside
        QVERIFY2(hasPiece(now, 180, 300, 100) && hasPiece(now, 180, 300, 150), qPrintable(text));
        QVERIFY2(hasPiece(now, 100, 300, 350, 0.01), qPrintable(text));
        // what moves
        QVERIFY2(hasPiece(now, 100 + dx, 180 + dx, 100 + dy) && hasPiece(now, 100 + dx, 180 + dx, 150 + dy), qPrintable(text));
    };

    expectBefore("at the start");

    // a stock vector layer isn't cut: it moves as a whole, as in stock Krita
    if (!overlay) {
        KoPathShape *path = new KoPathShape();
        path->moveTo(QPointF(100, 100));
        path->lineTo(QPointF(300, 100));
        path->lineTo(QPointF(300, 140));
        path->close();
        path->normalize();
        stock->addShape(path);
        settle(image, 300);
        KisTool *transform = start(stock, {stock});
        translate(transform, 40, 200);
        finish(transform);
        QCOMPARE(stock->shapes().size(), 1);
        QVERIFY(stock->transformation() != QTransform());
        undo();
        QCOMPARE(stock->transformation(), QTransform());
    }

    // a move: one undo step, and the selection goes along
    {
        const int steps = doc->undoStack()->index();
        const QRect selectionBefore = image->globalSelection()->selectedExactRect();
        KisTool *transform = start(freehand, {freehand});
        QVERIFY(transform);
        translate(transform, 40, 200);
        finish(transform);
        expectMoved(40, 200, "moved");
        QCOMPARE(freehand->transformation(), QTransform());
        QCOMPARE(doc->undoStack()->index(), steps + 1);
        QCOMPARE(image->globalSelection()->selectedExactRect().topLeft(),
                 selectionBefore.topLeft() + QPoint(qRound(40 * ppp), qRound(200 * ppp)));

        // the paint: the moved part where it went, nothing left where it was
        const QRect painted = freehand->projection()->exactBounds();
        KoColor color(cs);
        freehand->projection()->pixel(140 * ppp, 300 * ppp, &color);
        QVERIFY2(color.opacityU8() > 127, qPrintable(QString("moved part not painted, bounds %1,%2 %3x%4")
                                                     .arg(painted.x()).arg(painted.y()).arg(painted.width()).arg(painted.height())));
        freehand->projection()->pixel(140 * ppp, 100 * ppp, &color);
        QVERIFY2(color.opacityU8() < 128, "the moved part is still painted where it was");
        freehand->projection()->pixel(250 * ppp, 100 * ppp, &color);
        QVERIFY2(color.opacityU8() > 127, "the part that stays isn't painted");

        undo();
        expectBefore("after undo");
        QCOMPARE(image->globalSelection()->selectedExactRect(), selectionBefore);
        doc->undoStack()->redo();
        settle(image, 400);
        expectMoved(40, 200, "after redo");
        undo();
        expectBefore("after the second undo");
    }

    // a scale around the selection's center: no brush size change by default
    const QPointF center = QRectF(selected).center() / ppp;
    auto scaled = [&] (qreal v, qreal c) { return c + 2 * (v - c); };
    for (const bool on : {false, true}) {
        KisTool *transform = start(freehand, {freehand});
        QVERIFY(transform);
        QCheckBox *box = optionChild<QCheckBox>(transform, "chkScaleBrushSizes");
        QVERIFY(box);
        box->setChecked(on);
        QTest::qWait(100);
        transform->setProperty("scaleX", 200.0);
        transform->setProperty("scaleY", 200.0);
        recalculate(transform);
        finish(transform);

        const QVector<Piece> now = pieces(freehand.data());
        const QString text = QString("scaled, brush size %1: %2").arg(on ? "on" : "off", describe(now));
        QVERIFY2(now.size() == 5, qPrintable(text));
        QVERIFY2(hasPiece(now, 180, 300, 100) && hasPiece(now, 180, 300, 150), qPrintable(text));
        QVERIFY2(hasPiece(now, scaled(100, center.x()), scaled(180, center.x()), scaled(100, center.y()), 3), qPrintable(text));
        // only the moved part changes brush size
        const Piece *stays = pieceAt(now, 180, 100);
        const Piece *moved = pieceAt(now, scaled(100, center.x()), scaled(100, center.y()));
        QVERIFY2(stays && moved, qPrintable(text));
        QVERIFY2(qAbs(stays->radius - radius) < 1e-6, qPrintable(text));
        QVERIFY2(qAbs(moved->radius - radius * (on ? 2 : 1)) < 1e-6, qPrintable(text));

        box->setChecked(false);
        undo();
        expectBefore("after undoing a scale");
    }

    // Cancel leaves it as it was, and so does applying with no change
    {
        KisTool *transform = start(freehand, {freehand});
        translate(transform, 40, 200);
        const int steps = doc->undoStack()->index();
        QVERIFY(QMetaObject::invokeMethod(transform, "requestStrokeCancellation"));
        settle(image, 400);
        expectBefore("after Cancel");
        QCOMPARE(doc->undoStack()->index(), steps);

        transform = start(freehand, {freehand});
        finish(transform);
        expectBefore("after no change");
        QCOMPARE(doc->undoStack()->index(), steps);
    }

    // a layer moved before: its own transform counts, and stays
    {
        const QTransform moved = QTransform::fromTranslate(10, 10);
        freehand->setTransformation(moved);
        settle(image, 300);
        KisTool *transform = start(freehand, {freehand});
        translate(transform, 40, 200);
        finish(transform);
        const QVector<Piece> now = pieces(freehand.data());
        const QString text = QString("layer moved before: %1").arg(describe(now));
        QVERIFY2(now.size() == 5, qPrintable(text));
        QVERIFY2(hasPiece(now, 180, 310, 110) && hasPiece(now, 180, 310, 160), qPrintable(text));
        QVERIFY2(hasPiece(now, 150, 220, 310) && hasPiece(now, 150, 220, 360), qPrintable(text));
        QCOMPARE(freehand->transformation(), moved);
        undo();
        freehand->setTransformation(QTransform());
        settle(image, 300);
        expectBefore("after the layer moved back");
    }

    // several layers, one with a stroke in a shape group, and a raster layer
    {
        raster->paintDevice()->fill(QRect(QPoint(60 * ppp, 60 * ppp), QPoint(80 * ppp, 80 * ppp)), KoColor(Qt::red, cs));
        KisTool *transform = start(freehand, {raster, freehand, second});
        translate(transform, 40, 200);
        finish(transform);
        expectMoved(40, 200, "several layers");
        const QVector<Piece> now = pieces(second.data());
        const QString text = QString("second layer: %1").arg(describe(now));
        QVERIFY2(now.size() == 4, qPrintable(text));
        QVERIFY2(hasPiece(now, 180, 300, 120) && hasPiece(now, 180, 300, 170), qPrintable(text));
        QVERIFY2(hasPiece(now, 140, 220, 320) && hasPiece(now, 140, 220, 370), qPrintable(text));
        QVERIFY2(qAbs(raster->paintDevice()->exactBounds().left() - 100 * ppp) <= 2, "the raster layer didn't move");
        // the piece of the line in the group paints too, where it went
        QList<VCurveShape*> secondCurves;
        collect(second->shapes(), &secondCurves);
        for (VCurveShape *curve : secondCurves) {
            if (VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(curve)) stroke->waitForRendering();
        }
        second->forceUpdateTimedNode();
        settle(image, 300);
        KoColor inGroup(cs);
        second->projection()->pixel(180 * ppp, 370 * ppp, &inGroup);
        QVERIFY2(inGroup.opacityU8() > 127, "the moved piece of the stroke in the group isn't painted");
        undo();
        expectBefore("after undoing several layers");
        QCOMPARE(image->globalSelection()->selectedExactRect(), selected);
        QCOMPARE(pieces(second.data()).size(), 2);
    }

    tools->switchToolRequested("KritaShape/KisToolBrush");
    settle(image, 200);
    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    transformConfig.writeEntry("forceLodMode", false);
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VSelectionTransformTest)
#include "VSelectionTransformTest.moc"
