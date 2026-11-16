/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTimer>
#include <QtEndian>

#include <KoColorSpaceRegistry.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoCanvasBase.h>
#include <KoToolBase.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <actions/KisPasteActionFactories.h>
#include <actions/kis_selection_action_factories.h>
#include <commands/kis_set_global_selection_command.h>
#include <kis_canvas2.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_shape_controller.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <KoPathShape.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <kundo2stack.h>
#include <KisGlobalResourcesInterface.h>
#include <KoColorBackground.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>

#include <testui.h>

#include "VCurveShape.h"
#include "VPointEditTool.h"
#include "VFreehandLayer.h"
#include "VStrokePrimitive.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * Edit > Clear on vector layers from the tools that work on raster layers, and
 * paste of freehand curves on each kind of active layer, through the real
 * actions and tools.
 */
class VClearPasteTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testClearAndPaste();
    void testCopySelectionKeepsVector();
    void testCutAndClearAtTheSelection();
    void testCopySelectedPoints();
};

namespace {
using VWindowTest::encode;

/// a line of 40 points from (\p x, \p y) to the right, going down by \p slope per step
QByteArray curveSvg(qreal x, qreal y, qreal slope = 0)
{
    QVector<double> positions, radii;
    QString d;
    for (int i = 0; i < 40; i++) {
        const qreal px = x + i * 4;
        const qreal py = y + i * slope;
        positions << px << py;
        radii << 2;
        d += QString("%1%2,%3 ").arg(i ? "L" : "M").arg(px).arg(py);
    }
    const QString arrays =
        QString("<krita:a name=\"offsets\" type=\"i32\">%1</krita:a>").arg(encode(QVector<qint32>{0})) +
        QString("<krita:a name=\"cyclic\" type=\"u8\">%1</krita:a>").arg(encode(QVector<quint8>{0})) +
        QString("<krita:a name=\"position\" type=\"f64x2\">%1</krita:a>").arg(encode(positions)) +
        QString("<krita:a name=\"radius\" type=\"f64\">%1</krita:a>").arg(encode(radii));
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:krita=\"http://krita.org/namespaces/svg/krita\""
                   " width=\"400pt\" height=\"400pt\" viewBox=\"0 0 400 400\">"
                   "<path d=\"%1\" fill=\"none\" stroke=\"#000000\" stroke-width=\"4\""
                   " krita:type=\"vcurve\" krita:vcurve-version=\"1\">"
                   "<krita:curves version=\"1\" points=\"40\" curves=\"1\">%2</krita:curves></path></svg>")
        .arg(d, arrays).toUtf8();
}

using VWindowTest::paste;

void settle(KisImageSP image)
{
    QTest::qWait(150);
    image->waitForDone();
    QTest::qWait(150);
}

/// the shapes of \p layer in document coordinates, by their outlines: brush strokes' bounding rects depend on their rendering
QVector<QRectF> outlines(KisShapeLayer *layer)
{
    QVector<QRectF> rects;
    Q_FOREACH (KoShape *shape, layer->shapes()) rects << shape->absoluteOutlineRect();
    return rects;
}
}

