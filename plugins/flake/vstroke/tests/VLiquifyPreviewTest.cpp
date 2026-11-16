/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QGridLayout>
#include <QComboBox>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QStandardItemModel>
#include <QTimer>
#include <QWidget>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QDialogButtonBox>
#include <QPushButton>
#include <cmath>
#include <cstdio>
#include <functional>

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
#include "VStrokePrimitive.h"
#include "VStrokeRenderCache.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * What the canvas shows while a Liquify stroke on a freehand layer is
 * still open: after each preview update and the strokes' re-rendering, the
 * composited image has the brush stroke where its curves moved, with
 * Accurate (LOD 0) and Instant Preview (LOD 2), on a freehand layer alone
 * and together with a raster layer. Then in-tool undo, cancel and apply.
 */
class VLiquifyPreviewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPreview();
};

namespace {

/// the number of mostly blue pixels of \p device in \p rect (in the device's current level of detail)
int bluePixels(KisPaintDeviceSP device, const QRect &rect)
{
    const QImage img = device->convertToQImage(0, rect);
    int count = 0;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) {
            count += qAlpha(line[x]) > 0 && qBlue(line[x]) > 2 * qMax(qRed(line[x]), qGreen(line[x])) + 20;
        }
    }
    return count;
}

using VWindowTest::settle;

/// the transform tool's preview mode combo
QComboBox *previewModes(KisTool *transform)
{
    Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
        if (!widget) continue;
        if (QComboBox *combo = widget->findChild<QComboBox*>("cmbPreviewMode")) return combo;
    }
    return nullptr;
}

bool modeEnabled(QComboBox *modes, int index)
{
    QStandardItemModel *model = qobject_cast<QStandardItemModel*>(modes->model());
    return model && model->item(index)->isEnabled();
}

}

