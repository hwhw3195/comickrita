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
#include <kundo2stack.h>
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
 * Switching transform modes applies what is done so far, as one undo step, for
 * raster and vector layers and both previews. Choosing the current mode again
 * still resets, and a switch with nothing done adds no step.
 */
class VModeSwitchAppliesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSwitchApplies();
};

namespace {

QToolButton *modeButton(KisTool *transform, const QString &name)
{
    Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
        if (!widget) continue;
        if (QToolButton *button = widget->findChild<QToolButton*>(name)) return button;
    }
    return nullptr;
}

enum Edit { MoveFree, DragWarp, DragLiquify, NoEdit };

void drag(KisTool *transform, KisView *view, const QPointF &from, const QPointF &to, qreal ppp)
{
    QImage canvas(64, 64, QImage::Format_ARGB32_Premultiplied);
    QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    KoPointerEvent hover(&hoverEvent, from / ppp);
    transform->mouseMoveEvent(&hover);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent begin(&pressEvent, from / ppp);
    transform->beginPrimaryAction(&begin);
    for (int step = 1; step <= 10; step++) {
        QMouseEvent moveEvent(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent moved(&moveEvent, (from + (to - from) * step / 10.0) / ppp);
        transform->continuePrimaryAction(&moved);
        QPainter painter(&canvas);
        transform->paint(painter, *view->canvasBase()->viewConverter());
        painter.end();
        QTest::qWait(30);
    }
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    KoPointerEvent end(&releaseEvent, to / ppp);
    transform->endPrimaryAction(&end);
    QPainter painter(&canvas);
    transform->paint(painter, *view->canvasBase()->viewConverter());
}

qreal distance(const VCurveData &a, const QTransform &toImageA, const VCurveData &b, const QTransform &toImageB)
{
    qreal result = 0;
    for (int i = 0; i < a.positions.size() && i < b.positions.size(); i++) {
        result = qMax(result, QLineF(toImageA.map(a.positions[i]), toImageB.map(b.positions[i])).length());
    }
    return result;
}

}

void VModeSwitchAppliesTest::testSwitchApplies()
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

    struct Case {
        const char *name;
        bool withVector;
        bool fast;
        const char *editIn;
        Edit edit;
        const char *switchTo;
        int steps;
        bool rasterChanges;
        bool vectorChanges;
    };
    const QList<Case> cases = {
        {"raster, Accurate, Free to Warp", false, false, "freeTransformButton", MoveFree, "warpButton", 1, true, false},
        {"raster, Fast, Free to Warp", false, true, "freeTransformButton", MoveFree, "warpButton", 1, true, false},
        {"both, Accurate, Free to Liquify", true, false, "freeTransformButton", MoveFree, "liquifyButton", 1, true, true},
        {"both, Fast, Free to Liquify", true, true, "freeTransformButton", MoveFree, "liquifyButton", 1, true, true},
        // Warp moves freehand layers too (the deforming modes, 2026-11-15)
        {"both, Accurate, Warp to Liquify", true, false, "warpButton", DragWarp, "liquifyButton", 1, true, true},
        {"both, Fast, Warp to Perspective", true, true, "warpButton", DragWarp, "perspectiveTransformButton", 1, true, true},
        {"both, Accurate, Liquify to Warp", true, false, "liquifyButton", DragLiquify, "warpButton", 1, true, true},
        {"both, Accurate, Free again resets", true, false, "freeTransformButton", MoveFree, "freeTransformButton", 0, false, false},
        {"both, Accurate, nothing done", true, false, "freeTransformButton", NoEdit, "warpButton", 0, false, false},
    };

    QStringList failures;
    for (const Case &c : cases) {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        transformConfig.writeEntry("useOverlayPreviewStyle", c.fast);

        // layers of their own, so a row doesn't continue the last row's transform
        KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
        raster->paintDevice()->fill(QRect(180, 100, 120, 60), KoColor(Qt::red, cs));
        image->addNode(raster);
        KisShapeLayerSP vector;
        VStrokeShape *stroke = nullptr;
        KisNodeList selected{KisNodeSP(raster)};
        if (c.withVector) {
            vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
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
            stroke = VStrokeShape::fromImagePixels(line, ppp, data);
            vector->addShape(stroke);
            for (int i = 0; i < 5; i++) {
                stroke->waitForRendering();
                QTest::qWait(20);
            }
            selected << KisNodeSP(vector);
        }
        QTest::qWait(300);
        image->waitForDone();
        const QRect rasterBefore = raster->exactBounds();
        const VCurveData curvesBefore = stroke ? stroke->curves() : VCurveData();
        const QTransform scale = QTransform::fromScale(ppp, ppp);
        const QTransform placeBefore = stroke ? stroke->absoluteTransformation() * scale : QTransform();
        const int stepsBefore = doc->undoStack()->index();

        nodes->slotUiActivatedNode(c.withVector ? KisNodeSP(vector) : KisNodeSP(raster));
        nodes->slotSetSelectedNodes(selected);
        freeAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QVERIFY(transform);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
        // the stroke started before the tool read the setting
        QVERIFY(QMetaObject::invokeMethod(transform, "slotRestartTransform"));
        QTest::qWait(600);

        QToolButton *editIn = modeButton(transform, c.editIn);
        QToolButton *switchTo = modeButton(transform, c.switchTo);
        QVERIFY(editIn && switchTo);
        editIn->click();
        QTest::qWait(600);

        if (c.edit == MoveFree) {
            transform->setProperty("translateX", transform->property("translateX").toDouble() + 100);
            QVERIFY(QMetaObject::invokeMethod(transform, "slotEditingFinished"));
        } else if (c.edit == DragWarp) {
            drag(transform, view, QPointF(180, 100), QPointF(120, 60), ppp);
        } else if (c.edit == DragLiquify) {
            drag(transform, view, QPointF(240, 150), QPointF(240, 260), ppp);
        }
        QTest::qWait(300);

        switchTo->click();
        QTest::qWait(800);

        QVERIFY(QMetaObject::invokeMethod(transform, "requestStrokeEnd"));
        QTest::qWait(300);
        image->waitForDone();

        const int steps = doc->undoStack()->index() - stepsBefore;
        const bool rasterChanged = raster->exactBounds() != rasterBefore;
        const qreal vectorMoved = stroke ? distance(curvesBefore, placeBefore, stroke->curves(), stroke->absoluteTransformation() * scale) : 0;

        // and one undo per step brings the layers back
        for (int i = 0; i < steps; i++) doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(300);
        image->waitForDone();
        const bool rasterBack = raster->exactBounds() == rasterBefore;
        const qreal vectorLeft = stroke ? distance(curvesBefore, placeBefore, stroke->curves(), stroke->absoluteTransformation() * scale) : 0;

        const bool right = steps == c.steps && rasterChanged == c.rasterChanges && (vectorMoved > 20) == c.vectorChanges
            && (c.vectorChanges || vectorMoved < 1) && rasterBack && vectorLeft < 1;
        std::fprintf(stderr, "%-36s undo steps %d, raster changed %d, vector moved %.1f px, after undo raster back %d, vector off by %.1f px: %s\n",
                     c.name, steps, rasterChanged, vectorMoved, rasterBack, vectorLeft, right ? "right" : "WRONG");
        if (!right) failures << c.name;
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));

    transformConfig.writeEntry("useOverlayPreviewStyle", false);

    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VModeSwitchAppliesTest)
#include "VModeSwitchAppliesTest.moc"