void VClearPasteTest::testClearAndPaste()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KisImageSP image = new KisImage(0, 1500, 1500, KoColorSpaceRegistry::instance()->rgb8(), "clear");
    KisDocument *doc = VWindowTest::addDocument(image);
    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    image->addNode(raster);
    KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
    freehand->setNodeProperty("freehandvector", true);
    image->addNode(freehand);
    KisShapeLayerSP stock = new KisShapeLayer(doc->shapeController(), image, "stock", OPACITY_OPAQUE_U8);
    image->addNode(stock);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();

    auto activate = [&] (KisNodeSP node) {
        nodes->slotUiActivatedNode(node);
        nodes->slotImageRequestNodeReselection(node, {node});
        QTest::qWait(50);
    };
    auto fillRaster = [&] () {
        raster->paintDevice()->fill(QRect(10, 10, 100, 100), KoColor(Qt::red, raster->colorSpace()));
        raster->setDirty();
        image->waitForDone();
    };
    auto useTool = [&] (const QString &id) {
        tools->switchToolRequested("InteractionTool");
        QTest::qWait(100);
        settle(image);
        tools->switchToolRequested(id);
        QTest::qWait(150);
    };
    auto clear = [&] () {
        KisClearActionFactory().run(viewManager);
        settle(image);
    };

    activate(freehand);
    paste(curveSvg(40, 40), viewManager);
    paste(curveSvg(40, 100), viewManager);
    QCOMPARE(freehand->shapes().size(), 2);
    settle(image);

    // Del does on a vector layer what it does on a raster layer, in the tools that clear those
    const QStringList toolIds = {"KisToolTransform", "KritaTransform/KisToolMove", "KritaShape/KisToolLine",
                                 "KritaShape/KisToolRectangle", "KritaShape/KisToolBrush"};
    for (const QString &id : toolIds) {
        fillRaster();
        activate(raster);
        useTool(id);
        QCOMPARE(tools->activeToolId(), id);
        clear();
        QVERIFY2(raster->paintDevice()->exactBounds().isEmpty(), qPrintable("raster, " + id));

        activate(freehand);
        useTool(id);
        const int undoBefore = doc->undoStack()->index();
        clear();
        QVERIFY2(freehand->shapes().isEmpty(), qPrintable("vector, " + id));
        QCOMPARE(doc->undoStack()->index(), undoBefore + 1);
        doc->undoStack()->undo();
        settle(image);
        QVERIFY2(freehand->shapes().size() == 2, qPrintable("undo, " + id));
    }

    // with a selection around one stroke: only that one (testCutAndClearAtTheSelection has the ones it crosses)
    useTool("KisToolTransform");
    {
        KisShapeLayer *layer = freehand.data();
        QList<KoShape*> shapes = layer->shapes();
        std::sort(shapes.begin(), shapes.end(), [] (KoShape *a, KoShape *b) { return a->absoluteOutlineRect().y() < b->absoluteOutlineRect().y(); });
        KoShape *top = shapes.first();
        KoShape *bottom = shapes.last();
        const QRect topPixels = QTransform::fromScale(image->xRes(), image->yRes()).mapRect(top->absoluteOutlineRect()).toAlignedRect();
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(topPixels.adjusted(-3, -3, 3, 3));
        KisSetGlobalSelectionCommand(image, selection).redo();
        settle(image);
        clear();
        QCOMPARE(layer->shapes().size(), 1);
        QCOMPARE(layer->shapes().first(), bottom);
        doc->undoStack()->undo();
        settle(image);
        QCOMPARE(layer->shapes().size(), 2);
        KisSetGlobalSelectionCommand(image, KisSelectionSP()).redo();
        settle(image);
    }

    // a raster and a vector layer selected: both clear, in one undo step
    useTool("KritaShape/KisToolLine");
    {
        fillRaster();
        activate(freehand);
        nodes->slotImageRequestNodeReselection(freehand, {raster, freehand});
        QTest::qWait(50);
        settle(image);
        const int undoBefore = doc->undoStack()->index();
        clear();
        QVERIFY(raster->paintDevice()->exactBounds().isEmpty());
        QVERIFY(freehand->shapes().isEmpty());
        QCOMPARE(doc->undoStack()->index(), undoBefore + 1);
        doc->undoStack()->undo();
        settle(image);
        QCOMPARE(freehand->shapes().size(), 2);
        QVERIFY(!raster->paintDevice()->exactBounds().isEmpty());
    }

    // a locked layer stays
    activate(freehand);
    freehand->setUserLocked(true);
    useTool("KritaShape/KisToolLine");
    clear();
    QCOMPARE(freehand->shapes().size(), 2);
    freehand->setUserLocked(false);

    // paste: a freehand curve and a brush stroke, copied by the real action with the Select Shapes tool
    for (const qreal dpi : {72.0, 300.0}) {
        image->setResolution(dpi / 72.0, dpi / 72.0);
        settle(image);
        const qreal ppp = image->xRes();
        activate(freehand);
        useTool("InteractionTool");
        KoSelection *selection = view->canvasBase()->selectedShapesProxy()->selection();
        selection->deselectAll();
        QList<KoShape*> old = freehand->shapes();
        Q_FOREACH (KoShape *shape, old) {
            freehand->removeShape(shape);
            delete shape;
        }
        activate(freehand);
        paste(curveSvg(40, 100), viewManager);
        QCOMPARE(freehand->shapes().size(), 1);
        VCurveShape *curve = dynamic_cast<VCurveShape*>(freehand->shapes().first());
        QVERIFY(curve);

        KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"), KisGlobalResourcesInterface::instance());
        QVERIFY(preset);
        VStrokeData settings;
        settings.preset = preset;
        settings.brushSize = 12;
        VStrokeShape *brush = VStrokeShape::fromImagePixels({VStrokePrimitive::line(KisPaintInformation(QPointF(40, 200), 1.0),
                                                                                    KisPaintInformation(QPointF(300, 240), 1.0))},
                                                            ppp, settings);
        brush->setBackground(toQShared(new KoColorBackground(Qt::blue)));
        freehand->addShape(brush);
        settle(image);

        const QList<QList<KoShape*>> copies = {{curve}, {brush}, {curve, brush}};
        for (const QList<KoShape*> &copy : copies) {
            const QString name = QString("%1 dpi, ").arg(dpi) + (copy.size() > 1 ? "both" : dynamic_cast<VStrokeShape*>(copy.first()) ? "brush" : "curve");
            auto copyNow = [&] () {
                activate(freehand);
                useTool("InteractionTool");
                selection->deselectAll();
                for (KoShape *shape : copy) selection->select(shape);
                QTest::qWait(50);
                KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
                settle(image);
            };

            // on the freehand layer: copies in the same layer, each offset from the last
            copyNow();
            const QVector<QRectF> before = outlines(freehand.data());
            const int layersBefore = image->root()->childCount();
            KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
            settle(image);
            QVERIFY2(image->root()->childCount() == layersBefore, qPrintable(name));
            QVERIFY2(freehand->shapes().size() == before.size() + copy.size(), qPrintable(name));
            QRectF first;
            Q_FOREACH (KoShape *shape, freehand->shapes()) {
                if (!before.contains(shape->absoluteOutlineRect())) first |= shape->absoluteOutlineRect();
            }
            const QRectF original = KoShape::absoluteOutlineRect(copy);
            QVERIFY2(!first.isEmpty(), qPrintable(name + " pasted exactly on top of the original"));
            const qreal distance = QLineF(first.topLeft(), original.topLeft()).length();
            QVERIFY2(distance > 2.0, qPrintable(name + " pasted on top of itself"));

            const QVector<QRectF> afterFirst = outlines(freehand.data());
            KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
            settle(image);
            QRectF second;
            Q_FOREACH (KoShape *shape, freehand->shapes()) {
                if (!afterFirst.contains(shape->absoluteOutlineRect())) second |= shape->absoluteOutlineRect();
            }
            QVERIFY2(!second.isEmpty(), qPrintable(name + " second paste on top of the first"));
            doc->undoStack()->undo();
            settle(image);
            doc->undoStack()->undo();
            settle(image);
            QVERIFY2(freehand->shapes().size() == before.size(), qPrintable(name));

            // on a raster layer and a stock vector layer: a new freehand layer, whichever tool is active
            for (KisNodeSP active : {KisNodeSP(raster), KisNodeSP(stock)}) {
                for (const QString &id : {QString("InteractionTool"), QString("KritaShape/KisToolBrush")}) {
                    copyNow();
                    activate(active);
                    useTool(id);
                    const QString row = name + " on " + active->name() + ", " + id;
                    const int count = image->root()->childCount();
                    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
                    settle(image);
                    QVERIFY2(image->root()->childCount() == count + 1, qPrintable(row));
                    KisNodeSP added = image->root()->lastChild();
                    QVERIFY2(VFreehandLayer::isFreehand(added), qPrintable(row));
                    KisShapeLayer *addedLayer = dynamic_cast<KisShapeLayer*>(added.data());
                    QVERIFY2(addedLayer && addedLayer->shapes().size() == copy.size(), qPrintable(row));
                    doc->undoStack()->undo();
                    settle(image);
                    QVERIFY2(image->root()->childCount() == count, qPrintable(row));
                }
            }
        }
    }

    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

