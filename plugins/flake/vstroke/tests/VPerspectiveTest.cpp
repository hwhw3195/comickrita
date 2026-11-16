/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <KConfigGroup>
#include <KSharedConfig>
#include <cstdio>

#include <KoCanvasController.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoPointerEvent.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <kactioncollection.h>
#include <kis_canvas2.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_shape_layer.h>
#include <kis_tool.h>
#include <kundo2stack.h>
#include <testui.h>

#include "VCurveShape.h"
#include "VMoveSession.h"
#include "VStrokePrimitive.h"
#include "VStrokeRenderCache.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * The transform tool's Perspective on a freehand layer, next to a raster
 * layer in the same stroke: the brush stroke's points go where the raster
 * layer's pixels go, in Accurate and Fast, and an undo brings them back.
 * While dragging, the stroke never shows a rendering of another pose unbent
 * and never goes back to where it started between passes. Switching from
 * Fast to Accurate right after a drag leaves nothing holding renders back.
 */
class VPerspectiveTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPerspective();
    void testFollowsLayerSelection();
};

namespace {
QComboBox *previewModes(KisTool *transform)
{
    Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
        if (!widget) continue;
        if (QComboBox *combo = widget->findChild<QComboBox*>("cmbPreviewMode")) return combo;
    }
    return nullptr;
}

QToolButton *modeButton(KisTool *transform, const QString &name)
{
    Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
        if (!widget) continue;
        if (QToolButton *button = widget->findChild<QToolButton*>(name)) return button;
    }
    return nullptr;
}

/// the middle of the opaque pixels of \p device within \p rect
QPointF centroid(KisPaintDeviceSP device, const QRect &rect)
{
    KoColor color(device->colorSpace());
    QPointF sum;
    qreal weight = 0;
    for (int y = rect.top(); y <= rect.bottom(); y++) {
        for (int x = rect.left(); x <= rect.right(); x++) {
            device->pixel(x, y, &color);
            const qreal a = color.opacityF();
            sum += QPointF(x + 0.5, y + 0.5) * a;
            weight += a;
        }
    }
    return weight > 0 ? sum / weight : QPointF(-1e6, -1e6);
}

/// how far \p p is from the line through \p points
qreal distanceToLine(const QVector<QPointF> &points, const QPointF &p)
{
    qreal nearest = 1e9;
    for (int i = 0; i + 1 < points.size(); i++) {
        const QPointF d = points[i + 1] - points[i];
        const qreal len2 = d.x() * d.x() + d.y() * d.y();
        const qreal u = len2 > 0 ? qBound(0.0, QPointF::dotProduct(p - points[i], d) / len2, 1.0) : 0;
        nearest = qMin(nearest, QLineF(p, points[i] + d * u).length());
    }
    return nearest;
}
}

