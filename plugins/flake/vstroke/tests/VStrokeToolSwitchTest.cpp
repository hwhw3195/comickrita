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
#include <KSharedConfig>
#include <KConfigGroup>
#include <QAbstractButton>
#include <QImage>
#include <QPainter>
#include <KoColor.h>
#include <kis_paint_device.h>
#include <QToolButton>
#include <QWidget>
#include <QtEndian>
#include <cmath>
#include <QtTest/qtestspontaneevent.h>

#include <KoCanvasController.h>
#include <kactioncollection.h>
#include <KoColorSpaceRegistry.h>
#include <KoToolManager.h>
#include <KoShapeController.h>
#include <KoPointerEvent.h>
#include <QKeyEvent>
#include <KoToolRegistry.h>
#include <actions/KisPasteActionFactories.h>
#include <kis_tool.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <KisGlobalResourcesInterface.h>
#include <KoCanvasResourceProvider.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <kis_canvas2.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <kis_image.h>
#include <KisLodPreferences.h>
#include <kis_node_manager.h>
#include <kis_paint_layer.h>
#include <kis_shape_layer.h>
#include <KoProperties.h>
#include <KoPathShape.h>
#include <KoShapeStroke.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeController.h>
#include <kundo2stack.h>

#include <testui.h>
#include <KisShapeLayerHooks.h>

#include "VCurveShape.h"
#include "VStrokePrimitive.h"
#include "VStrokeCurves.h"
#include "VStrokeShape.h"
#include <KoColorBackground.h>
#include "VGeomCurveShape.h"
#include "VCurveOpacity.h"
#include "VEditTool.h"
#include "VPointEditTool.h"
#include "VWindowTestUtils.h"

/**
 * The brush button and freehand vector layers through the installed
 * plugins: Krita loads the modules as the application does, with a real
 * window, view, tool manager and paste action. The test links the vstroke
 * library only to look into the shapes.
 */
class VStrokeToolSwitchTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSwitchInWindow();
};

namespace {

using VWindowTest::encode;

QByteArray svgDocument(const QString &content)
{
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:krita=\"http://krita.org/namespaces/svg/krita\""
                   " width=\"100pt\" height=\"100pt\" viewBox=\"0 0 100 100\">%1</svg>").arg(content).toUtf8();
}

/// a stock triangle
QByteArray stockPathSvg()
{
    return svgDocument("<path d=\"M10,10 L60,10 L60,50 Z\" fill=\"#ff0000\" stroke=\"#000000\" stroke-width=\"2\"/>");
}

/// a freehand curve, as a freehand vector layer saves and copies it
QByteArray freehandCurveSvg()
{
    const QString arrays =
        QString("<krita:a name=\"offsets\" type=\"i32\">%1</krita:a>").arg(encode(QVector<qint32>{0})) +
        QString("<krita:a name=\"cyclic\" type=\"u8\">%1</krita:a>").arg(encode(QVector<quint8>{0})) +
        QString("<krita:a name=\"position\" type=\"f64x2\">%1</krita:a>").arg(encode(QVector<double>{10, 10, 60, 30})) +
        QString("<krita:a name=\"radius\" type=\"f64\">%1</krita:a>").arg(encode(QVector<double>{2, 3}));
    return svgDocument("<path d=\"M10,10 L60,30\" fill=\"none\" stroke=\"#000000\" stroke-width=\"4\""
                       " krita:type=\"vcurve\" krita:vcurve-version=\"1\">"
                       "<krita:curves version=\"1\" points=\"2\" curves=\"1\">" + arrays + "</krita:curves></path>");
}

using VWindowTest::paste;

/// the number of pixels \p node's projection covers in \p rect
int coveredPixels(KisNodeSP node, const QRect &rect)
{
    const QImage img = node->projection()->convertToQImage(0, rect);
    int count = 0;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) count += qAlpha(line[x]) > 0;
    }
    return count;
}

/// the number of pixels in \p rect of \p node's projection that are mostly blue
int bluePixels(KisNodeSP node, const QRect &rect)
{
    const QImage img = node->projection()->convertToQImage(0, rect);
    int count = 0;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) {
            count += qAlpha(line[x]) > 0 && qBlue(line[x]) > 2 * qMax(qRed(line[x]), qGreen(line[x])) + 20;
        }
    }
    return count;
}

/// the Layers docker's safe asserts seen since the last reset (KRITA_NO_ASSERT_MSG turns them into warnings)
QStringList s_safeAsserts;
QtMessageHandler s_previousHandler = nullptr;
void catchSafeAsserts(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains("SAFE ASSERT") && message.contains("kis_dummies_facade_base")) s_safeAsserts << message;
    if (s_previousHandler) s_previousHandler(type, context, message);
}

QStringList shapeIds(KisNodeSP node)
{
    QStringList ids;
    if (KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(node.data())) {
        Q_FOREACH (KoShape *shape, layer->shapes()) ids << shape->shapeId();
    }
    return ids;
}

bool isFreehand(KisNodeSP node)
{
    return node && node->nodeProperties().boolProperty("freehandvector", false);
}

void drag(KisTool *tool, const QPointF &from, const QPointF &to)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent begin(&press, from);
    tool->beginPrimaryAction(&begin);
    QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent moved(&move, to);
    tool->continuePrimaryAction(&moved);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    // the shape tools cancel on a release that does not come from the user
    QSpontaneKeyEvent::setSpontaneous(&release);
    KoPointerEvent end(&release, to);
    tool->endPrimaryAction(&end);
}

}

