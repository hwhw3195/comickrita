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
#include <KConfigGroup>
#include <KSharedConfig>
#include <cstdio>

#include <KoCanvasController.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoPathShape.h>
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
#include <kis_coordinates_converter.h>
#include <kis_group_layer.h>
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
 * Transforming several layers at once, in a window: the Fast preview shows
 * vector layers where they are at any resolution.
 */
class VFastPreviewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPreviewPlacesVectorLayers();
};

namespace {

using VWindowTest::settle;

/// the area of mostly blue pixels the tool's preview paints, in image px
QRectF bluePreviewRect(KisTool *transform, KisView *view)
{
    QImage painted(2400, 1800, QImage::Format_ARGB32_Premultiplied);
    painted.fill(Qt::transparent);
    {
        QPainter painter(&painted);
        transform->paint(painter, *view->canvasBase()->viewConverter());
    }
    QRect ink;
    for (int y = 0; y < painted.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(painted.constScanLine(y));
        for (int x = 0; x < painted.width(); x++) {
            if (qAlpha(line[x]) > 40 && qBlue(line[x]) > 2 * qMax(qRed(line[x]), qGreen(line[x])) + 20) {
                ink |= QRect(x, y, 1, 1);
            }
        }
    }
    if (ink.isEmpty()) return QRectF();
    KisCanvas2 *canvas = dynamic_cast<KisCanvas2*>(view->canvasBase());
    return canvas->coordinatesConverter()->widgetToImage(QRectF(ink));
}

QString str(const QRectF &r)
{
    return QString("(%1,%2 %3x%4)").arg(qRound(r.x())).arg(qRound(r.y())).arg(qRound(r.width())).arg(qRound(r.height()));
}

}

void VFastPreviewTest::testPreviewPlacesVectorLayers()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("useOverlayPreviewStyle", true);
    transformConfig.writeEntry("forceLodMode", false);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, 1000, 800, cs, "fast preview");
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

    QAction *freeAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformFree");
    QVERIFY(freeAction);
    freeAction->trigger();
    QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
    KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
    QVERIFY(transform);
    QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));

    // two small marks far apart, so the handles stay clear of what's measured
    auto addRaster = [&] (KisNodeSP parent) {
        KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
        raster->paintDevice()->fill(QRect(40, 40, 30, 30), KoColor(Qt::red, cs));
        raster->paintDevice()->fill(QRect(930, 730, 30, 30), KoColor(Qt::red, cs));
        image->addNode(raster, parent);
        return raster;
    };
    // a blue brush stroke from (500, 500) to (800, 500)
    auto addFreehand = [&] (KisNodeSP parent, qreal ppp) {
        KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
        vector->setNodeProperty("freehandvector", true);
        image->addNode(vector, parent);
        VStrokeData data;
        data.preset = preset;
        data.brushSize = 20;
        QVector<VStrokePrimitive> line;
        KisPaintInformation prev(QPointF(500, 500), 1.0);
        for (int x = 510; x <= 800; x += 10) {
            KisPaintInformation next(QPointF(x, 500), 1.0);
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
    // a blue rectangle in the same place
    auto addStock = [&] (KisNodeSP parent, qreal ppp) {
        KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "stock", OPACITY_OPAQUE_U8);
        image->addNode(vector, parent);
        KoPathShape *path = new KoPathShape();
        path->moveTo(QPointF(500, 490) / ppp);
        path->lineTo(QPointF(800, 490) / ppp);
        path->lineTo(QPointF(800, 510) / ppp);
        path->lineTo(QPointF(500, 510) / ppp);
        path->close();
        path->normalize();
        path->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
        vector->addShape(path);
        return vector;
    };

    enum Selection { FreehandAndRaster, StockAndRaster, Group, FreehandAlone };
    struct Case { qreal dpi; Selection selection; const char *name; };
    const QList<Case> cases = {
        {72, FreehandAndRaster, "72 dpi, freehand and raster"},
        {72, StockAndRaster, "72 dpi, stock and raster"},
        {72, Group, "72 dpi, group"},
        {72, FreehandAlone, "72 dpi, freehand alone"},
        {300, FreehandAndRaster, "300 dpi, freehand and raster"},
        {300, StockAndRaster, "300 dpi, stock and raster"},
        {300, Group, "300 dpi, group"},
        {300, FreehandAlone, "300 dpi, freehand alone"},
    };

    QStringList failures;
    for (const Case &c : cases) {
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();

        const qreal ppp = c.dpi / 72.0;
        image->setResolution(ppp, ppp);

        KisNodeList selection;
        KisNodeSP active;
        if (c.selection == Group) {
            KisGroupLayerSP group = new KisGroupLayer(image, "group", OPACITY_OPAQUE_U8);
            image->addNode(group);
            addRaster(group);
            addFreehand(group, ppp);
            selection << group;
            active = group;
        } else {
            KisNodeSP vector = c.selection == StockAndRaster ? KisNodeSP(addStock(image->root(), ppp))
                                                             : KisNodeSP(addFreehand(image->root(), ppp));
            selection << vector;
            active = vector;
            if (c.selection != FreehandAlone) selection << addRaster(image->root());
        }
        settle(image, 500);

        nodes->slotUiActivatedNode(active);
        nodes->slotSetSelectedNodes(selection);
        freeAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        // waiting on the image would end the transform stroke
        QTest::qWait(1000);

        const QRectF shown = bluePreviewRect(transform, view);
        const QRectF expected(500, 490, 300, 20);
        const bool right = !shown.isEmpty() &&
            qAbs(shown.center().x() - expected.center().x()) < 30 &&
            qAbs(shown.center().y() - expected.center().y()) < 30 &&
            qAbs(shown.width() - expected.width()) < 40;
        std::fprintf(stderr, "%-32s preview %s, expected %s: %s\n", c.name, qPrintable(str(shown)), qPrintable(str(expected)), right ? "right" : "WRONG");
        if (!right) failures << QString("%1: preview %2").arg(c.name, str(shown));
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));

    transformConfig.writeEntry("useOverlayPreviewStyle", false);

    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VFastPreviewTest)
#include "VFastPreviewTest.moc"