void VPerspectiveTest::testPerspective()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());
    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("forceLodMode", false);
    transformConfig.writeEntry("scaleBrushSizes", false);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600, 400, cs, "perspective");
    KisDocument *doc = VWindowTest::addDocument(image);
    KisPaintLayerSP base = new KisPaintLayer(image, "base", OPACITY_OPAQUE_U8);
    image->addNode(base);
    image->waitForDone();

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
    const qreal ppp = image->xRes();

    QAction *freeAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformFree");
    QVERIFY(freeAction);

    // the corners of what the stroke transforms, and where the top right one goes
    const QRectF bounds(100, 100, 400, 200);
    const QPointF cornerFrom = bounds.topRight();
    const QPointF cornerTo(430, 150);
    QTransform cornerMap;
    QVERIFY(QTransform::quadToQuad(QPolygonF({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()}),
                                   QPolygonF({bounds.topLeft(), cornerTo, bounds.bottomRight(), bounds.bottomLeft()}), cornerMap));
    // a drag inside the box moves it: by whole pixels at every step, farther than the stroke is tall
    const QPointF moveFrom(300, 250);
    const QPointF moveBy(40, 60);
    // solid pixels: the bent stand-in (Fast) softens the faintest edge pixels
    auto inkOf = [] (KisPaintDeviceSP device, const QRect &rect) {
        const QImage pixels = device->convertToQImage(nullptr, rect);
        int ink = 0;
        for (int y = 0; y < pixels.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
            for (int x = 0; x < pixels.width(); x++) ink += qAlpha(line[x]) > 64;
        }
        return ink;
    };
    // dark pixels of the image: the stroke, not the red markers
    auto darkOf = [] (KisPaintDeviceSP device, const QRect &rect) {
        const QImage pixels = device->convertToQImage(nullptr, rect);
        int ink = 0;
        for (int y = 0; y < pixels.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
            for (int x = 0; x < pixels.width(); x++) ink += qAlpha(line[x]) > 64 && qRed(line[x]) < 128;
        }
        return ink;
    };
    const QVector<QPointF> markers = {QPointF(150, 200), QPointF(300, 200), QPointF(450, 200)};

    QStringList failures;
    for (const int row : {0, 1, 2, 3, 4, 5, 6}) {
        const bool fast = row == 1 || row == 2 || row == 4 || row == 6;
        // moves as quick as a hand's, by fractions of pixels: the stand-in shows while the renders catch up
        const bool rapid = row >= 5;
        const int stepCount = rapid ? 40 : 10;
        // Fast, then the preview switches to Accurate right after the drag
        const bool switchToAccurate = row == 2;
        // the box dragged by its inside: a move, no perspective
        const bool move = row >= 3;
        const char *name = row == 0 ? "Accurate" : row == 1 ? "Fast" : row == 2 ? "Fast>Acc" : row == 3 ? "Move" :
                           row == 4 ? "Move Fast" : row == 5 ? "Rapid" : "Rapid Fast";
        const QPointF dragFrom = move ? moveFrom : cornerFrom;
        const QPointF by = rapid ? moveBy + QPointF(0.3, 0.7) : moveBy;
        const QPointF dragTo = move ? moveFrom + by : cornerTo;
        const QTransform expected = move ? QTransform::fromTranslate(by.x(), by.y()) : cornerMap;
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        transformConfig.writeEntry("useOverlayPreviewStyle", fast);

        // layers of its own, so a row doesn't continue the last row's transform
        KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
        for (const QPointF &corner : {bounds.topLeft(), bounds.topRight() - QPointF(4, 0),
                                      bounds.bottomRight() - QPointF(4, 4), bounds.bottomLeft() - QPointF(0, 4)}) {
            raster->paintDevice()->fill(QRect(corner.toPoint(), QSize(4, 4)), KoColor(Qt::red, cs));
        }
        for (const QPointF &marker : markers) {
            raster->paintDevice()->fill(QRect((marker - QPointF(2, 2)).toPoint(), QSize(4, 4)), KoColor(Qt::red, cs));
        }
        image->addNode(raster);
        KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
        vector->setNodeProperty("freehandvector", true);
        image->addNode(vector);
        VStrokeData data;
        data.preset = preset;
        data.brushSize = 8;
        QVector<VStrokePrimitive> line;
        KisPaintInformation prev(markers.first(), 1.0);
        for (int x = 155; x <= 450; x += 5) {
            KisPaintInformation next(QPointF(x, 200), 1.0);
            line << VStrokePrimitive::line(prev, next);
            prev = next;
        }
        VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, ppp, data);
        vector->addShape(stroke);
        for (int i = 0; i < 5; i++) {
            stroke->waitForRendering();
            QTest::qWait(20);
        }
        QTest::qWait(300);
        image->waitForDone();
        const VCurveData original = stroke->curves();
        const int inkBefore = inkOf(vector->projection(), image->bounds());
        int leastInk = inkBefore;
        const int darkBefore = darkOf(image->projection(), image->bounds());
        int leastDark = darkBefore;
        const QTransform toImage = stroke->absoluteTransformation() * QTransform::fromScale(ppp, ppp);
        const int steps = doc->undoStack()->index();

        nodes->slotUiActivatedNode(vector);
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(raster), KisNodeSP(vector)});
        freeAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QVERIFY(transform);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
        QVERIFY(QMetaObject::invokeMethod(transform, "slotRestartTransform"));
        QTest::qWait(600);
        QToolButton *perspective = modeButton(transform, "perspectiveTransformButton");
        QVERIFY(perspective);
        perspective->click();
        QTest::qWait(800);

        // drag the top right corner in
        bool warped = false;
        const int rendersBefore = VStrokeRenderCache::totalRenders();
        const int staleBefore = VStrokeShape::staleShapePaints();
        const int restoresBefore = VMoveSession::restoreCount();
        QImage canvas(64, 64, QImage::Format_ARGB32_Premultiplied);
        // the tool picks the handle as the cursor comes over it
        QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent hover(&hoverEvent, dragFrom / ppp);
        transform->mouseMoveEvent(&hover);
        QTest::qWait(50);
        const bool modeRight = transform->property("transformMode").toInt() == 4;
        QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin(&pressEvent, dragFrom / ppp);
        transform->beginPrimaryAction(&begin);
        for (int step = 1; step <= stepCount; step++) {
            QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent moved(&moveEvent, (dragFrom + (dragTo - dragFrom) * step / qreal(stepCount)) / ppp);
            transform->continuePrimaryAction(&moved);
            QPainter painter(&canvas);
            transform->paint(painter, *view->canvasBase()->viewConverter());
            painter.end();
            warped |= stroke->isRendersPaused();
            QTest::qWait(rapid ? 8 : 60);
            if (move && (!rapid || step % 4 == 0)) {
                // the stroke stays on the canvas while it moves (no waitForDone: it ends the transform)
                QTest::qWait(rapid ? 4 : 100);
                leastInk = qMin(leastInk, inkOf(vector->projection(), image->bounds()));
                leastDark = qMin(leastDark, darkOf(image->projection(), image->bounds()));
            }
        }
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        const int rendersDuring = VStrokeRenderCache::totalRenders() - rendersBefore;
        KoPointerEvent end(&releaseEvent, dragTo / ppp);
        transform->endPrimaryAction(&end);
        const int staleDuring = VStrokeShape::staleShapePaints() - staleBefore;
        const int restoresDuring = VMoveSession::restoreCount() - restoresBefore;
        bool switchedRight = true;
        if (switchToAccurate) {
            // within the Fast pause: the stroke must render again at once
            QComboBox *modes = previewModes(transform);
            switchedRight = modes != nullptr;
            if (modes) modes->setCurrentIndex(0);
            QTest::qWait(100);
            switchedRight &= !stroke->isRendersPaused();
        }
        QTest::qWait(600);
        QMetaObject::invokeMethod(transform, "requestStrokeEnd");
        for (int i = 0; i < 40; i++) QTest::qWait(20);
        image->waitForDone();

        // the stroke's points went where the map sends them
        const VCurveData now = stroke->curves();
        qreal pointError = original.pointCount() == now.pointCount() ? 0 : 1e9;
        QVector<QPointF> line2;
        for (int i = 0; i < now.pointCount() && i < original.pointCount(); i++) {
            const QPointF at = toImage.map(now.positions[i]);
            line2 << at;
            pointError = qMax(pointError, QLineF(at, expected.map(toImage.map(original.positions[i]))).length());
        }
        // and where the raster layer's pixels went
        qreal rasterError = 0;
        for (const QPointF &marker : markers) {
            const QPointF target = expected.map(marker);
            const QPointF found = centroid(raster->paintDevice(), QRect(target.toPoint() - QPoint(8, 8), QSize(17, 17)));
            rasterError = qMax(rasterError, distanceToLine(line2, found));
        }
        const bool steps1 = doc->undoStack()->index() == steps + 1;
        doc->undoStack()->undo();
        QTest::qWait(300);
        image->waitForDone();
        const bool undone = stroke->curves() == original;
        // and it isn't left bending its rendering (Fast)
        QTest::qWait(400);
        const bool still = !stroke->isRendersPaused();

        const bool right = modeRight && pointError < 0.5 && rasterError < 1.5 && steps1 && undone && still &&
                           staleDuring == 0 && restoresDuring == 0 && switchedRight &&
                           // a move keeps the whole stroke on the canvas: the layer repaints it at its new
                           // place (VCurveShape::curvesChanged()). Fast bends the stroke while it moves and
                           // renders after, Accurate renders as it goes
                           (move ? leastInk >= inkBefore * 97 / 100 && leastDark >= darkBefore * 97 / 100 &&
                                       (fast ? rendersDuring == 0 : true)
                                 : fast ? warped && rendersDuring == 0 : rendersDuring > 0) &&
                           !stroke->isRendersPaused();
        std::fprintf(stderr, "%-9s perspective mode %d, point error %.3f px, raster error %.2f px, one step %d, undone %d, settled after undo %d, warped %d, %d renders while moving, %d stale paints, %d restores, switch right %d, ink %d of %d, canvas %d of %d: %s\n",
                     name, modeRight, pointError, rasterError, steps1, undone, still, warped, rendersDuring, staleDuring, restoresDuring, switchedRight, leastInk, inkBefore, leastDark, darkBefore, right ? "right" : "WRONG");
        if (!right) failures << name;
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));

    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

