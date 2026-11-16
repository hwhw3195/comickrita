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
#include <QStandardItemModel>
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
#include <testui.h>

#include "VCurveShape.h"
#include "VStrokePrimitive.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * Switching to Liquify inside a transform of a raster and a freehand layer:
 * from any mode, the freehand layer comes along and the preview is Accurate,
 * even from the modes that leave vector layers out.
 *
 * Free Transform of a freehand layer previews at full resolution only (a
 * reduced preview would scale the brush sizes), so Accurate with Instant
 * Preview is greyed out and shows as Accurate, and the saved choice stays
 * for a raster layer.
 */
class VLiquifyModeSwitchTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSwitchingToLiquify();
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

bool modeEnabled(QComboBox *modes, int index)
{
    QStandardItemModel *model = qobject_cast<QStandardItemModel*>(modes->model());
    return model && model->item(index)->isEnabled();
}

}

void VLiquifyModeSwitchTest::testSwitchingToLiquify()
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
    transformConfig.writeEntry("forceLodMode", false);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 600, 400, cs, "switching");
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

    struct Case { const char *button; bool fast; const char *name; };
    const QList<Case> cases = {
        {"freeTransformButton", true, "Fast, from Free Transform"},
        {"warpButton", true, "Fast, from Warp"},
        {"cageButton", true, "Fast, from Cage"},
        {"meshButton", true, "Fast, from Mesh"},
        {"perspectiveTransformButton", true, "Fast, from Perspective"},
        {"freeTransformButton", false, "Accurate, from Free Transform"},
        {"warpButton", false, "Accurate, from Warp"},
    };

    QStringList failures;
    for (const Case &c : cases) {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        transformConfig.writeEntry("useOverlayPreviewStyle", c.fast);

        // layers of its own, so a row doesn't continue the last row's transform
        KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
        raster->paintDevice()->fill(QRect(180, 100, 120, 60), KoColor(Qt::red, cs));
        image->addNode(raster);
        KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
        vector->setNodeProperty("freehandvector", true);
        image->addNode(vector);
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
        VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, ppp, data);
        vector->addShape(stroke);
        for (int i = 0; i < 5; i++) {
            stroke->waitForRendering();
            QTest::qWait(20);
        }
        QTest::qWait(300);
        image->waitForDone();
        const VCurveData original = stroke->curves();

        nodes->slotUiActivatedNode(vector);
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(raster), KisNodeSP(vector)});
        freeAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QVERIFY(transform);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
        // the stroke started before the tool read the setting
        QVERIFY(QMetaObject::invokeMethod(transform, "slotRestartTransform"));
        QTest::qWait(600);

        QToolButton *from = modeButton(transform, c.button);
        QToolButton *liquify = modeButton(transform, "liquifyButton");
        QVERIFY(from && liquify);
        from->click();
        QTest::qWait(600);
        liquify->click();
        QTest::qWait(600);

        QComboBox *modes = previewModes(transform);
        QVERIFY(modes);
        // the combo shows the saved mode, and offers Accurate and Fast
        const bool modeShown = modes->currentIndex() == (c.fast ? 2 : 0) && !modeEnabled(modes, 1) && modeEnabled(modes, 2);
        bool warped = false;

        // a Move drag down across the stroke
        QImage canvas(64, 64, QImage::Format_ARGB32_Premultiplied);
        QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin(&pressEvent, QPointF(240, 150) / ppp);
        transform->beginPrimaryAction(&begin);
        for (int step = 1; step <= 10; step++) {
            QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent moved(&moveEvent, QPointF(240, 150 + 11 * step) / ppp);
            transform->continuePrimaryAction(&moved);
            QPainter painter(&canvas);
            transform->paint(painter, *view->canvasBase()->viewConverter());
            painter.end();
            warped |= stroke->isRendersPaused();
            QTest::qWait(30);
        }
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end(&releaseEvent, QPointF(240, 260) / ppp);
        transform->endPrimaryAction(&end);
        {
            QPainter painter(&canvas);
            transform->paint(painter, *view->canvasBase()->viewConverter());
        }
        QTest::qWait(600);

        const VCurveData now = stroke->curves();
        const QTransform toImage = stroke->absoluteTransformation() * QTransform::fromScale(ppp, ppp);
        qreal moved = 0;
        for (int i = 0; i < now.positions.size() && i < original.positions.size(); i++) {
            moved = qMax(moved, QLineF(toImage.map(original.positions[i]), toImage.map(now.positions[i])).length());
        }
        // Fast bends the stroke while it moves, Accurate renders it
        const bool right = modeShown && warped == c.fast && !stroke->isRendersPaused() && moved > 20;
        std::fprintf(stderr, "%-32s preview mode %d, shown right %d, warped %d, stroke moved %.1f px: %s\n", c.name, modes->currentIndex(), modeShown, warped, moved, right ? "right" : "WRONG");
        if (!right) failures << c.name;
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));

    // Accurate with Instant Preview saved: off for a freehand layer, kept for a raster one
    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    transformConfig.writeEntry("forceLodMode", true);
    KisNodeSP lastVector;
    KisNodeSP lastRaster;
    for (KisNodeSP node = image->root()->lastChild(); node; node = node->prevSibling()) {
        if (!lastVector && dynamic_cast<KisShapeLayer*>(node.data())) lastVector = node;
        if (!lastRaster && node->name() == "raster") lastRaster = node;
    }
    QVERIFY(lastVector && lastRaster);
    auto freeTransformModes = [&] (KisNodeSP node) -> QComboBox* {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        if (!QTest::qWaitFor([&] () { return tools->activeToolId() == QString("KritaShape/KisToolBrush"); })) return nullptr;
        image->waitForDone();
        nodes->slotUiActivatedNode(node);
        nodes->slotSetSelectedNodes(KisNodeList{node});
        freeAction->trigger();
        if (!QTest::qWaitFor([&] () { return tools->activeToolId() == QString("KisToolTransform"); })) return nullptr;
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        if (!transform) return nullptr;
        QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged");
        QMetaObject::invokeMethod(transform, "slotRestartTransform");
        QTest::qWait(600);
        if (QToolButton *freeButton = modeButton(transform, "freeTransformButton")) freeButton->click();
        QTest::qWait(600);
        return previewModes(transform);
    };
    QComboBox *modes = freeTransformModes(lastVector);
    QVERIFY(modes);
    QCOMPARE(modes->currentIndex(), 0);
    QVERIFY(!modeEnabled(modes, 1));
    QVERIFY(modeEnabled(modes, 2));
    QVERIFY(!modes->toolTip().isEmpty());
    modes = freeTransformModes(lastRaster);
    QVERIFY(modes);
    QCOMPARE(modes->currentIndex(), 1);
    QVERIFY(modeEnabled(modes, 1));
    QVERIFY(modes->toolTip().isEmpty());
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
    image->waitForDone();
    transformConfig.writeEntry("forceLodMode", false);

    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VLiquifyModeSwitchTest)
#include "VLiquifyModeSwitchTest.moc"