void VClearPasteTest::testCopySelectionKeepsVector()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);

    for (const qreal dpi : {72.0, 300.0}) {
        const qreal res = dpi / 72.0;
        const QString row = QString("%1 dpi").arg(dpi);
        KisImageSP image = new KisImage(0, 500 * res, 500 * res, KoColorSpaceRegistry::instance()->rgb8(), "copy");
        image->setResolution(res, res);
        KisDocument *doc = VWindowTest::addDocument(image);
        KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
        image->addNode(raster);
        KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
        freehand->setNodeProperty("freehandvector", true);
        image->addNode(freehand);
        KisGroupLayerSP group = new KisGroupLayer(image, "group", OPACITY_OPAQUE_U8);
        image->addNode(group);
        KisShapeLayerSP inGroup = new KisShapeLayer(doc->shapeController(), image, "in group", OPACITY_OPAQUE_U8);
        inGroup->setNodeProperty("freehandvector", true);
        image->addNode(inGroup, group);
        image->waitForDone();

        KisView *view = VWindowTest::openWindow(doc);
        KisMainWindow *window = view->mainWindow();
        KisViewManager *viewManager = window->viewManager();
        KisNodeManager *nodes = viewManager->nodeManager();

        // a line through the selection's right edge, and a line outside it
        for (KisShapeLayerSP layer : QList<KisShapeLayerSP>{freehand, inGroup}) {
            nodes->slotUiActivatedNode(layer);
            QTest::qWait(50);
            paste(curveSvg(100, 100), viewManager);
            paste(curveSvg(100, 300), viewManager);
            QVERIFY2(layer->shapes().size() == 2, qPrintable(row));
        }
        raster->paintDevice()->fill(QRect(QPoint(50 * res, 50 * res), QPoint(400 * res, 400 * res)),
                                    KoColor(Qt::red, raster->colorSpace()));
        KoToolManager::instance()->switchToolRequested("KritaShape/KisToolBrush");
        QTest::qWait(150);
        settle(image);

        // x from 50 to 180 pt: the line goes from 100 to 256
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(QPoint(50 * res, 50 * res), QPoint(180 * res, 150 * res)));
        KisSetGlobalSelectionCommand(image, selection).redo();
        settle(image);

        auto maxX = [] (KisShapeLayer *layer) {
            qreal result = -1;
            Q_FOREACH (KoShape *shape, layer->shapes()) {
                VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
                if (!curve) continue;
                Q_FOREACH (const QPointF &p, curve->curves().positions) {
                    result = qMax(result, curve->absoluteTransformation().map(p).x());
                }
            }
            return result;
        };
        const QVector<QRectF> originalOutlines = outlines(freehand.data());
        // the global selection is a child too, until the paste deselects it
        auto layerCount = [&] () {
            int result = 0;
            for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
                if (node->inherits("KisLayer")) result++;
            }
            return result;
        };

        // both layers selected: the raster layer stays raster, the vector layer vector and cut down
        nodes->slotUiActivatedNode(freehand);
        nodes->slotImageRequestNodeReselection(freehand, {raster, freehand});
        QTest::qWait(50);
        QVERIFY2(nodes->selectedNodes().size() == 2, qPrintable(row));
        KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
        settle(image);
        QCOMPARE(outlines(freehand.data()), originalOutlines);

        const int count = layerCount();
        KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
        settle(image);
        QVERIFY2(layerCount() == count + 2, qPrintable(row));
        KisShapeLayer *pasted = nullptr;
        KisPaintLayer *pastedRaster = nullptr;
        for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
            if (node.data() == raster.data() || node.data() == freehand.data() || node.data() == group.data()) continue;
            if (KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(node.data())) pasted = layer;
            if (KisPaintLayer *layer = dynamic_cast<KisPaintLayer*>(node.data())) pastedRaster = layer;
        }
        QVERIFY2(pasted, qPrintable(row + ": the vector layer was rasterized"));
        QVERIFY2(pastedRaster, qPrintable(row));
        QVERIFY2(VFreehandLayer::isFreehand(pasted), qPrintable(row));
        QVERIFY2(pasted->shapes().size() == 1, qPrintable(row + ": the line outside should stay behind"));
        QVERIFY2(qAbs(maxX(pasted) - 180) < 2, qPrintable(row + QString(": cut at %1").arg(maxX(pasted))));
        doc->undoStack()->undo();
        settle(image);
        QVERIFY2(layerCount() == count, qPrintable(row));
        QCOMPARE(outlines(freehand.data()), originalOutlines);

        // a group copied with its vector layer inside
        KisSetGlobalSelectionCommand(image, selection).redo();
        settle(image);
        nodes->slotUiActivatedNode(group);
        nodes->slotImageRequestNodeReselection(group, {group});
        QTest::qWait(50);
        KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
        settle(image);
        // a paste goes into the active group: outside it here, to find it
        nodes->slotUiActivatedNode(raster);
        QTest::qWait(50);
        KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
        settle(image);
        QVERIFY2(layerCount() == count + 1, qPrintable(row));
        KisNodeSP pastedGroup;
        for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
            if (node.data() != group.data() && node->inherits("KisGroupLayer")) pastedGroup = node;
        }
        QVERIFY2(pastedGroup && pastedGroup->childCount() == 1, qPrintable(row));
        KisShapeLayer *pastedInGroup = dynamic_cast<KisShapeLayer*>(pastedGroup->firstChild().data());
        QVERIFY2(pastedInGroup && pastedInGroup->shapes().size() == 1, qPrintable(row));
        QVERIFY2(qAbs(maxX(pastedInGroup) - 180) < 2, qPrintable(row));

        image->waitForDone();
        delete view;
        KisPart::instance()->removeMainWindow(window);
        delete window;
        KisPart::instance()->removeDocument(doc);
        QTest::qWait(50);
    }
}