/**
 * Every transform mode follows the layer selection: when the selected layers
 * change, what is done so far is applied to the old layers as its own undo
 * step and the next drag moves the new ones.
 */
void VPerspectiveTest::testFollowsLayerSelection()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());
    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("forceLodMode", false);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600, 400, cs, "follow");
    KisDocument *doc = VWindowTest::addDocument(image);
    image->addNode(new KisPaintLayer(image, "base", OPACITY_OPAQUE_U8));
    image->waitForDone();

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
    const qreal ppp = image->xRes();

    QStringList failures;
    for (const bool fast : {false, true}) {
        // the first row warms the tool up: its first stroke in a new window misses the first drag
        for (const QString &rowName : {QString("warm-up"), QString("Free"), QString("Free vector"), QString("Perspective")}) {
            const QString modeName = rowName == "Perspective" ? "Perspective" : "Free";
            const bool vectorA = rowName == "Free vector";
            const QString name = rowName + (fast ? " Fast" : "");
            transformConfig.writeEntry("useOverlayPreviewStyle", fast);
            tools->switchToolRequested("KritaShape/KisToolBrush");
            QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
            image->waitForDone();

            // two layers, each a block in the middle; or a freehand layer of a brush stroke for a
            KisNodeSP a;
            if (vectorA) {
                KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), image, "a", OPACITY_OPAQUE_U8);
                layer->setNodeProperty("freehandvector", true);
                image->addNode(layer);
                VStrokeData data;
                data.preset = preset;
                data.brushSize = 8;
                QVector<VStrokePrimitive> line;
                KisPaintInformation prev(QPointF(205, 160), 1.0);
                for (int x = 215; x <= 395; x += 10) {
                    KisPaintInformation next(QPointF(x, x % 20 ? 240 : 160), 1.0);
                    line << VStrokePrimitive::line(prev, next);
                    prev = next;
                }
                VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, ppp, data);
                layer->addShape(stroke);
                for (int i = 0; i < 5; i++) {
                    stroke->waitForRendering();
                    QTest::qWait(20);
                }
                QTest::qWait(300);
                a = layer;
            } else {
                KisPaintLayerSP layer = new KisPaintLayer(image, "a", OPACITY_OPAQUE_U8);
                layer->paintDevice()->fill(QRect(200, 150, 200, 100), KoColor(Qt::red, cs));
                image->addNode(layer);
                a = layer;
            }
            KisPaintLayerSP b = new KisPaintLayer(image, "b", OPACITY_OPAQUE_U8);
            b->paintDevice()->fill(QRect(200, 150, 200, 100), KoColor(Qt::blue, cs));
            image->addNode(b);
            image->waitForDone();
            const int aTop = a->projection()->exactBounds().top();
            const int steps = doc->undoStack()->index();

            nodes->slotUiActivatedNode(a);
            nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(a)});
            // (a window made after an earlier one is gone lacks the transform mode actions)
            tools->switchToolRequested("KisToolTransform");
            QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
            KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
            QVERIFY(transform);
            QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
            QVERIFY(QMetaObject::invokeMethod(transform, "slotRestartTransform"));
            QTest::qWait(600);
            // the tool starts in Free Transform
            if (modeName == "Perspective") {
                QToolButton *perspective = modeButton(transform, "perspectiveTransformButton");
                QVERIFY(perspective);
                perspective->click();
            }
            // the stroke finds its layers
            QTest::qWait(600);

            // a drag inside the box moves it
            auto drag = [&] (const QPointF &from, const QPointF &by) {
                QImage canvas(64, 64, QImage::Format_ARGB32_Premultiplied);
                QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                KoPointerEvent hover(&hoverEvent, from / ppp);
                transform->mouseMoveEvent(&hover);
                {
                    QPainter painter(&canvas);
                    transform->paint(painter, *view->canvasBase()->viewConverter());
                }
                QTest::qWait(50);
                QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent begin(&pressEvent, from / ppp);
                transform->beginPrimaryAction(&begin);
                for (int step = 1; step <= 5; step++) {
                    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    KoPointerEvent moved(&moveEvent, (from + by * step / 5.0) / ppp);
                    transform->continuePrimaryAction(&moved);
                    QTest::qWait(40);
                }
                QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                KoPointerEvent end(&releaseEvent, (from + by) / ppp);
                transform->endPrimaryAction(&end);
                QTest::qWait(400);
            };
            // off the middle: Free Transform's rotation center is there
            drag(QPointF(250, 180), QPointF(0, 80));
            // the selection changes to b alone
            nodes->slotUiActivatedNode(b);
            nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(b)});
            QTest::qWait(800);
            const bool aApplied = doc->undoStack()->index() == steps + 1;
            drag(QPointF(250, 180), QPointF(0, -80));
            QMetaObject::invokeMethod(transform, "requestStrokeEnd");
            QTest::qWait(400);
            image->waitForDone();

            for (int i = 0; i < 10; i++) QTest::qWait(30);
            const int aMoved = a->projection()->exactBounds().top() - aTop;
            const int bTop = b->paintDevice()->exactBounds().top();
            const bool right = aApplied && qAbs(aMoved - 80) <= 1 && bTop == 70 &&
                               doc->undoStack()->index() == steps + 2;
            std::fprintf(stderr, "%-18s a applied on selection change %d, a moved %d (80), b top %d (70), %d steps (2): %s\n",
                         qPrintable(name), aApplied, aMoved, bTop, doc->undoStack()->index() - steps,
                         right ? "right" : "WRONG");
            if (!right && rowName != "warm-up") failures << name;
        }
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));

    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VPerspectiveTest)
#include "VPerspectiveTest.moc"