void VStrokeToolSwitchTest::testSwitchInWindow()
{
    // offscreen there is no OpenGL and no resource storages: warn instead of modal dialogs
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    // the offscreen platform starts with no clipboard data, which KisClipboard dereferences
    QApplication::clipboard()->setMimeData(new QMimeData());

    // the transform tool reads the Liquify brush size from the config when
    // it is made, and saves it after each stroke: start from the default
    KSharedConfig::openConfig()->group("LiquifyTool/Move").writeEntry("size", 60.0);
    // ... and the preview style: the default, Accurate with Instant Preview
    KSharedConfig::openConfig()->group("KisToolTransform").writeEntry("useOverlayPreviewStyle", false);
    KSharedConfig::openConfig()->group("KisToolTransform").writeEntry("forceLodMode", true);

    QVERIFY(KoToolRegistry::instance()->contains("VStrokeTool"));
    QVERIFY(KoToolRegistry::instance()->contains("KritaShape/KisToolBrush"));

    KisImageSP image = new KisImage(0, 400, 300, KoColorSpaceRegistry::instance()->rgb8(), "switch");
    KisDocument *doc = VWindowTest::addDocument(image);

    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    image->addNode(raster);
    KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
    vector->setNodeProperty("freehandvector", true);
    image->addNode(vector);
    KisShapeLayerSP plain = new KisShapeLayer(doc->shapeController(), image, "plain", OPACITY_OPAQUE_U8);
    image->addNode(plain);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();

    nodes->slotUiActivatedNode(raster);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));

    // as clicking the layer: Krita first swaps the brush for the shape select tool
    nodes->slotUiActivatedNode(vector);
    QTRY_COMPARE(tools->activeToolId(), QString("VStrokeTool"));

    // the toolbox has no button for the vector brush: the brush button stands
    // for it, checked and highlighted, and shape select is neither
    {
        QToolButton *brushButton = window->findChild<QToolButton*>("KritaShape/KisToolBrush");
        QToolButton *selectButton = window->findChild<QToolButton*>("InteractionTool");
        QVERIFY(brushButton);
        QVERIFY(selectButton);
        const QColor highlight = qApp->palette().color(QPalette::Highlight);
        QTRY_VERIFY(brushButton->isChecked());
        QTRY_COMPARE(brushButton->palette().color(QPalette::Button), highlight);
        QVERIFY(!selectButton->isChecked());
        QCOMPARE(selectButton->palette().color(QPalette::Button), qApp->palette().color(QPalette::Button));
    }

    nodes->slotUiActivatedNode(raster);
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));

    nodes->slotUiActivatedNode(vector);
    QTRY_COMPARE(tools->activeToolId(), QString("VStrokeTool"));

    // the shape select tool picked by hand stays
    tools->switchToolRequested("InteractionTool");
    QTest::qWait(100);
    QCOMPARE(tools->activeToolId(), QString("InteractionTool"));

    // a stock vector layer: the vector brush only draws on freehand layers
    tools->switchToolRequested("VStrokeTool");
    QTRY_COMPARE(tools->activeToolId(), QString("VStrokeTool"));
    nodes->slotUiActivatedNode(plain);
    QTRY_COMPARE(tools->activeToolId(), QString("InteractionTool"));
    nodes->slotUiActivatedNode(raster);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
    nodes->slotUiActivatedNode(plain);
    QTest::qWait(100);
    QCOMPARE(tools->activeToolId(), QString("InteractionTool"));

    // paste: stock paths convert on a freehand layer, stay on a stock one
    nodes->slotUiActivatedNode(vector);
    paste(stockPathSvg(), viewManager);
    QCOMPARE(shapeIds(vector), QStringList({"VGeomCurveShape"}));

    nodes->slotUiActivatedNode(plain);
    paste(stockPathSvg(), viewManager);
    QCOMPARE(shapeIds(plain), QStringList({"KoPathShape"}));

    // freehand curves pasted elsewhere get a new freehand layer
    for (KisNodeSP target : {KisNodeSP(raster), KisNodeSP(plain)}) {
        nodes->slotUiActivatedNode(target);
        const int layers = image->root()->childCount();
        paste(freehandCurveSvg(), viewManager);
        QCOMPARE(int(image->root()->childCount()), layers + 1);
        KisNodeSP added = image->root()->lastChild();
        QVERIFY(isFreehand(added));
        QVERIFY(added->name().startsWith("Freehand"));
        QCOMPARE(shapeIds(added), QStringList({"VGeomCurveShape"}));
    }
    QCOMPARE(shapeIds(plain), QStringList({"KoPathShape"}));

    // the stock rectangle tool on a freehand layer (the shape tools want a
    // brush preset, and offscreen there are no resources)
    KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"),
                                                                              KisGlobalResourcesInterface::instance());
    QVERIFY(preset);
    view->canvasBase()->resourceManager()->setResource(KoCanvasResource::CurrentPaintOpPreset,
                                                       QVariant::fromValue(preset));
    nodes->slotUiActivatedNode(vector);
    tools->switchToolRequested("KritaShape/KisToolRectangle");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolRectangle"));
    KisTool *rectangle = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KritaShape/KisToolRectangle"));
    QVERIFY(rectangle);
    drag(rectangle, QPointF(120, 100), QPointF(220, 180));
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(vector), QStringList({"VGeomCurveShape", "VGeomCurveShape"}));
    // undo removes it
    doc->undoStack()->undo();
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(vector), QStringList({"VGeomCurveShape"}));

    // the line tool adds its shape by itself, not through the shape tools' path
    tools->switchToolRequested("KritaShape/KisToolLine");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolLine"));
    KisTool *line = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KritaShape/KisToolLine"));
    QVERIFY(line);
    drag(line, QPointF(100, 200), QPointF(250, 240));
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(vector), QStringList({"VGeomCurveShape", "VGeomCurveShape"}));
    {
        VGeomCurveShape *drawn = dynamic_cast<VGeomCurveShape*>(vector->shapes().last());
        QVERIFY(drawn);
        QCOMPARE(drawn->curves().pointCount(), 2);
        KoShapeStrokeSP lineStroke = qSharedPointerDynamicCast<KoShapeStroke>(drawn->stroke());
        QVERIFY(lineStroke && lineStroke->lineWidth() > 0);
        QCOMPARE(drawn->curves().radii[0], 0.5 * lineStroke->lineWidth());
        QCOMPARE(drawn->curves().radii[1], 0.5 * lineStroke->lineWidth());
    }
    doc->undoStack()->undo();
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(vector), QStringList({"VGeomCurveShape"}));

    // on a stock vector layer the line stays a stock path
    nodes->slotUiActivatedNode(plain);
    QTest::qWait(50);
    tools->switchToolRequested("KritaShape/KisToolLine");
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolLine"));
    drag(line, QPointF(100, 200), QPointF(250, 240));
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(plain), QStringList({"KoPathShape", "KoPathShape"}));
    doc->undoStack()->undo();
    image->waitForDone();
    QTest::qWait(50);
    QCOMPARE(shapeIds(plain), QStringList({"KoPathShape"}));
    nodes->slotUiActivatedNode(vector);
    QTest::qWait(50);

    // the vector brush keeps the brush's blend mode in the stroke: a QPainter mode and one of Krita's own
    {
        tools->switchToolRequested("VStrokeTool");
        QTRY_COMPARE(tools->activeToolId(), QString("VStrokeTool"));
        KisTool *brush = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "VStrokeTool"));
        QVERIFY(brush);
        const int strokesBefore = vector->shapes().size();
        for (const QString &op : {COMPOSITE_MULT, COMPOSITE_HUE}) {
            preset->settings()->setPaintOpCompositeOp(op);
            view->canvasBase()->resourceManager()->setResource(KoCanvasResource::CurrentPaintOpPreset,
                                                               QVariant::fromValue(preset));
            QCOMPARE(view->canvasBase()->resourceManager()->resource(KoCanvasResource::CurrentEffectiveCompositeOp).toString(), op);
            drag(brush, QPointF(100, 120), QPointF(220, 150));
            image->waitForDone();
            QTest::qWait(50);
            QCOMPARE(vector->shapes().size(), strokesBefore + 1);
            VStrokeShape *drawn = dynamic_cast<VStrokeShape*>(vector->shapes().last());
            QVERIFY(drawn);
            QCOMPARE(drawn->data().compositeOp, op);
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            QCOMPARE(vector->shapes().size(), strokesBefore);
        }
        preset->settings()->setPaintOpCompositeOp(COMPOSITE_OVER);
        view->canvasBase()->resourceManager()->setResource(KoCanvasResource::CurrentPaintOpPreset,
                                                           QVariant::fromValue(preset));
    }

    // the point edit tool: drag the corner of the pasted triangle, then undo
    VCurveShape *curve = dynamic_cast<VCurveShape*>(vector->shapes().first());
    QVERIFY(curve);
    const VCurveData original = curve->curves();
    const QPointF corner = curve->absoluteTransformation().map(original.positions[1]);
    tools->switchToolRequested("VPointEditTool");
    QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
    VPointEditTool *pointEdit = dynamic_cast<VPointEditTool*>(tools->toolById(view->canvasBase(), "VPointEditTool"));
    QVERIFY(pointEdit);
    // points show on selected objects only: the first click selects the triangle
    view->canvasBase()->shapeManager();
    KoSelection *shapeSelection = view->canvasBase()->selectedShapesProxy()->selection();
    shapeSelection->deselectAll();
    {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin(&press, corner + QPointF(1, 1));
        pointEdit->mousePressEvent(&begin);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end(&release, corner + QPointF(1, 1));
        pointEdit->mouseReleaseEvent(&end);
    }
    QVERIFY(shapeSelection->isSelected(curve));
    QVERIFY(pointEdit->selectedPoints(curve).isEmpty());
    QVERIFY(!pointEdit->decorationsRect().isEmpty());
    {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin(&press, corner + QPointF(1, 1));
        pointEdit->mousePressEvent(&begin);
        QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent moved(&move, corner + QPointF(16, 6));
        pointEdit->mouseMoveEvent(&moved);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end(&release, corner + QPointF(16, 6));
        pointEdit->mouseReleaseEvent(&end);
    }
    image->waitForDone();
    QCOMPARE(pointEdit->selectedPoints(curve), QVector<int>({1}));
    const QPointF movedCorner = curve->absoluteTransformation().map(curve->curves().positions[1]);
    QVERIFY2(QLineF(movedCorner, corner + QPointF(15, 5)).length() < 1e-6,
             qPrintable(QString("corner at %1,%2").arg(movedCorner.x()).arg(movedCorner.y())));
    QCOMPARE(curve->curves().positions[0], original.positions[0]);
    doc->undoStack()->undo();
    image->waitForDone();
    QVERIFY(curve->curves() == original);

    // one Edit Shapes button: the point edit tool on freehand layers
    tools->switchToolRequested("PathTool");
    QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
    // a dashed path stays stock on a freehand layer: selecting it gives stock Edit Shapes
    paste(svgDocument("<path d=\"M10,70 L90,70\" fill=\"none\" stroke=\"#000000\" stroke-width=\"2\""
                      " stroke-dasharray=\"4,2\"/>"), viewManager);
    KoPathShape *dashed = nullptr;
    Q_FOREACH (KoShape *shape, vector->shapes()) {
        if (shape->shapeId() == "KoPathShape") dashed = dynamic_cast<KoPathShape*>(shape);
    }
    QVERIFY(dashed);
    // the canvas follows the current layer's shape manager whenever it is
    // asked for it, which painting the canvas does all the time
    view->canvasBase()->shapeManager();
    shapeSelection = view->canvasBase()->selectedShapesProxy()->selection();
    shapeSelection->deselectAll();
    shapeSelection->select(dashed);
    QTRY_COMPARE(tools->activeToolId(), QString("PathTool"));
    shapeSelection->deselectAll();
    shapeSelection->select(curve);
    QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
    // mixed: the point edit tool, which skips the stock path
    shapeSelection->select(dashed);
    QTest::qWait(100);
    QCOMPARE(tools->activeToolId(), QString("VPointEditTool"));
    shapeSelection->deselectAll();
    // a stock vector layer: stock Edit Shapes; a raster layer: the brush, as stock Krita
    nodes->slotUiActivatedNode(plain);
    QTRY_COMPARE(tools->activeToolId(), QString("PathTool"));
    nodes->slotUiActivatedNode(vector);
    QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
    nodes->slotUiActivatedNode(raster);
    QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));

    // shapes from the shape tools, dragged around with the point edit tool,
    // leave nothing behind: the layer is what a full repaint gives
    {
        KisShapeLayerSP edited = new KisShapeLayer(doc->shapeController(), image, "edited", OPACITY_OPAQUE_U8);
        edited->setNodeProperty("freehandvector", true);
        image->addNode(edited);
        image->waitForDone();
        nodes->slotUiActivatedNode(edited);
        QTest::qWait(50);
        tools->switchToolRequested("KritaShape/KisToolRectangle");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolRectangle"));
        drag(rectangle, QPointF(60, 60), QPointF(160, 140));
        tools->switchToolRequested("KritaShape/KisToolLine");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolLine"));
        drag(line, QPointF(200, 80), QPointF(330, 220));
        image->waitForDone();
        QTest::qWait(50);
        QCOMPARE(shapeIds(edited), QStringList({"VGeomCurveShape", "VGeomCurveShape"}));

        tools->switchToolRequested("PathTool");
        QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
        view->canvasBase()->shapeManager();
        shapeSelection = view->canvasBase()->selectedShapesProxy()->selection();
        Q_FOREACH (KoShape *shape, edited->shapes()) {
            VCurveShape *target = dynamic_cast<VCurveShape*>(shape);
            QVERIFY(target);
            shapeSelection->deselectAll();
            shapeSelection->select(target);
            QTest::qWait(20);
            const QPointF grab = target->absoluteTransformation().map(target->curves().positions[0]);

            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, grab);
            pointEdit->mousePressEvent(&begin);
            QPointF at = grab;
            for (int step = 1; step <= 120; step++) {
                at = grab + QPointF(40 * std::sin(step * 0.15), 0.5 * step);
                QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent moved(&move, at);
                pointEdit->mouseMoveEvent(&moved);
                if (step % 4 == 0) QTest::qWait(step % 12 == 0 ? 12 : 1);
                else qApp->processEvents();
            }
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, at);
            pointEdit->mouseReleaseEvent(&end);
            QVERIFY(target->curves().positions[0] != grab);
        }
        image->waitForDone();
        QTest::qWait(200);
        image->waitForDone();

        const QRect bounds = image->bounds();
        const QImage painted = edited->projection()->convertToQImage(nullptr, bounds);
        // the whole layer, from the shapes as they are now
        edited->shapeManager()->update(QRectF(QPointF(), QSizeF(image->width() / image->xRes(), image->height() / image->yRes())));
        image->waitForDone();
        QTest::qWait(100);
        image->waitForDone();
        const QImage repainted = edited->projection()->convertToQImage(nullptr, bounds);
        int leftover = 0;
        QRect where;
        for (int y = 0; y < bounds.height(); y++) {
            for (int x = 0; x < bounds.width(); x++) {
                if (qAlpha(painted.pixel(x, y)) > 24 && qAlpha(repainted.pixel(x, y)) == 0) {
                    leftover++;
                    where |= QRect(x, y, 1, 1);
                }
            }
        }
        QVERIFY2(leftover == 0, qPrintable(QString("%1 pixels left behind in %2,%3 %4x%5")
                                               .arg(leftover).arg(where.x()).arg(where.y())
                                               .arg(where.width()).arg(where.height())));
        shapeSelection->deselectAll();
    }

    // Edit Shapes picks the object whose line is under the cursor, not its
    // bounding box; drags on empty canvas select points in a rectangle,
    // ellipse, lasso or polygon
    {
        KisShapeLayerSP picked = new KisShapeLayer(doc->shapeController(), image, "picked", OPACITY_OPAQUE_U8);
        picked->setNodeProperty("freehandvector", true);
        image->addNode(picked);
        image->waitForDone();
        nodes->slotUiActivatedNode(picked);
        QTest::qWait(50);
        tools->switchToolRequested("KritaShape/KisToolRectangle");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolRectangle"));
        drag(rectangle, QPointF(60, 60), QPointF(200, 200));
        // on top, and its bounding box covers the box
        tools->switchToolRequested("KritaShape/KisToolLine");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolLine"));
        drag(line, QPointF(50, 50), QPointF(250, 250));
        image->waitForDone();
        QTest::qWait(50);
        QCOMPARE(picked->shapes().size(), 2);
        VCurveShape *box = dynamic_cast<VCurveShape*>(picked->shapes()[0]);
        VCurveShape *diagonal = dynamic_cast<VCurveShape*>(picked->shapes()[1]);
        QVERIFY(box && diagonal);
        QVERIFY(diagonal->zIndex() > box->zIndex());

        tools->switchToolRequested("PathTool");
        QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
        view->canvasBase()->shapeManager();
        shapeSelection = view->canvasBase()->selectedShapesProxy()->selection();

        auto click = [&] (const QPointF &at, QEvent::Type type = QEvent::MouseButtonPress) {
            QMouseEvent press(type, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, at);
            if (type == QEvent::MouseButtonDblClick) pointEdit->mouseDoubleClickEvent(&begin);
            else pointEdit->mousePressEvent(&begin);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, at);
            pointEdit->mouseReleaseEvent(&end);
        };
        auto dragThrough = [&] (const QVector<QPointF> &points) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, points.first());
            pointEdit->mousePressEvent(&begin);
            for (int i = 1; i < points.size(); i++) {
                QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent moved(&move, points[i]);
                pointEdit->mouseMoveEvent(&moved);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, points.last());
            pointEdit->mouseReleaseEvent(&end);
        };

        // the right edge of the box, inside the line's bounding box
        shapeSelection->deselectAll();
        click(QPointF(200, 100));
        QVERIFY(shapeSelection->isSelected(box));
        QVERIFY(!shapeSelection->isSelected(diagonal));
        // inside the line's bounding box, off the line: nothing
        shapeSelection->deselectAll();
        click(QPointF(230, 100));
        QVERIFY(shapeSelection->selectedShapes().isEmpty());
        click(QPointF(225, 225));
        QVERIFY(shapeSelection->isSelected(diagonal));

        shapeSelection->deselectAll();
        shapeSelection->select(box);
        QTest::qWait(20);
        QVector<int> all;
        QVector<int> top;
        for (int i = 0; i < box->curves().pointCount(); i++) {
            all << i;
            if (box->absoluteTransformation().map(box->curves().positions[i]).y() < 100) top << i;
        }
        QVERIFY(!top.isEmpty() && top.size() < all.size());

        // corners away from the line, so no press lands on it
        dragThrough({QPointF(40, 220), QPointF(220, 40)});
        QCOMPARE(pointEdit->selectedPoints(box), all);

        pointEdit->setSelectionShape(VPointEditTool::SelectionShape::Ellipse);
        dragThrough({QPointF(40, 220), QPointF(220, 40)});
        // the box's corners lie outside the ellipse inside them
        QVERIFY(pointEdit->selectedPoints(box).isEmpty());

        pointEdit->setSelectionShape(VPointEditTool::SelectionShape::Lasso);
        dragThrough({QPointF(220, 40), QPointF(130, 40), QPointF(40, 40), QPointF(40, 90), QPointF(130, 90), QPointF(220, 90)});
        QCOMPARE(pointEdit->selectedPoints(box), top);

        pointEdit->setSelectionShape(VPointEditTool::SelectionShape::Polygon);
        click(QPointF(220, 40));
        click(QPointF(40, 40));
        click(QPointF(40, 90));
        // still open: nothing selected yet
        QVERIFY(pointEdit->selectedPoints(box).isEmpty());
        click(QPointF(220, 90));
        click(QPointF(220, 90), QEvent::MouseButtonDblClick);
        QCOMPARE(pointEdit->selectedPoints(box), top);
        // a click on the first corner closes it too; the triangle holds the
        // bottom right corner only
        click(QPointF(60, 230));
        click(QPointF(230, 230));
        click(QPointF(230, 60));
        click(QPointF(60, 230));
        QCOMPARE(pointEdit->selectedPoints(box).size(), 1);
        // Escape cancels and selects nothing
        click(QPointF(40, 220));
        click(QPointF(220, 220));
        click(QPointF(220, 40));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        pointEdit->keyPressEvent(&escape);
        QVERIFY(pointEdit->selectedPoints(box).isEmpty());
        pointEdit->setSelectionShape(VPointEditTool::SelectionShape::Rectangle);

        // the brush size shortcuts step the falloff, which no undo touches
        QAction *increase = view->canvasBase()->canvasController()->actionCollection()->action("increase_brush_size");
        QAction *decrease = view->canvasBase()->canvasController()->actionCollection()->action("decrease_brush_size");
        QVERIFY(increase && decrease);
        // A disabled action still emits on trigger(), while a key press goes
        // through the shortcut map, which skips disabled actions; the tool
        // manager disables tool actions the active tool didn't declare. (A
        // real key press would need the main window shown, which the test
        // environment's toolbar code doesn't survive.)
        QVERIFY(increase->isEnabled() && decrease->isEnabled());
        auto press = [] (QAction *action) {
            if (action->isEnabled()) action->trigger();
        };
        pointEdit->setFalloffRadius(0);
        press(increase);
        press(increase);
        QCOMPARE(pointEdit->falloffRadius(), 2.0);
        press(decrease);
        QCOMPARE(pointEdit->falloffRadius(), 1.0);
        pointEdit->setFalloffRadius(30);
        press(increase);
        QCOMPARE(pointEdit->falloffRadius(), 32.0);
        // a size change right before an edit survives undoing the edit
        const VCurveData beforeEdit = box->curves();
        click(QPointF(130, 230));  // empty canvas: clears the point selection
        const QPointF boxCorner = box->absoluteTransformation().map(beforeEdit.positions[1]);
        press(increase);
        QCOMPARE(pointEdit->falloffRadius(), 35.0);
        dragThrough({boxCorner, boxCorner + QPointF(10, -10)});
        image->waitForDone();
        QVERIFY(!(box->curves() == beforeEdit));
        doc->undoStack()->undo();
        image->waitForDone();
        QVERIFY(box->curves() == beforeEdit);
        QCOMPARE(pointEdit->falloffRadius(), 35.0);
        pointEdit->setFalloffRadius(0);
        shapeSelection->deselectAll();
    }

    // the Vector Edit tool: a button next to Edit Shapes, enabled on freehand layers only
    {
        QToolButton *button = window->findChild<QToolButton*>("VEditTool");
        QToolButton *editShapes = window->findChild<QToolButton*>("PathTool");
        QVERIFY(button && editShapes);
        QCOMPARE(button->parentWidget(), editShapes->parentWidget());
        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_VERIFY(!button->isEnabled());
        nodes->slotUiActivatedNode(vector);
        QTRY_VERIFY(button->isEnabled());

        // each mode's shortcut activates the tool in that mode, from any tool
        KisKActionCollection *actions = view->canvasBase()->canvasController()->actionCollection();
        QAction *trimMode = actions->action("VEditToolTrim");
        QAction *subdivideMode = actions->action("VEditToolSubdivide");
        QAction *thicknessMode = actions->action("VEditToolThickness");
        QVERIFY(trimMode && subdivideMode && thicknessMode);
        QVERIFY(trimMode->isEnabled());
        trimMode->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("VEditTool"));
        VEditTool *edit = dynamic_cast<VEditTool*>(tools->toolById(view->canvasBase(), "VEditTool"));
        QVERIFY(edit);
        QCOMPARE(edit->mode(), VEditTool::Mode::Trim);
        QVERIFY(subdivideMode->isEnabled());
        subdivideMode->trigger();
        QCOMPARE(edit->mode(), VEditTool::Mode::Subdivide);

        // the brush size shortcuts size the brush
        QAction *increase = actions->action("increase_brush_size");
        QVERIFY(increase && increase->isEnabled());
        edit->setBrushSize(30);
        increase->trigger();
        QVERIFY(edit->settings().size > 30);

        // Apply on the selected triangle subdivides it, as one undo step
        VCurveShape *triangle = dynamic_cast<VCurveShape*>(vector->shapes().first());
        QVERIFY(triangle);
        const VCurveData before = triangle->curves();
        KoSelection *selection = view->canvasBase()->selectedShapesProxy()->selection();
        selection->deselectAll();
        selection->select(triangle);
        // (Apply moved to Edit Shapes; Vector Edit stays the tool under test after it)
        tools->switchToolRequested("VPointEditTool");
        QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
        VPointEditTool *points = dynamic_cast<VPointEditTool*>(tools->toolById(view->canvasBase(), "VPointEditTool"));
        QVERIFY(points);
        points->setActsOnWholeLayer(false);
        points->actionSettings().subdivideSpacing = 5;
        points->applyToSelected(VEditActions::Whole::Subdivide);
        image->waitForDone();
        tools->switchToolRequested("VEditTool");
        QTRY_COMPARE(tools->activeToolId(), QString("VEditTool"));
        QVERIFY(triangle->curves().pointCount() > before.pointCount());
        doc->undoStack()->undo();
        image->waitForDone();
        QVERIFY(triangle->curves() == before);
        selection->deselectAll();

        // the Thickness brush: Thicken widens the line under it only
        thicknessMode->trigger();
        QCOMPARE(edit->mode(), VEditTool::Mode::Thickness);
        edit->setBrushSize(10);
        edit->settings().thicknessTool = VEditTool::ThicknessTool::Thicken;
        const QPointF at = triangle->absoluteTransformation().map(before.positions[0]);
        // a click; Ctrl+click arrives as a press with Ctrl (KisAlternateInvocationAction)
        auto dabAt = [&] (const QPointF &point, Qt::KeyboardModifiers modifiers) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, modifiers);
            KoPointerEvent begin(&press, point);
            edit->mousePressEvent(&begin);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, point);
            edit->mouseReleaseEvent(&end);
            image->waitForDone();
        };
        dabAt(at, Qt::NoModifier);
        QVERIFY(triangle->curves().radii[0] > before.radii[0]);
        for (int i = 1; i < before.pointCount(); i++) {
            if (QLineF(triangle->absoluteTransformation().map(before.positions[i]), at).length() > 5) {
                QCOMPARE(triangle->curves().radii[i], before.radii[i]);
            }
        }
        QCOMPARE(doc->undoStack()->undoText(), QString("Thicken Strokes"));
        doc->undoStack()->undo();
        image->waitForDone();
        QVERIFY(triangle->curves() == before);

        // Ctrl swaps Thicken and Thin
        dabAt(at, Qt::ControlModifier);
        QVERIFY(triangle->curves().radii[0] < before.radii[0]);
        QCOMPARE(doc->undoStack()->undoText(), QString("Thin Strokes"));
        doc->undoStack()->undo();
        edit->settings().thicknessTool = VEditTool::ThicknessTool::Thin;
        dabAt(at, Qt::ControlModifier);
        QVERIFY(triangle->curves().radii[0] > before.radii[0]);
        doc->undoStack()->undo();
        image->waitForDone();
        QVERIFY(triangle->curves() == before);

        // Set: at full strength with a flat falloff, one dab sets the radius
        {
            VEditTool::Settings saved = edit->settings();
            edit->settings().thicknessTool = VEditTool::ThicknessTool::Set;
            edit->settings().setRadius = 3.5;
            edit->settings().strength = 1.0;
            edit->settings().profile = VPointEdit::Profile::Constant;
            dabAt(at, Qt::ControlModifier);  // Ctrl leaves Set alone
            const qreal scale = std::sqrt(std::abs(triangle->absoluteTransformation().determinant()));
            QVERIFY(qAbs(triangle->curves().radii[0] * scale - 3.5) < 1e-9);
            QCOMPARE(doc->undoStack()->undoText(), QString("Set Stroke Radius"));
            doc->undoStack()->undo();
            image->waitForDone();
            QVERIFY(triangle->curves() == before);
            edit->settings() = saved;
        }

        // the Opacity brush: Decrease, Ctrl for Increase, Set
        {
            VEditTool::Settings saved = edit->settings();
            QAction *opacityMode = actions->action("VEditToolOpacity");
            QVERIFY(opacityMode && opacityMode->isEnabled());
            opacityMode->trigger();
            QCOMPARE(edit->mode(), VEditTool::Mode::Opacity);
            edit->settings().opacityTool = VEditTool::OpacityTool::Decrease;
            auto opacityAt = [&] (int i) { return triangle->curves().attribute(VCurveOpacity::Attribute, i, 1.0); };
            dabAt(at, Qt::NoModifier);
            const qreal decreased = opacityAt(0);
            QVERIFY(decreased < 1.0);
            QCOMPARE(doc->undoStack()->undoText(), QString("Decrease Stroke Opacity/Density"));
            dabAt(at, Qt::ControlModifier);
            QVERIFY(opacityAt(0) > decreased);
            QCOMPARE(doc->undoStack()->undoText(), QString("Increase Stroke Opacity/Density"));
            doc->undoStack()->undo();
            doc->undoStack()->undo();
            image->waitForDone();
            QVERIFY(triangle->curves() == before);

            edit->settings().opacityTool = VEditTool::OpacityTool::Set;
            edit->settings().setOpacity = 0.25;
            edit->settings().strength = 1.0;
            edit->settings().profile = VPointEdit::Profile::Constant;
            dabAt(at, Qt::NoModifier);
            QVERIFY(qAbs(opacityAt(0) - 0.25) < 1e-9);
            QCOMPARE(doc->undoStack()->undoText(), QString("Set Stroke Opacity/Density"));
            doc->undoStack()->undo();
            image->waitForDone();
            QVERIFY(triangle->curves() == before);
            edit->settings() = saved;
        }

        // brush strokes: Set keeps the pressure taper (the radius at full pressure), and
        // Density changes the dabs, not the opacity mask
        {
            VEditTool::Settings saved = edit->settings();
            VStrokeData data;
            data.preset = preset;
            data.brushSize = 6;
            const qreal ppp = image->xRes();
            const QPointF origin = at + QPointF(10, 10);
            QVector<VStrokePrimitive> line;
            KisPaintInformation prev(origin * ppp, 0.05);
            for (int i = 1; i <= 6; i++) {
                KisPaintInformation next((origin + QPointF(4 * i, 0)) * ppp, 0.05 + 0.15 * i);
                line << VStrokePrimitive::line(prev, next);
                prev = next;
            }
            VStrokeShape *brushStroke = VStrokeShape::fromImagePixels(line, ppp, data);
            vector->addShape(brushStroke);
            brushStroke->waitForRendering();
            image->waitForDone();
            const VCurveData strokeBefore = brushStroke->curves();
            const QPointF middle = origin + QPointF(12, 0);

            thicknessMode->trigger();
            edit->setBrushSize(200);
            edit->settings().thicknessTool = VEditTool::ThicknessTool::Set;
            edit->settings().setRadius = 3.0;
            edit->settings().strength = 1.0;
            edit->settings().profile = VPointEdit::Profile::Constant;
            dabAt(middle, Qt::NoModifier);
            const VCurveData set = brushStroke->curves();
            for (int i = 0; i < set.pointCount(); i++) {
                // radius in image pixels, the setting in points
                const qreal pressure = set.attribute(QStringLiteral("pressure"), i, 1.0);
                QVERIFY2(qAbs(set.radii[i] / brushStroke->resolution() - 3.0 * pressure) < 1e-9,
                         qPrintable(QString("%1: %2").arg(i).arg(set.radii[i] / brushStroke->resolution())));
                // so the size multiplier is the same along the stroke: no big light dabs at the tips
                QVERIFY(qAbs(VStrokeCurves::paintInfo(set, i, data.brushSize).sizeMultiplier() -
                             3.0 * brushStroke->resolution() / (0.5 * data.brushSize)) < 1e-9);
            }
            doc->undoStack()->undo();
            image->waitForDone();
            QVERIFY(brushStroke->curves() == strokeBefore);

            QAction *opacityMode = actions->action("VEditToolOpacity");
            opacityMode->trigger();
            // each mode has its own brush size
            edit->setBrushSize(200);
            edit->settings().opacityTool = VEditTool::OpacityTool::Set;
            edit->settings().setOpacity = 1.0;
            edit->settings().affectOpacity = false;
            edit->settings().affectDensity = true;
            dabAt(middle, Qt::NoModifier);
            const VCurveData dense = brushStroke->curves();
            QVERIFY(!dense.attributes.contains(VCurveOpacity::Attribute));
            for (int i = 0; i < dense.pointCount(); i++) {
                QCOMPARE(dense.attribute(VStrokeCurves::DensityScale, i, 1.0), 0.0);
                QCOMPARE(dense.attribute(VStrokeCurves::DensityOffset, i, 0.0), 1.0);
                // whatever the brush made of the light pressure, the dabs are solid
                QCOMPARE(VStrokeCurves::paintInfo(dense, i, data.brushSize).applyDensity(0.05), 1.0);
            }
            QCOMPARE(doc->undoStack()->undoText(), QString("Set Stroke Opacity/Density"));
            // drawn shapes have no dabs: the triangle under the brush is left alone
            QVERIFY(triangle->curves() == before);
            doc->undoStack()->undo();
            image->waitForDone();
            QVERIFY(brushStroke->curves() == strokeBefore);

            // Decrease on both: the dabs scale, the mask fades
            edit->settings().opacityTool = VEditTool::OpacityTool::Decrease;
            edit->settings().affectOpacity = true;
            dabAt(middle, Qt::NoModifier);
            const VCurveData lighter = brushStroke->curves();
            QVERIFY(qAbs(lighter.attribute(VStrokeCurves::DensityScale, 3, 1.0) - 0.9) < 1e-9);
            QCOMPARE(lighter.attribute(VStrokeCurves::DensityOffset, 3, 0.0), 0.0);
            QVERIFY(qAbs(lighter.attribute(VCurveOpacity::Attribute, 3, 1.0) - 0.9) < 1e-9);
            doc->undoStack()->undo();
            image->waitForDone();

            // through the undo stack, which still has edits of it
            view->canvasBase()->addCommand(view->canvasBase()->shapeController()->removeShape(brushStroke));
            image->waitForDone();
            QTest::qWait(50);
            edit->settings() = saved;
            edit->setBrushSize(saved.size);
        }

        // Simplify's merge across two shapes: one shape goes, undo brings it back
        {
            const int shapesBefore = vector->shapes().size();
            paste(freehandCurveSvg(), viewManager);
            paste(freehandCurveSvg(), viewManager);
            QCOMPARE(vector->shapes().size(), shapesBefore + 2);
            KoShape *first = vector->shapes()[shapesBefore];
            KoShape *second = vector->shapes()[shapesBefore + 1];
            tools->switchToolRequested("VEditTool");
            QTRY_COMPARE(tools->activeToolId(), QString("VEditTool"));
            edit->setMode(VEditTool::Mode::Simplify);
            VPointEditTool *actions = dynamic_cast<VPointEditTool*>(tools->toolById(view->canvasBase(), "VPointEditTool"));
            QVERIFY(actions);
            actions->setActsOnWholeLayer(false);
            actions->actionSettings().mergeOverlapping = true;
            actions->actionSettings().mergeDistance = 4;
            selection->deselectAll();
            selection->select(first);
            selection->select(second);
            tools->switchToolRequested("VPointEditTool");
            QTRY_COMPARE(tools->activeToolId(), QString("VPointEditTool"));
            actions->applyToSelected(VEditActions::Whole::Simplify);
            image->waitForDone();
            tools->switchToolRequested("VEditTool");
            QTRY_COMPARE(tools->activeToolId(), QString("VEditTool"));
            QTest::qWait(50);
            QCOMPARE(vector->shapes().size(), shapesBefore + 1);
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            QCOMPARE(vector->shapes().size(), shapesBefore + 2);
            QVERIFY(vector->shapes().contains(first) && vector->shapes().contains(second));
            actions->actionSettings().mergeOverlapping = false;
            doc->undoStack()->undo();
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            QCOMPARE(vector->shapes().size(), shapesBefore);
            selection->deselectAll();
        }

        // off freehand layers it gives way, as the point editor does
        nodes->slotUiActivatedNode(raster);
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        QTRY_VERIFY(!button->isEnabled());
    }

    // Liquify (Move) on a raster and a freehand layer together, through the transform tool
    {
        KisPaintDeviceSP pixels = raster->paintDevice();
        pixels->fill(QRect(40, 40, 120, 120), KoColor(Qt::red, pixels->colorSpace()));
        image->waitForDone();
        VCurveShape *triangle = dynamic_cast<VCurveShape*>(vector->shapes().first());
        QVERIFY(triangle);
        const VCurveData original = triangle->curves();
        const QPointF corner = triangle->absoluteTransformation().map(original.positions[1]);
        const QStringList plainBefore = shapeIds(plain);
        // a brush stroke just right of the corner (and of the stock triangle above it)
        VStrokeShape *brushStroke = nullptr;
        {
            VStrokeData data;
            data.preset = preset;
            data.brushSize = 6;
            const qreal ppp = image->xRes();
            QVector<VStrokePrimitive> line;
            KisPaintInformation prev((corner + QPointF(12, 8)) * ppp, 1.0);
            for (int i = 1; i <= 6; i++) {
                KisPaintInformation next((corner + QPointF(12 + 10 * i, 8)) * ppp, 1.0);
                line << VStrokePrimitive::line(prev, next);
                prev = next;
            }
            brushStroke = VStrokeShape::fromImagePixels(line, ppp, data);
            brushStroke->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
            vector->addShape(brushStroke);
            for (int i = 0; i < 10; i++) {
                brushStroke->waitForRendering();
                QTest::qWait(20);
            }
            image->waitForDone();
        }
        const VCurveData brushOriginal = brushStroke->curves();

        auto pixelSum = [pixels] () {
            const QImage img = pixels->convertToQImage(0, QRect(0, 0, 400, 300));
            qint64 sum = 0;
            for (int y = 0; y < img.height(); y++) {
                const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                for (int x = 0; x < img.width(); x++) sum += qAlpha(line[x]) * (x + 3 * y);
            }
            return sum;
        };
        const qint64 rasterBefore = pixelSum();
        // image->waitForDone() would ask the open transform stroke to end
        auto settle = [] () {
            for (int i = 0; i < 15; i++) QTest::qWait(20);
        };
        auto findButton = [] (KisTool *tool, const char *name) -> QAbstractButton* {
            Q_FOREACH (QPointer<QWidget> widget, tool->optionWidgets()) {
                if (QAbstractButton *button = widget ? widget->findChild<QAbstractButton*>(name) : nullptr) return button;
            }
            return nullptr;
        };
        // the size is saved in the config: set it for each stroke
        auto setLiquifySize = [&] (KisTool *transform, qreal value) {
            QWidget *size = nullptr;
            Q_FOREACH (QPointer<QWidget> widget, transform->optionWidgets()) {
                if (!size && widget) size = widget->findChild<QWidget*>("liquifySizeSlider");
            }
            if (!size || !QMetaObject::invokeMethod(size, "setValue", Q_ARG(double, value))) return false;
            settle();
            return true;
        };
        auto liquifyDrag = [&] (KisTool *transform, QPointF from = QPointF()) {
            // a Move drag, by default just below the triangle's corner, 15 px above the red square
            if (from.isNull()) from = corner + QPointF(-30, 15);
            // the stroke starts asynchronously: let it take the drag
            settle();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, from);
            transform->beginPrimaryAction(&begin);
            for (int i = 1; i <= 10; i++) {
                QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                KoPointerEvent moved(&move, from + QPointF(6 * i, 0));
                transform->continuePrimaryAction(&moved);
                QTest::qWait(5);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, from + QPointF(60, 0));
            transform->endPrimaryAction(&end);
            // Liquify sends its changes to the image when the canvas repaints the
            // tool, which the hidden window never does
            QImage canvasImage(400, 300, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&canvasImage);
            transform->paint(painter, *view->canvasBase()->viewConverter());
            painter.end();
            settle();
        };

        QAction *liquifyAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformLiquify");
        QVERIFY(liquifyAction);
        image->setLodPreferences(KisLodPreferences(2));
        nodes->slotUiActivatedNode(vector);
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(raster), KisNodeSP(vector), KisNodeSP(plain)});
        liquifyAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QVERIFY(transform);
        QAbstractButton *liquifyMode = findButton(transform, "liquifyButton");
        QAbstractButton *scaleMode = findButton(transform, "liquifyScale");
        QAbstractButton *moveMode = findButton(transform, "liquifyMove");
        QVERIFY(liquifyMode && scaleMode && moveMode);
        QTRY_VERIFY(liquifyMode->isChecked());

        // with a freehand layer in it, the other modes are greyed out
        QVERIFY(!scaleMode->isEnabled());
        QVERIFY(moveMode->isEnabled() && moveMode->isChecked());

        // the preview moves the curves themselves, without adding points; in-tool undo takes it back
        liquifyDrag(transform);
        QTRY_VERIFY(triangle->curves() != original);
        QCOMPARE(triangle->curves().pointCount(), original.pointCount());
        transform->requestUndoDuringStroke();
        settle();
        QTRY_VERIFY(triangle->curves() == original);

        // a second drag, then cancel
        liquifyDrag(transform);
        QTRY_VERIFY(triangle->curves() != original);
        transform->requestStrokeCancellation();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(triangle->curves() == original);
        QCOMPARE(pixelSum(), rasterBefore);

        // a new stroke, applied: both layers keep it, and one undo takes both back
        {
            // after a cancel, the next press starts the stroke
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent begin(&press, QPointF(390, 290));
            transform->beginPrimaryAction(&begin);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            KoPointerEvent end(&release, QPointF(390, 290));
            transform->endPrimaryAction(&end);
        }
        settle();
        QTRY_VERIFY(liquifyMode->isChecked());
        QTRY_VERIFY(!scaleMode->isEnabled());
        liquifyDrag(transform);
        QTRY_VERIFY(triangle->curves() != original);
        const VCurveData preview = triangle->curves();
        transform->requestStrokeEnd();
        image->waitForDone();
        QTest::qWait(50);
        image->waitForDone();
        QVERIFY(triangle->curves() == preview);
        QVERIFY(pixelSum() != rasterBefore);
        // the layer shows the moved curves without another repaint
        {
            const QRect moved = triangle->boundingRect().toAlignedRect() & image->bounds();
            QTRY_VERIFY(coveredPixels(vector, moved) > moved.width() * moved.height() / 10);
        }
        // ... and the moved brush stroke, in the merged image too
        QVERIFY(brushStroke->curves() != brushOriginal);
        {
            for (int i = 0; i < 10; i++) {
                brushStroke->waitForRendering();
                QTest::qWait(20);
            }
            image->waitForDone();
            const QRect area = QRectF(brushStroke->boundingRect().topLeft() * image->xRes(),
                                      brushStroke->boundingRect().bottomRight() * image->xRes()).toAlignedRect() & image->bounds();
            QTRY_VERIFY(bluePixels(vector, area) > 50);
            QTRY_VERIFY(bluePixels(image->root(), area) >= bluePixels(vector, area) * 8 / 10);
        }
        // the stock vector layer is left out
        QCOMPARE(shapeIds(plain), plainBefore);
        doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(triangle->curves() == original);
        QCOMPARE(pixelSum(), rasterBefore);
        doc->undoStack()->redo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(triangle->curves() == preview);

        // a later Liquify stroke on a different selection of layers (not a
        // continuation of the applied one) leaves the applied one alone
        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        const int undoSteps = doc->undoStack()->count();
        nodes->slotUiActivatedNode(vector);
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(vector)});
        liquifyAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        settle();
        QTRY_VERIFY(liquifyMode->isChecked());
        const VCurveData brushBefore = brushStroke->curves();
        // a small brush over the brush stroke's far end, away from the triangle
        QVERIFY(setLiquifySize(transform, 10.0));
        liquifyDrag(transform, (corner + QPointF(70, 8)) * image->xRes());
        QVERIFY(brushStroke->curves() != brushBefore);
        QVERIFY(triangle->curves() == preview);
        transform->requestStrokeEnd();
        image->waitForDone();
        QTest::qWait(50);
        image->waitForDone();
        QVERIFY(triangle->curves() == preview);
        QCOMPARE(doc->undoStack()->count(), undoSteps + 1);
        // undone on its own
        doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(triangle->curves() == preview);
        doc->undoStack()->undo();
        image->waitForDone();
        QTest::qWait(50);
        QVERIFY(triangle->curves() == original);
        QVERIFY(brushStroke->curves() == brushOriginal);

        // with the Fast preview chosen, Liquify still moves the curves (the
        // overlay preview can't), and the layer shows without another repaint
        {
            KConfigGroup group = KSharedConfig::openConfig()->group("KisToolTransform");
            const bool savedOverlay = group.readEntry("useOverlayPreviewStyle", false);
            group.writeEntry("useOverlayPreviewStyle", true);
            QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
            nodes->slotUiActivatedNode(raster);
            tools->switchToolRequested("KritaShape/KisToolBrush");
            QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
            image->waitForDone();
            nodes->slotUiActivatedNode(vector);
            nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(vector)});
            liquifyAction->trigger();
            QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
            settle();
            QTRY_VERIFY(liquifyMode->isChecked());
            liquifyDrag(transform, (corner + QPointF(70, 8)) * image->xRes());
            QVERIFY(brushStroke->curves() != brushOriginal);
            transform->requestStrokeEnd();
            image->waitForDone();
            QTest::qWait(50);
            image->waitForDone();
            QVERIFY(brushStroke->curves() != brushOriginal);
            for (int i = 0; i < 10; i++) {
                brushStroke->waitForRendering();
                QTest::qWait(20);
            }
            image->waitForDone();
            const QRect area = QRectF(brushStroke->boundingRect().topLeft() * image->xRes(),
                                      brushStroke->boundingRect().bottomRight() * image->xRes()).toAlignedRect() & image->bounds();
            QTRY_VERIFY(bluePixels(vector, area) > 50);
            QTRY_VERIFY(bluePixels(image->root(), area) >= bluePixels(vector, area) * 8 / 10);
            doc->undoStack()->undo();
            image->waitForDone();
            QTest::qWait(50);
            QVERIFY(brushStroke->curves() == brushOriginal);
            group.writeEntry("useOverlayPreviewStyle", savedOverlay);
            QVERIFY(QMetaObject::invokeMethod(transform, "slotGlobalConfigChanged"));
        }
        image->setLodPreferences(KisLodPreferences());

        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
    }

    // Free transform of a freehand layer with a brush stroke: applied, the
    // layer shows the stroke at its new place without another repaint; with
    // and without the reduced-resolution preview the OpenGL canvas uses
    for (int lod : {0, 2}) {
        image->setLodPreferences(KisLodPreferences(lod));
        KisShapeLayerSP strokes = new KisShapeLayer(doc->shapeController(), image, "strokes", OPACITY_OPAQUE_U8);
        strokes->setNodeProperty("freehandvector", true);
        image->addNode(strokes);
        image->waitForDone();

        VStrokeData data;
        data.preset = preset;
        data.brushSize = 10;
        QVector<VStrokePrimitive> line;
        KisPaintInformation prev(QPointF(60, 60), 1.0);
        for (int i = 1; i <= 8; i++) {
            KisPaintInformation next(QPointF(60 + 15 * i, 60 + (i % 2) * 10), 1.0);
            line << VStrokePrimitive::line(prev, next);
            prev = next;
        }
        VStrokeShape *stroke = VStrokeShape::fromImagePixels(line, image->xRes(), data);
        stroke->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(Qt::blue)));
        strokes->addShape(stroke);
        strokes->setDirty();
        auto settleRenders = [&] () {
            for (int i = 0; i < 10; i++) {
                stroke->waitForRendering();
                QTest::qWait(20);
            }
        };
        settleRenders();
        image->waitForDone();
        const QRect before(55, 50, 130, 30);
        QTRY_VERIFY(coveredPixels(strokes, before) > 300);

        QAction *freeAction = view->canvasBase()->canvasController()->actionCollection()->action("KisToolTransformFree");
        QVERIFY(freeAction);
        nodes->slotUiActivatedNode(strokes);
        nodes->slotSetSelectedNodes(KisNodeList{KisNodeSP(strokes)});
        freeAction->trigger();
        QTRY_COMPARE(tools->activeToolId(), QString("KisToolTransform"));
        KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(view->canvasBase(), "KisToolTransform"));
        QVERIFY(transform);
        for (int i = 0; i < 15; i++) QTest::qWait(20);

        // a drag inside the box moves the layer 100 px down
        const QPointF from(120, 65);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        KoPointerEvent begin(&press, from);
        transform->beginPrimaryAction(&begin);
        for (int i = 1; i <= 10; i++) {
            QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            KoPointerEvent moved(&move, from + QPointF(0, 10 * i));
            transform->continuePrimaryAction(&moved);
            QTest::qWait(5);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        KoPointerEvent end(&release, from + QPointF(0, 100));
        transform->endPrimaryAction(&end);
        for (int i = 0; i < 15; i++) QTest::qWait(20);

        transform->requestStrokeEnd();
        image->waitForDone();
        settleRenders();
        image->waitForDone();
        QTest::qWait(100);
        image->waitForDone();
        const QRect after = before.translated(0, 100);
        QTRY_VERIFY(coveredPixels(strokes, after) > 300);
        QTRY_VERIFY(bluePixels(image->root(), after) > 300);
        QCOMPARE(coveredPixels(strokes, before), 0);

        nodes->slotUiActivatedNode(raster);
        tools->switchToolRequested("KritaShape/KisToolBrush");
        QTRY_COMPARE(tools->activeToolId(), QString("KritaShape/KisToolBrush"));
        image->waitForDone();
        image->setLodPreferences(KisLodPreferences());
        strokes->setVisible(false);
        image->waitForDone();
    }

    // the layer actions, from the installed view plugin
    KisAction *add = viewManager->actionManager()->actionByName("add_new_freehand_vector_layer");
    KisAction *convert = viewManager->actionManager()->actionByName("convert_to_freehand_vector_layer");
    QVERIFY(add && convert);

    nodes->slotUiActivatedNode(raster);
    // the new layer gets its flag and name before it is in the image
    s_safeAsserts.clear();
    s_previousHandler = qInstallMessageHandler(catchSafeAsserts);
    add->trigger();
    image->waitForDone();
    QTest::qWait(50);
    qInstallMessageHandler(s_previousHandler);
    QVERIFY2(s_safeAsserts.isEmpty(), qPrintable(s_safeAsserts.join("; ")));
    QTRY_VERIFY(viewManager->activeNode() != KisNodeSP(raster));
    KisNodeSP added = viewManager->activeNode();
    QVERIFY(added->inherits("KisShapeLayer"));
    QVERIFY(isFreehand(added));
    QVERIFY(added->name().startsWith("Freehand"));

    nodes->slotUiActivatedNode(plain);
    QTRY_VERIFY(convert->isEnabled());
    convert->trigger();
    image->waitForDone();
    QTest::qWait(50);
    QVERIFY(isFreehand(plain));
    QCOMPARE(shapeIds(plain), QStringList({"VGeomCurveShape"}));
    doc->undoStack()->undo();
    image->waitForDone();
    QTest::qWait(50);
    QVERIFY(!isFreehand(plain));
    QCOMPARE(shapeIds(plain), QStringList({"KoPathShape"}));

    // tear down while QApplication exists, not in KisPart's global destructor
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VStrokeToolSwitchTest)

#include "VStrokeToolSwitchTest.moc"
