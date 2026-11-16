/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <KConfigGroup>
#include <KSharedConfig>
#include <cstdio>

#include <KoCanvasController.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoPointerEvent.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisLodPreferences.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <commands_new/kis_saved_commands.h>
#include <kactioncollection.h>
#include <kis_canvas2.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_shape_layer.h>
#include <kis_stroke_job_strategy.h>
#include <kis_tool.h>
#include <kundo2stack.h>
#include <testui.h>

#include "VCurveShape.h"
#include "VStrokePrimitive.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * A Liquify stroke's undo history does not grow with its preview passes
 * (audit F1): the incremental preview keeps one transaction per paint layer
 * across its passes and the commands of the last pass only. A stroke of
 * many passes and one of few give history entries of the same size, and
 * undo and redo give the same pixels and curves as the stroke showed. On a
 * raster layer, on a freehand layer, and on both.
 */
class VLiquifyHistoryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testHistoryDoesNotGrowWithPasses();
};

namespace {

using VWindowTest::settle;

/// the commands the history entry on top of \p doc's undo stack replays
int historyEntrySize(KisDocument *doc)
{
    KUndo2QStack *stack = doc->undoStack();
    if (stack->index() <= 0) return -1;
    const KisSavedMacroCommand *macro = dynamic_cast<const KisSavedMacroCommand*>(stack->command(stack->index() - 1));
    if (!macro) return -1;
    QVector<KisStrokeJobData*> jobs;
    macro->getCommandExecutionJobs(&jobs, false, false);
    const int size = jobs.size();
    qDeleteAll(jobs);
    return size;
}

}

void VLiquifyHistoryTest::testHistoryDoesNotGrowWithPasses()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());
    {
        KConfigGroup move = KSharedConfig::openConfig()->group("LiquifyTool/Move");
        move.writeEntry("size", 60.0);
        move.writeEntry("amount", 0.5);
        move.writeEntry("sizeHasPressure", false);
        move.writeEntry("amountHasPressure", false);
        move.writeEntry("useWashMode", false);
    }
    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    // the incremental preview runs at full resolution (Accurate)
    transformConfig.writeEntry("forceLodMode", false);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600, 400, cs, "liquify history");
    image->setLodPreferences(KisLodPreferences());
    KisDocument *doc = VWindowTest::addDocument(image);

    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    raster->paintDevice()->fill(QRect(180, 100, 120, 160), KoColor(Qt::red, cs));
    raster->paintDevice()->fill(QRect(200, 180, 80, 10), KoColor(Qt::green, cs));
    image->addNode(raster);
    KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
    vector->setNodeProperty("freehandvector", true);
    image->addNode(vector);
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
    VStrokeShape *stroke = nullptr;
    {
        VStrokeData data;
        data.preset = preset;
        data.brushSize = 8;
        QVector<VStrokePrimitive> line;
        KisPaintInformation prev(QPointF(120, 200), 1.0);
        for (int x = 125; x <= 360; x += 5) {
            KisPaintInformation next(QPointF(x, 200), 1.0);
            line << VStrokePrimitive::line(prev, next);
            prev = next;
        }
        stroke = VStrokeShape::fromImagePixels(line, ppp, data);
        stroke->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
        vector->addShape(stroke);
        for (int i = 0; i < 10; i++) {
            stroke->waitForRendering();
            QTest::qWait(20);
        }
        image->waitForDone();
    }
    const VCurveData original = stroke->curves();
    auto rasterPixels = [&] () { return raster->paintDevice()->convertToQImage(0, QRect(0, 0, 600, 400)); };
    const QImage rasterBefore = rasterPixels();

    QAction *liquifyAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformLiquify");
    QVERIFY(liquifyAction);

    struct Case { bool withRaster; bool withVector; const char *name; };
    const QList<Case> cases = {
        {true, false, "raster"},
        {false, true, "freehand"},
        {true, true, "raster and freehand"},
    };
    // a failed check returns from this only: the window and document go in the end either way
    auto runCases = [&] () {
    for (const Case &c : cases) {
        QMap<int, int> sizes;
        for (const int steps : {4, 16}) {
            // a different tool in between, so the stroke doesn't continue the last one
            nodes->slotUiActivatedNode(raster);
            tools->switchToolRequested("KritaShape/KisToolBrush");
            QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
            image->waitForDone();
            KisNodeList selected;
            if (c.withRaster) selected << KisNodeSP(raster);
            if (c.withVector) selected << KisNodeSP(vector);
            nodes->slotUiActivatedNode(selected.last());
            nodes->slotSetSelectedNodes(selected);
            liquifyAction->trigger();
            QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
            KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
            QVERIFY(transform);
            settle(300);

            // a Move drag across the stroke and the green bar, one preview pass per step
            QImage canvasImage(64, 64, QImage::Format_ARGB32_Premultiplied);
            auto repaintTool = [&] () {
                QPainter painter(&canvasImage);
                transform->paint(painter, *view->canvasBase()->viewConverter());
            };
            const QPointF from(240, 150);
            const QPointF to(240, 260);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, from / ppp);
            transform->beginPrimaryAction(&begin);
            for (int step = 1; step <= steps; step++) {
                QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent moved(&move, (from + (to - from) * (qreal(step) / steps)) / ppp);
                transform->continuePrimaryAction(&moved);
                repaintTool();
                // the pass runs before the next move
                QTest::qWait(60);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, to / ppp);
            transform->endPrimaryAction(&end);
            repaintTool();
            settle(300);

            const int undoSteps = doc->undoStack()->count();
            transform->requestStrokeEnd();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            QCOMPARE(doc->undoStack()->count(), undoSteps + 1);
            const QImage rasterAfter = rasterPixels();
            const VCurveData curvesAfter = stroke->curves();
            QCOMPARE(rasterAfter != rasterBefore, c.withRaster);
            QCOMPARE(curvesAfter != original, c.withVector);

            const int size = historyEntrySize(doc);
            std::fprintf(stderr, "%s, %d moves: the history entry replays %d commands\n", c.name, steps, size);
            QVERIFY2(size > 0, c.name);
            sizes[steps] = size;

            // undo and redo: the stroke's result, and back
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            QVERIFY2(rasterPixels() == rasterBefore, c.name);
            QVERIFY2(stroke->curves() == original, c.name);
            doc->undoStack()->redo();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            QVERIFY2(rasterPixels() == rasterAfter, c.name);
            QVERIFY2(stroke->curves() == curvesAfter, c.name);
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            QVERIFY2(rasterPixels() == rasterBefore, c.name);
            QVERIFY2(stroke->curves() == original, c.name);
            if (QTest::currentTestFailed()) break;
        }
        if (QTest::currentTestFailed()) break;
        QVERIFY2(sizes[16] == sizes[4],
                 qPrintable(QString("%1: %2 commands after 16 moves, %3 after 4").arg(c.name).arg(sizes[16]).arg(sizes[4])));
    }
    };
    runCases();

    KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
    if (transform && tools->activeToolId() == "KisToolTransform") {
        transform->requestStrokeCancellation();
        image->waitForDone();
    }
    nodes->slotUiActivatedNode(raster);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VLiquifyHistoryTest)

#include "VLiquifyHistoryTest.moc"