void VLiquifyPreviewTest::testPreview()
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
    KSharedConfig::openConfig()->group("LiquifyTool").deleteEntry("showEffectReach");

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600, 400, cs, "liquify preview");
    KisDocument *doc = VWindowTest::addDocument(image);

    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    // under the drag, above the brush stroke
    raster->paintDevice()->fill(QRect(180, 100, 120, 60), KoColor(Qt::red, cs));
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

    // a blue brush stroke along y = 200, densely sampled so Liquify can bend it
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
    QVERIFY(bluePixels(image->projection(), QRect(230, 195, 20, 10)) > 50);

    QAction *liquifyAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformLiquify");
    QVERIFY(liquifyAction);

    struct Case { int lod; bool withRaster; const char *name; };
    const QList<Case> cases = {
        {0, false, "freehand, LOD 0"},
        {2, false, "freehand, LOD 2"},
        {0, true, "freehand and raster, LOD 0"},
        {2, true, "freehand and raster, LOD 2"},
    };
    for (const Case &c : cases) {
    const int lod = c.lod;
    const bool withRaster = c.withRaster;
    std::fprintf(stderr, "--- %s\n", c.name);
    auto rasterPixels = [&] () { return raster->paintDevice()->convertToQImage(0, QRect(0, 0, 600, 400)); };
    const QImage rasterBefore = rasterPixels();
    transformConfig.writeEntry("forceLodMode", lod > 0);
    image->setLodPreferences(lod > 0 ? KisLodPreferences(lod) : KisLodPreferences());
    // a different tool in between, so the stroke doesn't continue the last one
    nodes->slotUiActivatedNode(raster);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
    image->waitForDone();
    nodes->slotUiActivatedNode(vector);
    nodes->slotSetSelectedNodes(withRaster ? KisNodeList{KisNodeSP(raster), KisNodeSP(vector)} : KisNodeList{KisNodeSP(vector)});
    liquifyAction->trigger();
    QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
    KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
    QVERIFY(transform);
    QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
    settle(300);
    // Accurate and Fast apply, with Instant Preview shown as Accurate; the
    // saved choice stays, also through a switch to Fast and back
    {
        QComboBox *modes = previewModes(transform);
        QVERIFY(modes);
        QCOMPARE(modes->currentIndex(), 0);
        QVERIFY(!modeEnabled(modes, 1));
        QVERIFY(modeEnabled(modes, 2));
        QCOMPARE(transformConfig.readEntry("forceLodMode", false), lod > 0);
        QCOMPARE(transformConfig.readEntry("useOverlayPreviewStyle", true), false);
        modes->setCurrentIndex(2);
        settle(300);
        modes = previewModes(transform);
        QCOMPARE(modes->currentIndex(), 2);
        QVERIFY(!modeEnabled(modes, 1));
        QCOMPARE(transformConfig.readEntry("useOverlayPreviewStyle", false), true);
        QCOMPARE(transformConfig.readEntry("forceLodMode", false), lod > 0);
        modes->setCurrentIndex(0);
        settle(300);
        modes = previewModes(transform);
        QCOMPARE(modes->currentIndex(), 0);
        QCOMPARE(transformConfig.readEntry("useOverlayPreviewStyle", true), false);
        QCOMPARE(transformConfig.readEntry("forceLodMode", false), lod > 0);
    }

    QImage canvasImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    auto repaintTool = [&] () {
        // Liquify sends its changes to the image when the canvas repaints the tool
        QPainter painter(&canvasImage);
        transform->paint(painter, *view->canvasBase()->viewConverter());
    };
    // the image px of the curve point that moved the most, and how far
    auto deepest = [&] (QPointF *from) {
        const VCurveData now = stroke->curves();
        const QTransform toImage = stroke->absoluteTransformation() * QTransform::fromScale(ppp, ppp);
        qreal best = -1;
        QPointF result;
        for (int i = 0; i < now.positions.size() && i < original.positions.size(); i++) {
            const QPointF a = toImage.map(original.positions[i]);
            const QPointF b = toImage.map(now.positions[i]);
            if (QLineF(a, b).length() > best) {
                best = QLineF(a, b).length();
                result = b;
                *from = a;
            }
        }
        return result;
    };
    // the composited image shows the stroke where its curves are now, and not
    // where they were. Liquify over a freehand layer runs at full resolution
    // even with Instant Preview chosen (the layer repaints at full resolution
    // only: its reduced copy, which the OpenGL canvas shows during a stroke
    // with a level of detail, kept the stroke where it was)
    auto checkShown = [&] (const char *when) {
        for (int i = 0; i < 10; i++) {
            stroke->waitForRendering();
            QTest::qWait(30);
        }
        settle(300);
        QPointF from;
        const QPointF to = deepest(&from);
        QVERIFY2(QLineF(from, to).length() > 20, qPrintable(QString("%1: moved %2 px").arg(when).arg(QLineF(from, to).length())));
        const QRect movedArea((to - QPointF(4, 4)).toPoint(), QSize(8, 8));
        const QRect oldArea((from - QPointF(2, 2)).toPoint(), QSize(4, 4));
        const int moved = bluePixels(image->projection(), movedArea);
        const int old = bluePixels(image->projection(), oldArea);
        std::fprintf(stderr, "%s, %s: moved area %d blue px, old area %d\n", when, c.name, moved, old);
        QVERIFY2(moved > 0, qPrintable(QString("%1, %2: nothing shown where the stroke moved").arg(when, c.name)));
        QVERIFY2(old == 0, qPrintable(QString("%1, %2: still shown where the stroke was").arg(when, c.name)));
    };

    // a Move drag down across the stroke, in steps, each one preview update
    const QPointF dragFrom(240, 150);
    const QPointF dragTo(240, 260);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent begin(&press, dragFrom / ppp);
    transform->beginPrimaryAction(&begin);
    const int rendersBefore = VStrokeRenderCache::totalRenders();
    for (int step = 1; step <= 10; step++) {
        const QPointF pos = dragFrom + (dragTo - dragFrom) * (step / 10.0);
        QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent moved(&move, pos / ppp);
        transform->continuePrimaryAction(&moved);
        repaintTool();
        QTest::qWait(40);
        if (step == 4) {
            // while it moves, the stroke renders as it does under the point
            // edit tool, so nothing snaps when it stops
            QPointF from;
            const QPointF to = deepest(&from);
            QVERIFY2(QLineF(from, to).length() > 10, c.name);
            const QRect movedArea((to - QPointF(4, 4)).toPoint(), QSize(8, 8));
            const QRect oldArea((from - QPointF(2, 2)).toPoint(), QSize(4, 4));
            QTRY_VERIFY_WITH_TIMEOUT(bluePixels(image->projection(), movedArea) > 0 &&
                                     bluePixels(image->projection(), oldArea) == 0, 200);
            QVERIFY2(VStrokeRenderCache::totalRenders() > rendersBefore, c.name);
        }
        if (step == 5) {
            checkShown("halfway through the drag");
            // the raster layer moves with it
            if (withRaster) QVERIFY2(rasterPixels() != rasterBefore, c.name);
        }
        if (QTest::currentTestFailed()) break;
    }
    if (!QTest::currentTestFailed()) {
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end(&release, dragTo / ppp);
        transform->endPrimaryAction(&end);
        repaintTool();
        checkShown("after the drag");
        // the drag can be applied or reset, with or without the raster grid moving
        QDialogButtonBox *buttons = nullptr;
        Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
            if (!buttons && widget) buttons = widget->findChild<QDialogButtonBox*>("buttonBox");
        }
        QVERIFY(buttons);
        QTRY_VERIFY2(buttons->button(QDialogButtonBox::Apply)->isEnabled(), c.name);
    }
    if (!QTest::currentTestFailed()) {
        // in-tool undo takes the drag back; the old place shows the stroke
        // again, with its rendering from before the drag: no render
        for (int i = 0; i < 10; i++) {
            stroke->waitForRendering();
            QTest::qWait(30);
        }
        const int rendersBeforeUndo = VStrokeRenderCache::totalRenders();
        transform->requestUndoDuringStroke();
        repaintTool();
        settle(300);
        QTRY_VERIFY(stroke->curves() == original);
        for (int i = 0; i < 10; i++) {
            stroke->waitForRendering();
            QTest::qWait(30);
        }
        settle(300);
        QTRY_VERIFY(bluePixels(image->projection(), QRect(230, 195, 20, 10)) > 0);
        QTRY_VERIFY(rasterPixels() == rasterBefore);
        QVERIFY2(VStrokeRenderCache::totalRenders() == rendersBeforeUndo, c.name);

        // another drag, applied: the stroke stays moved, and one undo takes it back
        QMouseEvent press2(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin2(&press2, dragFrom / ppp);
        transform->beginPrimaryAction(&begin2);
        for (int step = 1; step <= 10; step++) {
            QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent moved(&move, (dragFrom + (dragTo - dragFrom) * (step / 10.0)) / ppp);
            transform->continuePrimaryAction(&moved);
            repaintTool();
            QTest::qWait(20);
        }
        QMouseEvent release2(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end2(&release2, dragTo / ppp);
        transform->endPrimaryAction(&end2);
        repaintTool();
        settle(300);
        const VCurveData preview = stroke->curves();
        QVERIFY(preview != original);
        const QImage rasterPreview = rasterPixels();
        QCOMPARE(rasterPreview != rasterBefore, withRaster);
        const int undoSteps = doc->undoStack()->count();
        transform->requestStrokeEnd();
        image->waitForDone();
        QTest::qWait(50);
        image->waitForDone();
        QVERIFY(stroke->curves() == preview);
        QVERIFY(rasterPixels() == rasterPreview);
        QCOMPARE(doc->undoStack()->count(), undoSteps + 1);
        checkShown("after apply");
        doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(stroke->curves() == original);
        QVERIFY(rasterPixels() == rasterBefore);
        doc->undoStack()->redo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(stroke->curves() == preview);
        QVERIFY(rasterPixels() == rasterPreview);
        doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(rasterPixels() == rasterBefore);

        // a drag, then cancel: back to the original
        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        nodes->slotUiActivatedNode(vector);
        nodes->slotSetSelectedNodes(withRaster ? KisNodeList{KisNodeSP(raster), KisNodeSP(vector)} : KisNodeList{KisNodeSP(vector)});
        liquifyAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        settle(300);
        QMouseEvent press3(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin3(&press3, dragFrom / ppp);
        transform->beginPrimaryAction(&begin3);
        for (int step = 1; step <= 5; step++) {
            QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent moved(&move, (dragFrom + (dragTo - dragFrom) * (step / 5.0)) / ppp);
            transform->continuePrimaryAction(&moved);
            repaintTool();
            QTest::qWait(20);
        }
        QMouseEvent release3(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end3(&release3, dragTo / ppp);
        transform->endPrimaryAction(&end3);
        repaintTool();
        settle(300);
        QVERIFY(stroke->curves() != original);
        QCOMPARE(rasterPixels() != rasterBefore, withRaster);
        transform->requestStrokeCancellation();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(stroke->curves() == original);
        QVERIFY(rasterPixels() == rasterBefore);
    }
    if (QTest::currentTestFailed()) break;
    }

    if (!QTest::currentTestFailed()) {
        // Liquify on the raster layer alone: every preview mode, the saved one shown
        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(raster)});
        liquifyAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        settle(300);
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QComboBox *modes = previewModes(transform);
        QVERIFY(modes);
        QVERIFY(modeEnabled(modes, 1));
        QVERIFY(modeEnabled(modes, 2));
        // the last case saved Accurate with Instant Preview
        QCOMPARE(modes->currentIndex(), 1);
        transform->requestStrokeCancellation();
        image->waitForDone();

        // a stroke starts with the brush size the GUI thread last saved: the
        // config of a stroke's worker thread can still have an older one
        for (const qreal size : {77.0, 133.0, 211.0}) {
            tools->switchToolRequested("KritaShape/KisToolBrush");
            QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
            image->waitForDone();
            KSharedConfig::openConfig()->group("LiquifyTool/Move").writeEntry("size", size);
            liquifyAction->trigger();
            QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
            settle(300);
            QWidget *slider = nullptr;
            Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
                if (!slider && widget) slider = widget->findChild<QWidget*>("liquifySizeSlider");
            }
            QVERIFY(slider);
            QCOMPARE(slider->property("value").toDouble(), size);
            transform->requestStrokeCancellation();
            image->waitForDone();
        }

        // the brush size stays across undo, tools and layers, however it was changed
        auto startLiquify = [&] (KisNodeSP node) {
            tools->switchToolRequested("KritaShape/KisToolBrush");
            QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
            image->waitForDone();
            nodes->slotUiActivatedNode(node);
            nodes->slotSetSelectedNodes(KisNodeList{node});
            liquifyAction->trigger();
            QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
            settle(300);
        };
        auto sizeSlider = [&] () {
            QWidget *slider = nullptr;
            Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
                if (!slider && widget) slider = widget->findChild<QWidget*>("liquifySizeSlider");
            }
            return slider;
        };
        auto shownSize = [&] () { return sizeSlider()->property("value").toDouble(); };
        // as the slider does when the user finishes with it
        auto setSize = [&] (qreal size) {
            QVERIFY(QMetaObject::invokeMethod(sizeSlider(), "setValue", Q_ARG(double, size)));
            QVERIFY(QMetaObject::invokeMethod(sizeSlider(), "editingFinished"));
            settle(100);
        };
        auto drag = [&] (const QPointF &from, const QPointF &to, std::function<void()> midway = {}) {
            QImage canvas(64, 64, QImage::Format_ARGB32_Premultiplied);
            QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&pressEvent, from / ppp);
            transform->beginPrimaryAction(&begin);
            for (int step = 1; step <= 5; step++) {
                QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent moved(&moveEvent, (from + (to - from) * (step / 5.0)) / ppp);
                transform->continuePrimaryAction(&moved);
                QPainter painter(&canvas);
                transform->paint(painter, *view->canvasBase()->viewConverter());
                painter.end();
                QTest::qWait(20);
                if (step == 3 && midway) midway();
            }
            QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&releaseEvent, to / ppp);
            transform->endPrimaryAction(&end);
            QPainter painter(&canvas);
            transform->paint(painter, *view->canvasBase()->viewConverter());
            painter.end();
            settle(300);
        };
        const VCurveData unmoved = stroke->curves();

        // in-tool undo takes back the drag, and keeps a size changed before it or since
        startLiquify(vector);
        setSize(140);
        drag(QPointF(240, 150), QPointF(240, 260));
        QVERIFY(stroke->curves() != unmoved);
        setSize(170);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotIncreaseBrushSize"));
        const qreal changed = shownSize();
        QVERIFY(changed > 170);
        // the size wasn't an undo step of its own: this undo takes the drag back
        transform->requestUndoDuringStroke();
        settle(300);
        QTRY_VERIFY(stroke->curves() == unmoved);
        QCOMPARE(shownSize(), changed);
        transform->requestStrokeCancellation();
        image->waitForDone();

        // ... and a new stroke has it, after another tool
        startLiquify(vector);
        QCOMPARE(shownSize(), changed);
        // the size from a shortcut alone is saved too
        QVERIFY(QMetaObject::invokeMethod(transform, "slotDecreaseBrushSize"));
        const qreal smaller = shownSize();
        QVERIFY(smaller < changed);
        transform->requestStrokeCancellation();
        image->waitForDone();
        startLiquify(vector);
        QCOMPARE(shownSize(), smaller);

        // a stroke continuing an applied one takes the newest size, not the size it was applied with
        setSize(150);
        drag(QPointF(240, 150), QPointF(240, 260));
        transform->requestStrokeEnd();
        image->waitForDone();
        QTest::qWait(50);
        image->waitForDone();
        const VCurveData applied = stroke->curves();
        QVERIFY(applied != unmoved);
        startLiquify(raster);
        QCOMPARE(shownSize(), 150.0);
        setSize(190);
        transform->requestStrokeCancellation();
        image->waitForDone();
        startLiquify(vector);
        QCOMPARE(shownSize(), 190.0);
        // resetting to the continued state keeps it too, as does the second reset, a fresh stroke over the applied drag
        setSize(120);
        drag(QPointF(300, 150), QPointF(300, 260));
        QVERIFY(stroke->curves() != applied);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotCancelTransform"));
        settle(300);
        QTRY_VERIFY(stroke->curves() == applied);
        QCOMPARE(shownSize(), 120.0);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotCancelTransform"));
        settle(300);
        QTRY_VERIFY(stroke->curves() == applied);
        QCOMPARE(shownSize(), 120.0);
        transform->requestStrokeCancellation();
        image->waitForDone();

        // the vector layer forces Move: with Move's size, not the size of the mode saved last
        {
            KSharedConfig::openConfig()->group("LiquifyTool").writeEntry("mode", 1); // KisLiquifyProperties::SCALE
            KSharedConfig::openConfig()->group("LiquifyTool/Scale").writeEntry("size", 222.0);
            startLiquify(vector);
            QCOMPARE(shownSize(), 120.0);
            transform->requestStrokeCancellation();
            image->waitForDone();
        }

        // Liquify follows the layer selection: what is done so far stays on the
        // old layers as its own undo step, and the next drag needs no extra click
        {
            KUndo2Stack *undo = doc->undoStack();
            auto rasterNow = [&] () { return raster->paintDevice()->convertToQImage(0, QRect(0, 0, 600, 400)); };
            auto selectOnly = [&] (const KisNodeList &selection) {
                // no waitForDone() while the stroke should go on: it asks strokes to end
                nodes->slotSetSelectedNodes(selection);
                settle(600);
            };
            auto activate = [&] (KisNodeSP node, const KisNodeList &selection) {
                nodes->slotUiActivatedNode(node);
                selectOnly(selection);
            };
            const KisNodeList rasterOnly{KisNodeSP(raster)};
            const KisNodeList vectorOnly{KisNodeSP(vector)};
            const KisNodeList both{KisNodeSP(raster), KisNodeSP(vector)};

            // the accurate preview, so the raster layer's own pixels show the drags
            transformConfig.writeEntry("forceLodMode", false);
            image->setLodPreferences(KisLodPreferences());

            // a drag on the raster layer last, so the first stroke here doesn't continue an applied one
            startLiquify(raster);
            drag(QPointF(240, 130), QPointF(240, 150));
            transform->requestStrokeEnd();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();

            startLiquify(vector);
            setSize(130);
            const VCurveData curvesAtStart = stroke->curves();
            const int undoAtStart = undo->index();

            // another layer becomes active and selected
            drag(QPointF(240, 150), QPointF(240, 260));
            const VCurveData afterVector = stroke->curves();
            QVERIFY(afterVector != curvesAtStart);
            activate(raster, rasterOnly);
            QCOMPARE(undo->index(), undoAtStart + 1);
            QVERIFY(stroke->curves() == afterVector);
            QCOMPARE(shownSize(), 130.0);
            QImage rasterBefore = rasterNow();
            drag(QPointF(240, 130), QPointF(240, 210));
            QVERIFY(rasterNow() != rasterBefore);
            QVERIFY(stroke->curves() == afterVector);

            // a layer joins the selection, and the active layer stays
            selectOnly(both);
            QCOMPARE(undo->index(), undoAtStart + 2);
            rasterBefore = rasterNow();
            drag(QPointF(300, 130), QPointF(300, 240));
            const VCurveData afterBoth = stroke->curves();
            QVERIFY(afterBoth != afterVector);
            QVERIFY(rasterNow() != rasterBefore);

            // a layer leaves the selection, and the active layer stays
            selectOnly(rasterOnly);
            QCOMPARE(undo->index(), undoAtStart + 3);
            QVERIFY(stroke->curves() == afterBoth);
            drag(QPointF(300, 130), QPointF(300, 240));
            QVERIFY(stroke->curves() == afterBoth);
            selectOnly(rasterOnly);
            transform->requestUndoDuringStroke(); // the same selection again: the stroke goes on
            settle(600);
            QCOMPARE(undo->index(), undoAtStart + 3);

            // nothing done since: no undo step
            selectOnly(both);
            selectOnly(rasterOnly);
            QCOMPARE(undo->index(), undoAtStart + 3);

            // the selection changes during a drag: the drag ends on the layers it started on
            activate(vector, vectorOnly);
            drag(QPointF(240, 150), QPointF(240, 260), [&] () { nodes->slotSetSelectedNodes(rasterOnly); });
            settle(600);
            const VCurveData afterMidway = stroke->curves();
            QVERIFY(afterMidway != afterBoth);
            rasterBefore = rasterNow();
            drag(QPointF(240, 130), QPointF(240, 210));
            QVERIFY(stroke->curves() == afterMidway);
            QVERIFY(rasterNow() != rasterBefore);

            // the selection changes while the stroke is still starting
            nodes->slotSetSelectedNodes(both);
            QCoreApplication::processEvents();
            selectOnly(vectorOnly);
            rasterBefore = rasterNow();
            drag(QPointF(300, 150), QPointF(300, 260));
            QVERIFY(stroke->curves() != afterMidway);
            QVERIFY(rasterNow() == rasterBefore);

            // each step undoes on its own, down to before the first
            transform->requestStrokeEnd();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            while (undo->index() > undoAtStart) {
                undo->undo();
                image->waitForDone();
            }
            QTRY_VERIFY(stroke->curves() == curvesAtStart);
        }

        // the dashed ring shows where the effect ends, at twice the circle's radius, unless turned off
        {
            startLiquify(vector);
            setSize(100);
            QCheckBox *showReach = nullptr;
            Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
                if (!showReach && widget) showReach = widget->findChild<QCheckBox*>("liquifyShowReachChk");
            }
            QVERIFY(showReach);
            QVERIFY(showReach->isChecked());
            // directly under Reverse: the rows only Restore Shape shows leave no gap above it
            {
                QCheckBox *reverse = showReach->parentWidget()->findChild<QCheckBox*>("liquifyReverseDirectionChk");
                QVERIFY(reverse);
                QGridLayout *grid = nullptr;
                Q_FOREACH (QGridLayout *candidate, showReach->parentWidget()->findChildren<QGridLayout*>()) {
                    if (candidate->indexOf(showReach) >= 0) grid = candidate;
                }
                QVERIFY(grid);
                int row, reverseRow, column, rowSpan, columnSpan;
                grid->getItemPosition(grid->indexOf(showReach), &row, &column, &rowSpan, &columnSpan);
                grid->getItemPosition(grid->indexOf(reverse), &reverseRow, &column, &rowSpan, &columnSpan);
                QCOMPARE(row, reverseRow + 1);
            }
            auto outlines = [&] () {
                QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                KoPointerEvent hover(&moveEvent, QPointF(300, 200) / ppp);
                transform->mouseMoveEvent(&hover);
                QImage canvas(1600, 1200, QImage::Format_ARGB32_Premultiplied);
                canvas.fill(0);
                QPainter painter(&canvas);
                transform->paint(painter, *view->canvasBase()->viewConverter());
                painter.end();
                return canvas;
            };
            auto bounds = [] (const QImage &image) {
                QRect result;
                for (int y = 0; y < image.height(); y++) {
                    for (int x = 0; x < image.width(); x++) {
                        if (image.pixel(x, y) != 0) result |= QRect(x, y, 1, 1);
                    }
                }
                return result;
            };

            const QImage withRing = outlines();
            showReach->setChecked(false);
            QCOMPARE(KSharedConfig::openConfig()->group("LiquifyTool").readEntry("showEffectReach", true), false);
            const QImage withoutRing = outlines();
            const QRect circle = bounds(withoutRing);
            const QRect ring = bounds(withRing);
            QVERIFY(circle.width() > 20);
            QVERIFY2(qAbs(ring.width() - 2 * circle.width()) <= 4, qPrintable(QString("%1 %2").arg(ring.width()).arg(circle.width())));
            QVERIFY(qAbs(ring.center().x() - circle.center().x()) <= 2);

            // it has gaps all the way around
            const qreal radius = 0.5 * ring.width() - 1;
            int edges = 0;
            bool last = false;
            for (int i = 0; i < 1440; i++) {
                const QPointF at = QPointF(ring.center()) + radius * QPointF(cos(i * M_PI / 720), sin(i * M_PI / 720));
                bool on = false;
                for (int dy = -2; dy <= 2 && !on; dy++) {
                    for (int dx = -2; dx <= 2 && !on; dx++) {
                        on = withRing.pixel(at.toPoint() + QPoint(dx, dy)) != 0;
                    }
                }
                edges += on != last;
                last = on;
            }
            QVERIFY2(edges >= 16, qPrintable(QString::number(edges)));

            // the choice stays for the next stroke, and doesn't touch the deformation or its undo
            transform->requestStrokeCancellation();
            image->waitForDone();
            startLiquify(vector);
            QVERIFY(!showReach->isChecked());
            showReach->setChecked(true);
            QCOMPARE(bounds(outlines()).width(), ring.width());
            transform->requestStrokeCancellation();
            image->waitForDone();
        }
    }

    image->setLodPreferences(KisLodPreferences());
    nodes->slotUiActivatedNode(raster);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTest::qWait(100);
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VLiquifyPreviewTest)

#include "VLiquifyPreviewTest.moc"
