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
#include <QToolButton>
#include <KConfigGroup>
#include <KSharedConfig>

#include <KoCanvasController.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoPathShape.h>
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
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
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
 * The transform tool's Scale brush size checkbox: where it shows, and what
 * Free Transform does to brush strokes with it on and off.
 */
class VScaleBrushSizeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testCheckbox();
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

/// the height of what \p layer paints at the column \p x, in pixels
int paintedHeight(KisNodeSP layer, int x)
{
    const QRect bounds = layer->projection()->exactBounds();
    int height = 0;
    KoColor color(layer->colorSpace());
    for (int y = bounds.top(); y <= bounds.bottom(); y++) {
        layer->projection()->pixel(x, y, &color);
        if (color.opacityU8() > 127) height++;
    }
    return height;
}
}

void VScaleBrushSizeTest::testCheckbox()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("scaleBrushSizes", false);
    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    transformConfig.writeEntry("forceLodMode", false);

    const qreal ppp = 300.0 / 72.0;
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 2000, 1600, cs, "scale brush");
    image->setResolution(ppp, ppp);
    KisDocument *doc = VWindowTest::addDocument(image);

    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    raster->paintDevice()->fill(QRect(100, 100, 100, 100), KoColor(Qt::red, cs));
    image->addNode(raster);

    KisShapeLayerSP stock = new KisShapeLayer(doc->shapeController(), image, "stock", OPACITY_OPAQUE_U8);
    image->addNode(stock);
    auto rectanglePath = [] () {
        KoPathShape *path = new KoPathShape();
        path->moveTo(QPointF(20, 20));
        path->lineTo(QPointF(60, 20));
        path->lineTo(QPointF(60, 60));
        path->close();
        path->normalize();
        path->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
        return path;
    };
    stock->addShape(rectanglePath());

    // a poly-tool line only: its width already scales with the shape
    KisShapeLayerSP polyOnly = new KisShapeLayer(doc->shapeController(), image, "poly", OPACITY_OPAQUE_U8);
    polyOnly->setNodeProperty("freehandvector", true);
    image->addNode(polyOnly);
    KoPathShape *path = rectanglePath();
    VGeomCurveShape *geom = VCurveConverter::fromPath(path);
    delete path;
    QVERIFY(geom);
    polyOnly->addShape(geom);

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

    auto addFreehand = [&] (KisNodeSP parent) {
        KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
        vector->setNodeProperty("freehandvector", true);
        image->addNode(vector, parent);
        VStrokeData data;
        data.preset = preset;
        data.brushSize = 20;
        QVector<VStrokePrimitive> line;
        KisPaintInformation prev(QPointF(300, 300), 1.0);
        for (int x = 310; x <= 600; x += 10) {
            KisPaintInformation next(QPointF(x, 300), 1.0);
            line << VStrokePrimitive::line(prev, next);
            prev = next;
        }
        VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, ppp, data);
        stroke->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
        vector->addShape(stroke);
        for (int i = 0; i < 5; i++) {
            stroke->waitForRendering();
            QTest::qWait(20);
        }
        return vector;
    };
    KisShapeLayerSP freehand = addFreehand(image->root());
    KisGroupLayerSP group = new KisGroupLayer(image, "group", OPACITY_OPAQUE_U8);
    image->addNode(group);
    addFreehand(group);
    settle(image, 500);

    QAction *freeAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformFree");
    QVERIFY(freeAction);
    auto start = [&] (KisNodeSP active, const KisNodeList &selected) -> KisTool* {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        settle(image, 200);
        nodes->slotUiActivatedNode(active);
        nodes->slotSetSelectedNodes(selected);
        settle(image, 300);
        freeAction->trigger();
        // settle() would end the stroke: waitForDone() asks the tool to
        QTest::qWait(800);
        return dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
    };
    auto finish = [&] (KisTool *transform) {
        QMetaObject::invokeMethod(transform, "requestStrokeEnd");
        settle(image, 500);
    };

    KisTool *transform = nullptr;

    // what it does: each row scales twice over from the same stroke, then undoes
    VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(freehand->shapes().first());
    QVERIFY(stroke);
    const QVector<qreal> radiiBefore = stroke->curves().radii;
    const int heightBefore = paintedHeight(freehand, 450);
    QVERIFY(heightBefore > 10);

    // each preview mode: Accurate, Accurate with instant preview, Fast
    struct Mode { const char *name; bool overlay; bool forceLod; };
    for (const Mode &mode : {Mode{"accurate", false, false}, Mode{"instant", false, true}, Mode{"fast", true, false}})
    for (const bool on : {false, true}) {
        const QString name = QString("%1, %2").arg(mode.name).arg(on ? "on" : "off");
        transformConfig.writeEntry("useOverlayPreviewStyle", mode.overlay);
        transformConfig.writeEntry("forceLodMode", mode.forceLod);
        if (KisTool *tool = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"))) {
            QMetaObject::invokeMethod(tool, "slotGlobalConfigChanged");
        }
        const int steps = doc->undoStack()->index();
        transform = start(freehand, {freehand});
        QCheckBox *box = optionChild<QCheckBox>(transform, "chkScaleBrushSizes");
        QVERIFY(box);
        box->setChecked(on);
        QTest::qWait(200);
        transform->setProperty("scaleX", 200.0);
        transform->setProperty("scaleY", 200.0);
        QVERIFY(QMetaObject::invokeMethod(transform, "slotEditingFinished"));
        QVERIFY(QMetaObject::invokeMethod(transform, "requestImageRecalculation"));
        QTest::qWait(800);
        finish(transform);
        settle(image, 300);
        for (int i = 0; i < 5; i++) {
            stroke->waitForRendering();
            QTest::qWait(20);
        }
        freehand->forceUpdateTimedNode();
        settle(image, 300);

        QVERIFY2(doc->undoStack()->index() == steps + 1,
                 qPrintable(QString("%1: %2 steps").arg(name).arg(doc->undoStack()->index() - steps)));
        const qreal factor = on ? 2.0 : 1.0;
        QVERIFY2(qAbs(stroke->curves().radii.first() - radiiBefore.first() * factor) < 1e-6,
                 qPrintable(QString("%1: radius %2 from %3").arg(name).arg(stroke->curves().radii.first()).arg(radiiBefore.first())));
        // the paint follows: the stroke moved, so look where it is now
        const QRect bounds = freehand->projection()->exactBounds();
        const int height = paintedHeight(freehand, bounds.center().x());
        QVERIFY2(qAbs(height - heightBefore * factor) <= heightBefore * 0.25,
                 qPrintable(QString("%1: painted %2 from %3").arg(name).arg(height).arg(heightBefore)));

        doc->undoStack()->undo();
        settle(image, 500);
        QVERIFY2(stroke->curves().radii == radiiBefore, qPrintable(name));
    }

    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    transformConfig.writeEntry("forceLodMode", false);

    // where it shows: Free Transform and the deforming modes (Warp), with brush strokes in it; not Liquify
    struct Row { const char *name; KisNodeSP active; KisNodeList selected; bool shown; };
    const QList<Row> rows = {
        {"raster", raster, {raster}, false},
        {"stock vector", stock, {stock}, false},
        {"freehand with a poly-tool line only", polyOnly, {polyOnly}, false},
        {"freehand with a brush stroke", freehand, {freehand}, true},
        {"freehand and raster", freehand, {raster, freehand}, true},
        {"stock and raster", stock, {raster, stock}, false},
        // last: the brush tool doesn't activate on a group, so start() can't leave Transform from it
        {"group with a freehand layer", group, {group}, true},
    };
    for (const Row &row : rows) {
        transform = start(row.active, row.selected);
        QVERIFY2(transform, row.name);
        QCheckBox *box = optionChild<QCheckBox>(transform, "chkScaleBrushSizes");
        QVERIFY2(box, row.name);
        QVERIFY2(box->isHidden() == !row.shown, row.name);
        if (row.shown) {
            QToolButton *warp = optionChild<QToolButton>(transform, "warpButton");
            QToolButton *liquify = optionChild<QToolButton>(transform, "liquifyButton");
            QToolButton *free = optionChild<QToolButton>(transform, "freeTransformButton");
            QVERIFY2(warp && liquify && free, row.name);
            warp->click();
            QTest::qWait(500);
            QVERIFY2(!box->isHidden(), row.name);
            liquify->click();
            QTest::qWait(500);
            QVERIFY2(box->isHidden(), row.name);
            free->click();
            QTest::qWait(800);
            QVERIFY2(!box->isHidden(), row.name);
        }
        finish(transform);
    }

    transformConfig.writeEntry("scaleBrushSizes", false);
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VScaleBrushSizeTest)
#include "VScaleBrushSizeTest.moc"