void VClearPasteTest::testCutAndClearAtTheSelection()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);

    for (const qreal dpi : {72.0, 300.0}) {
        const qreal res = dpi / 72.0;
        const QString row = QString("%1 dpi").arg(dpi);
        KisImageSP image = new KisImage(0, 500 * res, 500 * res, KoColorSpaceRegistry::instance()->rgb8(), "cut");
        image->setResolution(res, res);
        KisDocument *doc = VWindowTest::addDocument(image);
        KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
        freehand->setNodeProperty("freehandvector", true);
        image->addNode(freehand);
        KisShapeLayerSP stock = new KisShapeLayer(doc->shapeController(), image, "stock", OPACITY_OPAQUE_U8);
        image->addNode(stock);
        image->waitForDone();

        KisView *view = VWindowTest::openWindow(doc);
        KisMainWindow *window = view->mainWindow();
        KisViewManager *viewManager = window->viewManager();
        KisNodeManager *nodes = viewManager->nodeManager();

        // through the selection's right edge, fully inside it, and outside it
        nodes->slotUiActivatedNode(freehand);
        QTest::qWait(50);
        paste(curveSvg(100, 100), viewManager);
        paste(curveSvg(45, 130), viewManager);
        paste(curveSvg(100, 300), viewManager);
        QVERIFY2(freehand->shapes().size() == 3, qPrintable(row));
        KoPathShape *path = new KoPathShape();
        path->moveTo(QPointF(100, 100));
        path->lineTo(QPointF(300, 100));
        path->lineTo(QPointF(300, 140));
        path->close();
        path->normalize();
        stock->addShape(path);
        KoToolManager::instance()->switchToolRequested("KritaShape/KisToolBrush");
        QTest::qWait(150);
        settle(image);

        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(QPoint(40 * res, 50 * res), QPoint(210 * res, 150 * res)));
        KisSetGlobalSelectionCommand(image, selection).redo();
        settle(image);

        auto ranges = [] (KisShapeLayer *layer) {
            QVector<QPair<qreal, qreal>> result;
            Q_FOREACH (KoShape *shape, layer->shapes()) {
                VCurveShape *curve = dynamic_cast<VCurveShape*>(shape);
                if (!curve) continue;
                QPolygonF points;
                Q_FOREACH (const QPointF &p, curve->curves().positions) points << curve->absoluteTransformation().map(p);
                result << qMakePair(points.boundingRect().left(), points.boundingRect().right());
            }
            std::sort(result.begin(), result.end());
            return result;
        };
        const QVector<QPair<qreal, qreal>> before = ranges(freehand.data());
        QCOMPARE(before.size(), 3);
        // what stays: the part past the edge, and the line outside
        auto expectCut = [&] (const char *what) {
            const QVector<QPair<qreal, qreal>> now = ranges(freehand.data());
            QVERIFY2(now.size() == 2, qPrintable(row + ": " + what + QString(": %1 shapes").arg(now.size())));
            QVERIFY2(qAbs(now[0].first - 100) < 2 && qAbs(now[0].second - 256) < 2, qPrintable(row + ": " + what + ": the line outside changed"));
            QVERIFY2(qAbs(now[1].first - 210) < 2 && qAbs(now[1].second - 256) < 2, qPrintable(row + ": " + what + QString(": cut at %1").arg(now[1].first)));
        };

        nodes->slotUiActivatedNode(freehand);
        nodes->slotSetSelectedNodes({freehand});
        QTest::qWait(50);

        // Del: one undo step
        {
            const int steps = doc->undoStack()->index();
            KisClearActionFactory().run(viewManager);
            settle(image);
            expectCut("Del");
            QCOMPARE(doc->undoStack()->index(), steps + 1);
            doc->undoStack()->undo();
            settle(image);
            QCOMPARE(ranges(freehand.data()), before);
        }

        // Cut: one undo step, and it pastes what it took as vectors
        {
            const int steps = doc->undoStack()->index();
            KisCutCopyActionFactory().run(KisCutCopyActionFactory::CutClip, viewManager);
            settle(image);
            expectCut("Cut");
            QCOMPARE(doc->undoStack()->index(), steps + 1);

            KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
            settle(image);
            KisShapeLayer *pasted = nullptr;
            for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
                KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(node.data());
                if (layer && layer != freehand.data() && layer != stock.data()) pasted = layer;
            }
            QVERIFY2(pasted, qPrintable(row + ": nothing pasted as vectors"));
            const QVector<QPair<qreal, qreal>> taken = ranges(pasted);
            QVERIFY2(taken.size() == 2, qPrintable(row + QString(": pasted %1 shapes").arg(taken.size())));
            QVERIFY2(qAbs(taken[1].second - 210) < 2, qPrintable(row + QString(": pasted up to %1").arg(taken[1].second)));

            doc->undoStack()->undo();
            settle(image);
            doc->undoStack()->undo();
            settle(image);
            KisSetGlobalSelectionCommand(image, selection).redo();
            settle(image);
            QCOMPARE(ranges(freehand.data()), before);
        }

        // a stock layer stays as it was with Cut, and with Del unless fully covered
        {
            nodes->slotUiActivatedNode(stock);
            nodes->slotSetSelectedNodes({stock});
            QTest::qWait(50);
            KisCutCopyActionFactory().run(KisCutCopyActionFactory::CutClip, viewManager);
            settle(image);
            QVERIFY2(stock->shapes().size() == 1, qPrintable(row));
            QVERIFY2(qAbs(stock->shapes().first()->absoluteOutlineRect().right() - 300) < 1, qPrintable(row));
            KisClearActionFactory().run(viewManager);
            settle(image);
            QVERIFY2(stock->shapes().size() == 1, qPrintable(row));
            // the clipboard mustn't hold pixels past the end of the test
            QApplication::clipboard()->setMimeData(new QMimeData());
        }

        image->waitForDone();
        delete view;
        KisPart::instance()->removeMainWindow(window);
        delete window;
        KisPart::instance()->removeDocument(doc);
        QTest::qWait(50);
    }
}

