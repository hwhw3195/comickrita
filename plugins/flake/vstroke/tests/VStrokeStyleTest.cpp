/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QApplication>
#include <QAbstractButton>
#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QDoubleSpinBox>
#include <QMimeData>
#include <QPainter>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <kistest.h>
#include <kis_pointer_utils.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <KoCanvasBase.h>
#include <KoToolBase.h>
#include <kis_canvas2.h>
#include <KoColorBackground.h>
#include <KoColorPopupAction.h>
#include <KoColorSpaceRegistry.h>
#include <KoFillConfigWidget.h>
#include <KoFlakeUtils.h>
#include <KoPathShape.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeStroke.h>
#include <KoShapeStrokeCommand.h>
#include <KoStrokeConfigWidget.h>
#include <KoToolManager.h>
#include <SvgWriter.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_shape_controller.h>
#include <kis_shape_layer.h>
#include <kis_simple_stroke_strategy.h>
#include <kundo2command.h>
#include <KisGlobalResourcesInterface.h>

#include "VCurveConverter.h"
#include "VGeomCurveShape.h"
#include "VStrokeShape.h"
#include "VWindowTestUtils.h"

/**
 * The Stroke tab of the shape select tool on freehand vector layers: a brush
 * stroke shows and takes its paint color and thickness, with no line drawn
 * around it, and the options a shape doesn't follow are greyed out.
 */
class VStrokeStyleTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testStrokeTab();
};

namespace {

QStringList s_compressorAsserts;
QtMessageHandler s_previousHandler = nullptr;
void catchCompressorAsserts(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains("SAFE ASSERT") && message.contains("m_sanityIsStarting")) {
        s_compressorAsserts << message;
    }
    if (s_previousHandler) s_previousHandler(type, context, message);
}

VStrokeShape *brushStroke(KisImageSP image, KisPaintOpPresetSP preset, qreal y, qreal pressure0, qreal pressure1, const QColor &color)
{
    VStrokeData settings;
    settings.preset = preset;
    settings.brushSize = 20;
    KisPaintInformation a(QPointF(40, y), pressure0);
    KisPaintInformation b(QPointF(300, y), pressure1);
    VStrokeShape *shape = VStrokeShape::fromImagePixels({VStrokePrimitive::line(a, b)}, image->xRes(), settings);
    shape->setBackground(toQShared(new KoColorBackground(color)));
    return shape;
}

qreal strokeWidth(KoShape *shape)
{
    const QSharedPointer<KoShapeStroke> stroke = qSharedPointerDynamicCast<KoShapeStroke>(shape->stroke());
    return stroke ? stroke->lineWidth() : -1.0;
}

/// pixels of \p node's projection close in hue to \p color
int colorPixels(KisNodeSP node, const QRect &rect, const QColor &color)
{
    const QImage img = node->projection()->convertToQImage(0, rect);
    int count = 0;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) {
            if (qAlpha(line[x]) < 200) continue;
            count += qAbs(qRed(line[x]) - color.red()) + qAbs(qGreen(line[x]) - color.green()) + qAbs(qBlue(line[x]) - color.blue()) < 30;
        }
    }
    return count;
}

KoColorPopupAction *colorAction(KoFillConfigWidget *fill)
{
    return fill->findChild<KoColorPopupAction*>();
}

using VWindowTest::settle;

/// keeps the image from being idle for a while
class SlowStroke : public KisSimpleStrokeStrategy
{
public:
    SlowStroke(int ms)
        : KisSimpleStrokeStrategy(QLatin1String("slow"), kundo2_noi18n("slow")),
          m_ms(ms)
    {
        enableJob(JOB_DOSTROKE);
    }
    void doStrokeCallback(KisStrokeJobData *) override { QThread::msleep(m_ms); }
private:
    int m_ms;
};

}