void VClearPasteTest::testCopySelectedPoints()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KisImageSP image = new KisImage(0, 500, 500, KoColorSpaceRegistry::instance()->rgb8(), "points");
    KisDocument *doc = VWindowTest::addDocument(image);
    KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
    freehand->setNodeProperty("freehandvector", true);
    image->addNode(freehand);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();
    KoCanvasBase *canvas = viewManager->canvasBase();
    nodes->slotUiActivatedNode(freehand);
    nodes->slotImageRequestNodeReselection(freehand, {freehand});
    QTest::qWait(50);

    paste(curveSvg(100, 100), viewManager);
    QCOMPARE(freehand->shapes().size(), 1);
    VCurveShape *line = dynamic_cast<VCurveShape*>(freehand->shapes().first());
    QVERIFY(line);
    const VCurveData original = line->curves();
    // something else on the clipboard, as in the report
    QApplication::clipboard()->setMimeData(new QMimeData());

    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(150);
    VPointEditTool *tool = dynamic_cast<VPointEditTool*>(tools->toolById(canvas, "VPointEditTool"));
    QVERIFY(tool);
    KoSelection *selection = canvas->selectedShapesProxy()->selection();
    selection->deselectAll();
    selection->select(line);
    QVERIFY(!tool->hasSelection());
    // points 10 to 19: x from 140 to 176
    QVector<int> picked;
    for (int i = 10; i < 20; i++) picked << i;
    tool->selectPoints(line, picked);
    QVERIFY(tool->hasSelection());

    auto sortedByX = [&] () {
        QList<VCurveShape*> shapes;
        Q_FOREACH (KoShape *s, freehand->shapes()) shapes << dynamic_cast<VCurveShape*>(s);
        std::sort(shapes.begin(), shapes.end(), [] (VCurveShape *a, VCurveShape *b) {
            return a->absoluteOutlineRect().width() > b->absoluteOutlineRect().width();
        });
        return shapes;
    };
    auto xRange = [] (VCurveShape *shape) {
        qreal lo = 1e9, hi = -1e9;
        Q_FOREACH (const QPointF &p, shape->curves().positions) {
            const qreal x = shape->absoluteTransformation().map(p).x();
            lo = qMin(lo, x);
            hi = qMax(hi, x);
        }
        return qMakePair(lo, hi);
    };

    // Copy and Paste: the stretch, as a stroke of its own on the same layer
    KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
    settle(image);
    QCOMPARE(line->curves(), original);
    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), 2);
    QList<VCurveShape*> shapes = sortedByX();
    QCOMPARE(shapes[0], line);
    QCOMPARE(shapes[1]->curves().pointCount(), 10);
    QCOMPARE(shapes[1]->shapeId(), line->shapeId());
    const auto pastedRange = xRange(shapes[1]);
    // a paste lands slightly offset
    QVERIFY2(qAbs((pastedRange.second - pastedRange.first) - 36) < 0.01, qPrintable(QString::number(pastedRange.second - pastedRange.first)));
    QVERIFY2(pastedRange.first > 130 && pastedRange.first < 160, qPrintable(QString::number(pastedRange.first)));
    doc->undoStack()->undo();
    settle(image);
    QCOMPARE(freehand->shapes().size(), 1);

    // Cut: the line keeps both sides, and a paste brings the stretch back
    tools->switchToolRequested("InteractionTool");
    QTest::qWait(100);
    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(150);
    selection->deselectAll();
    selection->select(line);
    tool->selectPoints(line, picked);
    KisCutCopyActionFactory().run(KisCutCopyActionFactory::CutClip, viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), 1);
    QCOMPARE(line->curves().curveCount(), 2);
    QCOMPARE(line->curves().pointCount(), 11 + 21);
    QVERIFY(!tool->hasSelection());
    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), 2);
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    settle(image);
    QCOMPARE(freehand->shapes().size(), 1);
    QCOMPARE(line->curves(), original);

    // Del: the stretch goes; every point: the stroke goes
    selection->deselectAll();
    selection->select(line);
    tool->selectPoints(line, picked);
    KisClearActionFactory().run(viewManager);
    settle(image);
    QCOMPARE(line->curves().curveCount(), 2);
    doc->undoStack()->undo();
    settle(image);
    QCOMPARE(line->curves(), original);
    QVector<int> all;
    for (int i = 0; i < original.pointCount(); i++) all << i;
    tool->selectPoints(line, all);
    KisClearActionFactory().run(viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), 0);
    doc->undoStack()->undo();
    settle(image);
    QCOMPARE(freehand->shapes().size(), 1);

    // no points: Copy takes the selected strokes whole
    line = dynamic_cast<VCurveShape*>(freehand->shapes().first());
    QApplication::clipboard()->setMimeData(new QMimeData());
    tools->switchToolRequested("InteractionTool");
    QTest::qWait(100);
    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(150);
    selection->deselectAll();
    selection->select(line);
    QVERIFY(!tool->hasSelection());
    KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
    settle(image);
    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), 2);
    Q_FOREACH (KoShape *s, freehand->shapes()) {
        QCOMPARE(dynamic_cast<VCurveShape*>(s)->curves().pointCount(), original.pointCount());
    }

    // a brush stroke: the copy is a brush stroke with the same brush
    KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"), KisGlobalResourcesInterface::instance());
    QVERIFY(preset);
    VStrokeData settings;
    settings.preset = preset;
    settings.brushSize = 12;
    QVector<VStrokePrimitive> primitives;
    for (int i = 0; i < 10; i++) {
        primitives << VStrokePrimitive::line(KisPaintInformation(QPointF(40 + i * 20, 300), 1.0),
                                             KisPaintInformation(QPointF(60 + i * 20, 300), 1.0));
    }
    VStrokeShape *brush = VStrokeShape::fromImagePixels(primitives, image->xRes(), settings);
    brush->setBackground(toQShared(new KoColorBackground(Qt::blue)));
    freehand->addShape(brush);
    settle(image);
    const int brushPoints = brush->curves().pointCount();
    QVERIFY2(brushPoints >= 6, qPrintable(QString::number(brushPoints)));
    tools->switchToolRequested("InteractionTool");
    QTest::qWait(100);
    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(150);
    selection->deselectAll();
    selection->select(brush);
    tool->selectPoints(brush, {1, 2, 3});
    const int countBefore = freehand->shapes().size();
    KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
    settle(image);
    KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
    settle(image);
    QCOMPARE(freehand->shapes().size(), countBefore + 1);
    VStrokeShape *pastedBrush = nullptr;
    Q_FOREACH (KoShape *s, freehand->shapes()) {
        VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(s);
        if (stroke && stroke != brush) pastedBrush = stroke;
    }
    QVERIFY(pastedBrush);
    QCOMPARE(pastedBrush->curves().pointCount(), 3);
    QCOMPARE(pastedBrush->color(), QColor(Qt::blue));
    QCOMPARE(brush->curves().pointCount(), brushPoints);

    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VClearPasteTest)

#include "VClearPasteTest.moc"