void VStrokeStyleTest::testStrokeTab()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());
    s_previousHandler = qInstallMessageHandler(catchCompressorAsserts);

    KisImageSP image = new KisImage(0, 400, 300, KoColorSpaceRegistry::instance()->rgb8(), "style");
    KisDocument *doc = VWindowTest::addDocument(image);

    KisShapeLayerSP vector = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
    vector->setNodeProperty("freehandvector", true);
    image->addNode(vector);
    KisShapeLayerSP plain = new KisShapeLayer(doc->shapeController(), image, "plain", OPACITY_OPAQUE_U8);
    image->addNode(plain);
    image->waitForDone();

    KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"),
                                                                              KisGlobalResourcesInterface::instance());
    QVERIFY(preset);

    VStrokeShape *blue = brushStroke(image, preset, 80, 1.0, 0.25, QColor(20, 40, 200));
    VStrokeShape *green = brushStroke(image, preset, 160, 0.5, 0.5, QColor(20, 180, 40));
    vector->addShape(blue);
    vector->addShape(green);

    KoPathShape *path = new KoPathShape();
    path->setShapeId(KoPathShapeId);
    path->moveTo(QPointF(10, 10));
    path->lineTo(QPointF(100, 10));
    path->lineTo(QPointF(100, 40));
    path->setStroke(toQShared(new KoShapeStroke(4.0, Qt::red)));
    path->normalize();
    VGeomCurveShape *line = VCurveConverter::fromPath(path);
    QVERIFY(line);
    vector->addShape(line);
    plain->addShape(path);
    settle(400);
    image->waitForDone();
    settle(400);

    const QRect bounds = image->bounds();

    // what a stroke shows: its paint color, and the diameter at its thickest point
    const qreal ppp = image->xRes();
    QVERIFY(qAbs(strokeWidth(blue) - 20.0 / ppp) < 1e-6);
    QVERIFY(qAbs(strokeWidth(green) - 10.0 / ppp) < 1e-6);
    QCOMPARE(qSharedPointerDynamicCast<KoShapeStroke>(blue->stroke())->color(), QColor(20, 40, 200));

    // no line around the stroke, whatever it is given
    {
        QImage canvas(400, 300, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        blue->paintStroke(painter);
        blue->paintMarkers(painter);
        painter.end();
        QImage empty(400, 300, QImage::Format_ARGB32_Premultiplied);
        empty.fill(Qt::transparent);
        QCOMPARE(canvas, empty);
    }

    // a new color becomes the paint color; undo brings it back
    QVERIFY(colorPixels(vector, bounds, QColor(20, 40, 200)) > 100);
    const VCurveData blueCurves = blue->curves();
    {
        KUndo2Command *command = KoFlake::modifyShapesStrokes({blue}, [] (KoShapeStrokeSP stroke) {
            stroke->setLineBrush(Qt::NoBrush);
            stroke->setColor(QColor(200, 30, 30));
        });
        command->redo();
        QCOMPARE(blue->color(), QColor(200, 30, 30));
        QVERIFY(blue->curves() == blueCurves);
        settle(400);
        vector->setDirty();
        image->waitForDone();
        settle(400);
        QVERIFY(colorPixels(vector, bounds, QColor(200, 30, 30)) > 100);
        QCOMPARE(colorPixels(vector, bounds, QColor(20, 40, 200)), 0);
        command->undo();
        QCOMPARE(blue->color(), QColor(20, 40, 200));
        delete command;
    }

    // no color and transparent hide the stroke; it keeps its paint color, and undo shows it again
    for (const QColor &color : {QColor(), QColor(Qt::transparent)}) {
        KUndo2Command *command = KoFlake::modifyShapesStrokes({blue}, [color] (KoShapeStrokeSP stroke) { stroke->setColor(color); });
        command->redo();
        QCOMPARE(blue->color(), QColor(20, 40, 200));
        QVERIFY(!qSharedPointerDynamicCast<KoShapeStroke>(blue->stroke())->color().isValid());
        command->undo();
        QCOMPARE(qSharedPointerDynamicCast<KoShapeStroke>(blue->stroke())->color(), QColor(20, 40, 200));
        delete command;
    }

    // a new width scales all radii, also below a pixel; the taper stays, and undo restores it
    for (const qreal width : {3.0 * strokeWidth(blue), 0.25 / ppp}) {
        const VCurveData before = blue->curves();
        const qreal oldWidth = strokeWidth(blue);
        KUndo2Command *command = KoFlake::modifyShapesStrokes({blue}, [width] (KoShapeStrokeSP stroke) { stroke->setLineWidth(width); });
        command->redo();
        QVERIFY(qAbs(strokeWidth(blue) - width) < 1e-9);
        for (int i = 0; i < before.radii.size(); i++) {
            QVERIFY(qAbs(blue->curves().radii[i] - before.radii[i] * width / oldWidth) < 1e-9);
        }
        QVERIFY(blue->curves().positions == before.positions);
        command->undo();
        for (int i = 0; i < before.radii.size(); i++) {
            QVERIFY(qAbs(blue->curves().radii[i] - before.radii[i]) < 1e-9);
        }
        delete command;
    }

    // the real widget
    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();
    nodes->slotUiActivatedNode(vector);
    tools->switchToolRequested("InteractionTool");
    QTRY_COMPARE(tools->activeToolId(), QString("InteractionTool"));
    KoToolBase *selectTool = tools->toolById(view->canvasBase(), "InteractionTool");
    QVERIFY(selectTool);

    // saved without a line, and a line in a file has no effect
    {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        SvgWriter writer(QList<KoShape*>{green});
        writer.save(buffer, image->bounds().size());
        QString svg = QString::fromUtf8(buffer.data());
        const int at = svg.indexOf("krita:type=\"vstroke\"");
        QVERIFY(at > 0);
        const int start = svg.lastIndexOf("<path", at);
        QVERIFY(!svg.mid(start, at - start).contains(" stroke="));

        svg.insert(at, "stroke=\"#ff0000\" stroke-width=\"7\" ");
        QBuffer stale;
        stale.setData(svg.toUtf8());
        stale.open(QIODevice::ReadOnly);
        QSizeF fragment;
        const QList<KoShape*> loaded = KisShapeLayer::createShapesFromSvg(&stale, QString(), image->bounds(), 72.0 * image->xRes(),
                                                                          doc->shapeController()->resourceManager(), true, &fragment);
        QCOMPARE(loaded.size(), 1);
        VStrokeShape *reloaded = dynamic_cast<VStrokeShape*>(loaded.first());
        QVERIFY(reloaded);
        QCOMPARE(reloaded->color(), green->color());
        QVERIFY(qAbs(strokeWidth(reloaded) - strokeWidth(green)) < 1e-6);
        qDeleteAll(loaded);
    }

    KoStrokeConfigWidget *widget = nullptr;
    Q_FOREACH (QPointer<QWidget> option, selectTool->optionWidgets()) {
        if (!option) continue;
        widget = option->findChild<KoStrokeConfigWidget*>();
        if (widget) break;
    }
    QVERIFY(widget);
    QDoubleSpinBox *lineWidth = widget->findChild<QDoubleSpinBox*>("lineWidth");
    QWidget *capJoin = widget->findChild<QWidget*>("capNJoinButton");
    QWidget *lineStyle = widget->findChild<QWidget*>("lineStyle");
    QVERIFY(lineWidth && capJoin && lineStyle);
    QList<QWidget*> markers;
    Q_FOREACH (QWidget *child, widget->findChildren<QWidget*>()) {
        if (QString(child->metaObject()->className()) == "KoMarkerSelector") markers << child;
    }
    QCOMPARE(markers.size(), 3);

    KoSelection *selection = view->canvasBase()->selectedShapesProxy()->selection();
    QVERIFY(selection);
    auto select = [&] (const QList<KoShape*> &shapes) {
        selection->deselectAll();
        Q_FOREACH (KoShape *shape, shapes) selection->select(shape);
        QVERIFY(QMetaObject::invokeMethod(widget, "selectionChanged"));
    };
    auto checkEnabled = [&] (bool width, bool cap, bool style, bool marker) {
        QCOMPARE(lineWidth->isEnabled(), width);
        QCOMPARE(capJoin->isEnabled(), cap);
        QCOMPARE(lineStyle->isEnabled(), style);
        Q_FOREACH (QWidget *m, markers) QCOMPARE(m->isEnabled(), marker);
    };

    select({blue});
    checkEnabled(true, true, false, false);
    select({line});
    checkEnabled(true, true, false, false);
    select({blue, line});
    checkEnabled(true, true, false, false);

    // caps reach the poly-tool stroke in a mixed selection, and the brush stroke stays as it is
    {
        const VCurveData before = blue->curves();
        QList<QButtonGroup*> groups = widget->findChildren<QButtonGroup*>();
        QAbstractButton *roundCap = nullptr;
        Q_FOREACH (QButtonGroup *group, groups) {
            if (group->buttons().size() == 3 && group->button(Qt::RoundCap)
                && group->button(Qt::RoundCap)->toolTip().contains("cap", Qt::CaseInsensitive)) roundCap = group->button(Qt::RoundCap);
        }
        QVERIFY(roundCap);
        roundCap->click();
        image->waitForDone();
        const QSharedPointer<KoShapeStroke> lineStroke = qSharedPointerDynamicCast<KoShapeStroke>(line->stroke());
        QVERIFY(lineStroke);
        QCOMPARE(lineStroke->capStyle(), Qt::RoundCap);
        QVERIFY(blue->curves() == before);
    }

    // gradients are for fills: brush and poly-tool strokes take none, other shapes still do
    {
        KoFillConfigWidget *strokeFill = widget->findChild<KoFillConfigWidget*>();
        KoFillConfigWidget *shapeFill = nullptr;
        Q_FOREACH (QPointer<QWidget> option, selectTool->optionWidgets()) {
            if (!option) continue;
            Q_FOREACH (KoFillConfigWidget *candidate, option->findChildren<KoFillConfigWidget*>()) {
                if (candidate != strokeFill) shapeFill = candidate;
            }
        }
        QVERIFY(strokeFill && shapeFill);
        QAbstractButton *strokeGradient = strokeFill->findChild<QAbstractButton*>("btnGradientFill");
        QAbstractButton *shapeGradient = shapeFill->findChild<QAbstractButton*>("btnGradientFill");
        QVERIFY(strokeGradient && shapeGradient);
        KoPathShape *plain = new KoPathShape();
        plain->moveTo(QPointF(10, 10));
        plain->lineTo(QPointF(30, 30));
        plain->setStroke(toQShared(new KoShapeStroke(2.0, Qt::black)));
        vector->addShape(plain);
        settle(400);

        auto checkGradient = [&] (const QList<KoShape*> &shapes, bool onStroke) {
            select(shapes);
            QVERIFY(QMetaObject::invokeMethod(strokeFill, "shapeChanged"));
            QVERIFY(QMetaObject::invokeMethod(shapeFill, "shapeChanged"));
            QCOMPARE(strokeGradient->isEnabled(), onStroke);
            QVERIFY(shapeGradient->isEnabled());
        };
        strokeFill->activate();
        shapeFill->activate();
        checkGradient({blue}, false);
        checkGradient({line}, false);
        checkGradient({blue, line}, false);
        checkGradient({plain}, true);
        checkGradient({blue, plain}, false);
        selection->deselectAll();
        vector->removeShape(plain);
        delete plain;
    }

    // shows the thickness in pixels, not its minimum, and a value set on two strokes shows on both
    select({blue});
    QVERIFY2(qAbs(lineWidth->value() - 20.0) < 0.011, qPrintable(QString::number(lineWidth->value())));
    select({blue, green});
    lineWidth->setValue(0.5);
    image->waitForDone();
    QVERIFY2(qAbs(strokeWidth(blue) * ppp - 0.5) < 1e-6, qPrintable(QString::number(strokeWidth(blue) * ppp)));
    QCOMPARE(strokeWidth(blue), strokeWidth(green));
    QVERIFY(QMetaObject::invokeMethod(widget, "selectionChanged"));
    QVERIFY2(qAbs(lineWidth->value() - 0.5) < 0.011, qPrintable(QString::number(lineWidth->value())));

    // mixed widths show their median, so the arrows start near what the strokes have
    {
        auto setWidth = [&] (KoShape *shape, qreal pixels) {
            const qreal width = pixels / ppp;
            QScopedPointer<KUndo2Command> command(KoFlake::modifyShapesStrokes({shape}, [width] (KoShapeStrokeSP stroke) { stroke->setLineWidth(width); }));
            command->redo();
        };
        auto shown = [&] () { return lineWidth->value(); };
        setWidth(blue, 2);
        setWidth(green, 10);
        setWidth(line, 40);
        image->waitForDone();
        select({blue});
        QVERIFY2(qAbs(shown() - 2) < 0.011, qPrintable(QString::number(shown())));
        select({blue, green, line});
        QVERIFY2(qAbs(shown() - 10) < 0.011, qPrintable(QString::number(shown())));
        select({blue, green});
        QVERIFY2(qAbs(shown() - 6) < 0.011, qPrintable(QString::number(shown())));
        // only showing it changes nothing, and typing still sets one width, keeping each stroke's taper
        QVERIFY(qAbs(strokeWidth(blue) * ppp - 2) < 1e-6);
        QVERIFY(qAbs(strokeWidth(green) * ppp - 10) < 1e-6);
        const VCurveData blueBefore = blue->curves();
        lineWidth->setValue(8);
        image->waitForDone();
        QVERIFY(qAbs(strokeWidth(blue) * ppp - 8) < 1e-6);
        QVERIFY(qAbs(strokeWidth(green) * ppp - 8) < 1e-6);
        for (int i = 0; i < blueBefore.radii.size(); i++) {
            QVERIFY(qAbs(blue->curves().radii[i] - blueBefore.radii[i] * 4) < 1e-9);
        }
    }

    // "no fill" hides a stroke on both tabs, not black; undo, reload and Solid bring it back
    {
        KoFillConfigWidget *strokeFill = widget->findChild<KoFillConfigWidget*>();
        KoFillConfigWidget *shapeFill = nullptr;
        Q_FOREACH (QPointer<QWidget> option, selectTool->optionWidgets()) {
            if (!option) continue;
            Q_FOREACH (KoFillConfigWidget *candidate, option->findChildren<KoFillConfigWidget*>()) {
                if (candidate != strokeFill) shapeFill = candidate;
            }
        }
        QVERIFY(strokeFill && shapeFill);
        strokeFill->activate();
        shapeFill->activate();
        QAbstractButton *strokeNone = strokeFill->findChild<QAbstractButton*>("btnNoFill");
        QAbstractButton *strokeSolid = strokeFill->findChild<QAbstractButton*>("btnSolidFill");
        QAbstractButton *shapeNone = shapeFill->findChild<QAbstractButton*>("btnNoFill");
        QVERIFY(strokeNone && strokeSolid && shapeNone);

        const QColor blueColor(20, 40, 200);
        const QColor greenColor(20, 180, 40);
        const QColor black(0, 0, 0);
        const QColor red(255, 0, 0);
        auto selectAndShow = [&] (const QList<KoShape*> &shapes) {
            select(shapes);
            QVERIFY(QMetaObject::invokeMethod(strokeFill, "shapeChanged"));
            QVERIFY(QMetaObject::invokeMethod(shapeFill, "shapeChanged"));
        };
        auto strokeColorValid = [] (KoShape *shape) {
            return qSharedPointerDynamicCast<KoShapeStroke>(shape->stroke())->color().isValid();
        };
        // no setDirty here: hiding has to repaint by itself
        auto pixels = [&] (const QColor &color) {
            settle(400);
            image->waitForDone();
            settle(400);
            return colorPixels(vector, bounds, color);
        };

        // the sections above leave the colors of the active resources on them
        selection->deselectAll();
        blue->setBackground(toQShared(new KoColorBackground(blueColor)));
        green->setBackground(toQShared(new KoColorBackground(greenColor)));
        line->setStroke(toQShared(new KoShapeStroke(strokeWidth(line), red)));
        line->setBackground(QSharedPointer<KoShapeBackground>());
        vector->setDirty();

        QVERIFY(pixels(blueColor) > 100);
        QVERIFY(pixels(greenColor) > 100);
        QVERIFY(pixels(red) > 100);
        const int blackBefore = pixels(black);

        // the Stroke tab
        selectAndShow({blue});
        strokeNone->click();
        QCOMPARE(pixels(blueColor), 0);
        QCOMPARE(pixels(black), blackBefore);
        QVERIFY(!strokeColorValid(blue));
        QCOMPARE(blue->color(), blueColor);
        selectAndShow({blue});
        QVERIFY(strokeNone->isChecked());

        // undo and redo, through the document as the app does
        doc->undoStack()->undo();
        QVERIFY(pixels(blueColor) > 100);
        QVERIFY(strokeColorValid(blue));
        doc->undoStack()->redo();
        QCOMPARE(pixels(blueColor), 0);

        // a new width on a hidden stroke leaves it hidden
        {
            const qreal width = 6.0 / ppp;
            QScopedPointer<KUndo2Command> command(KoFlake::modifyShapesStrokes({blue}, [width] (KoShapeStrokeSP stroke) { stroke->setLineWidth(width); }));
            command->redo();
            QVERIFY(qAbs(strokeWidth(blue) - width) < 1e-9);
            QVERIFY(!strokeColorValid(blue));
            QCOMPARE(pixels(blueColor), 0);
        }

        // Solid brings back this stroke's color, not the one of the selection before
        selectAndShow({green});
        QCOMPARE(colorAction(strokeFill)->currentColor(), greenColor);
        selectAndShow({blue});
        QCOMPARE(colorAction(strokeFill)->currentColor(), blueColor);
        settle(400);
        QVERIFY(!strokeColorValid(blue));
        strokeSolid->click();
        QVERIFY(pixels(blueColor) > 100);
        QCOMPARE(blue->color(), blueColor);

        // the Fill tab, where a brush stroke's color also lives: black before
        selectAndShow({green});
        shapeNone->click();
        QCOMPARE(pixels(greenColor), 0);
        QCOMPARE(pixels(black), blackBefore);
        QCOMPARE(green->color(), greenColor);
        QVERIFY(!strokeColorValid(green));

        // a saved hidden stroke stays hidden and keeps its color, and other readers show nothing
        {
            QBuffer buffer;
            buffer.open(QIODevice::WriteOnly);
            SvgWriter writer(QList<KoShape*>{green});
            writer.save(buffer, image->bounds().size());
            QVERIFY(buffer.data().contains("fill=\"none\""));
            buffer.close();
            buffer.open(QIODevice::ReadOnly);
            QSizeF fragment;
            const QList<KoShape*> loaded = KisShapeLayer::createShapesFromSvg(&buffer, QString(), image->bounds(), 72.0 * image->xRes(),
                                                                              doc->shapeController()->resourceManager(), true, &fragment);
            QCOMPARE(loaded.size(), 1);
            VStrokeShape *reloaded = dynamic_cast<VStrokeShape*>(loaded.first());
            QVERIFY(reloaded);
            QCOMPARE(reloaded->color(), greenColor);
            QVERIFY(!strokeColorValid(reloaded));
            vector->addShape(reloaded);
            QCOMPARE(pixels(greenColor), 0);
            vector->removeShape(reloaded);
            delete reloaded;
        }
        // a busy image ignores undo
        pixels(greenColor);
        doc->undoStack()->undo();
        QVERIFY(pixels(greenColor) > 100);
        QVERIFY(strokeColorValid(green));

        // an eraser saved with fill="none" still erases after a reload: only "no line" hides
        {
            VStrokeShape *eraser = brushStroke(image, preset, 200, 1.0, 1.0, greenColor);
            VStrokeData settings = eraser->data();
            delete eraser;
            settings.compositeOp = COMPOSITE_ERASE;
            KisPaintInformation a(QPointF(40, 200), 1.0);
            KisPaintInformation b(QPointF(300, 200), 1.0);
            eraser = VStrokeShape::fromImagePixels({VStrokePrimitive::line(a, b)}, image->xRes(), settings);
            QBuffer buffer;
            buffer.open(QIODevice::WriteOnly);
            SvgWriter writer(QList<KoShape*>{eraser});
            writer.save(buffer, image->bounds().size());
            buffer.close();
            buffer.open(QIODevice::ReadOnly);
            QSizeF fragment;
            const QList<KoShape*> loaded = KisShapeLayer::createShapesFromSvg(&buffer, QString(), image->bounds(), 72.0 * image->xRes(),
                                                                              doc->shapeController()->resourceManager(), true, &fragment);
            QCOMPARE(loaded.size(), 1);
            QVERIFY(strokeColorValid(loaded.first()));
            qDeleteAll(loaded);
            delete eraser;
        }

        // a poly-tool stroke: no line, not black, also after a reload
        selectAndShow({line});
        strokeNone->click();
        QCOMPARE(pixels(red), 0);
        QCOMPARE(pixels(black), blackBefore);
        {
            QBuffer buffer;
            buffer.open(QIODevice::WriteOnly);
            SvgWriter writer(QList<KoShape*>{line});
            writer.save(buffer, image->bounds().size());
            buffer.close();
            buffer.open(QIODevice::ReadOnly);
            QSizeF fragment;
            const QList<KoShape*> loaded = KisShapeLayer::createShapesFromSvg(&buffer, QString(), image->bounds(), 72.0 * image->xRes(),
                                                                              doc->shapeController()->resourceManager(), true, &fragment);
            QCOMPARE(loaded.size(), 1);
            vector->addShape(loaded.first());
            QCOMPARE(pixels(red), 0);
            QCOMPARE(pixels(black), blackBefore);
            vector->removeShape(loaded.first());
            delete loaded.first();
        }
        pixels(red);
        doc->undoStack()->undo();
        QVERIFY(pixels(red) > 100);
    }
    select({blue, green});

    // a color change while the image is busy pushes its command from the compressor
    settle(400);
    KoFillConfigWidget *fill = widget->findChild<KoFillConfigWidget*>();
    QVERIFY(fill);
    KoColorPopupAction *colorAction = fill->findChild<KoColorPopupAction*>();
    QVERIFY(colorAction);
    fill->activate();
    settle(300);
    s_compressorAsserts.clear();
    {
        KisStrokeId id = image->startStroke(new SlowStroke(400));
        image->addJob(id, new KisStrokeJobData());
        image->endStroke(id);
        colorAction->setCurrentColor(QColor(10, 200, 200));
        colorAction->trigger();
        settle(800);
    }
    QVERIFY(blue->color() == QColor(10, 200, 200));
    QVERIFY(green->color() == QColor(10, 200, 200));
    QVERIFY2(s_compressorAsserts.isEmpty(), qPrintable(s_compressorAsserts.join("\n")));

    selection->deselectAll();
    qInstallMessageHandler(s_previousHandler);
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VStrokeStyleTest)

#include "VStrokeStyleTest.moc"
