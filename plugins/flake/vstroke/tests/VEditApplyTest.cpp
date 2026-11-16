/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QComboBox>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QSpinBox>
#include <QClipboard>
#include <QPushButton>
#include <QMimeData>
#include <QTimer>
#include <QtEndian>
#include <QtTest/qtestspontaneevent.h>

#include <KConfigGroup>
#include <KSharedConfig>
#include <KoColorSpaceRegistry.h>
#include <KoPointerEvent.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoShapeController.h>
#include <KoShapeGroup.h>
#include <commands/KoShapeGroupCommand.h>
#include <KoToolBase.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <actions/KisPasteActionFactories.h>
#include <kis_canvas2.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_layer.h>
#include <kis_shape_layer.h>
#include <kis_slider_spin_box.h>
#include <kundo2stack.h>

#include <testui.h>

#include "VCurveOpacity.h"
#include "VCurveShape.h"
#include "VEditActions.h"
#include "VEditTool.h"
#include "VPointEdit.h"
#include "VPointEditTool.h"
#include "VWindowTestUtils.h"
#include <kis_zoom_manager.h>

class VEditApplyTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testAppliesToSelection();
};

namespace {
using VWindowTest::encode;

QByteArray curveSvg(qreal y)
{
    QVector<double> positions, radii;
    QString d;
    for (int i = 0; i < 40; i++) {
        const qreal x = 10 + i * 4;
        const qreal py = y + (i % 2 ? 0.2 : -0.2);
        positions << x << py;
        radii << 3;
        d += QString("%1%2,%3 ").arg(i ? "L" : "M").arg(x).arg(py);
    }
    const QString arrays =
        QString("<krita:a name=\"offsets\" type=\"i32\">%1</krita:a>").arg(encode(QVector<qint32>{0})) +
        QString("<krita:a name=\"cyclic\" type=\"u8\">%1</krita:a>").arg(encode(QVector<quint8>{0})) +
        QString("<krita:a name=\"position\" type=\"f64x2\">%1</krita:a>").arg(encode(positions)) +
        QString("<krita:a name=\"radius\" type=\"f64\">%1</krita:a>").arg(encode(radii));
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:krita=\"http://krita.org/namespaces/svg/krita\""
                   " width=\"300pt\" height=\"300pt\" viewBox=\"0 0 300 300\">"
                   "<path d=\"%1\" fill=\"none\" stroke=\"#000000\" stroke-width=\"6\""
                   " krita:type=\"vcurve\" krita:vcurve-version=\"1\">"
                   "<krita:curves version=\"1\" points=\"40\" curves=\"1\">%2</krita:curves></path></svg>")
        .arg(d, arrays).toUtf8();
}

using VWindowTest::paste;

void mouse(KoToolBase *tool, const QPointF &from, const QPointF &to)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent begin(&press, from);
    tool->mousePressEvent(&begin);
    QMouseEvent move(QEvent::MouseMove, QPointF(), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent moved(&move, to);
    tool->mouseMoveEvent(&moved);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QSpontaneKeyEvent::setSpontaneous(&release);
    KoPointerEvent end(&release, to);
    tool->mouseReleaseEvent(&end);
}
}

void VEditApplyTest::testAppliesToSelection()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KisImageSP image = new KisImage(0, 400, 300, KoColorSpaceRegistry::instance()->rgb8(), "apply");
    KisDocument *doc = VWindowTest::addDocument(image);
    KisPaintLayerSP raster = new KisPaintLayer(image, "raster", OPACITY_OPAQUE_U8);
    image->addNode(raster);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();
    KoCanvasBase *canvas = view->canvasBase();
    auto selection = [&] () { return canvas->selectedShapesProxy()->selection(); };

    // three strokes A, B and C on a new layer; @p select picks with the shape select tool active
    auto check = [&] (const QString &name, bool selectedOnly, const QString &expected, const std::function<void(QList<VCurveShape*> &)> &select) {
        for (VEditActions::Whole what : {VEditActions::Whole::Simplify, VEditActions::Whole::Smooth, VEditActions::Whole::Subdivide}) {
            KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
            layer->setNodeProperty("freehandvector", true);
            image->addNode(layer);
            image->waitForDone();
            nodes->slotUiActivatedNode(layer);
            QTest::qWait(50);
            paste(curveSvg(20), viewManager);
            paste(curveSvg(60), viewManager);
            paste(curveSvg(100), viewManager);
            QList<VCurveShape*> shapes;
            Q_FOREACH (KoShape *s, layer->shapes()) shapes << dynamic_cast<VCurveShape*>(s);
            std::sort(shapes.begin(), shapes.end(), [] (VCurveShape *a, VCurveShape *b) {
                return a->absoluteOutlineRect().y() < b->absoluteOutlineRect().y();
            });
            selection()->deselectAll();
            tools->switchToolRequested("InteractionTool");
            QTest::qWait(100);
            QList<VCurveData> before;
            for (VCurveShape *s : shapes) before << s->curves();
            select(shapes);

            tools->switchToolRequested("VPointEditTool");
            QTest::qWait(100);
            VPointEditTool *edit = dynamic_cast<VPointEditTool*>(tools->toolById(canvas, "VPointEditTool"));
            QVERIFY(edit);
            edit->setActsOnWholeLayer(!selectedOnly);
            edit->actionSettings().subdivideSpacing = 1;
            edit->actionSettings().joinEnds = false;
            edit->actionSettings().mergeOverlapping = false;
            edit->applyToSelected(what);
            image->waitForDone();

            QString changed;
            for (int i = 0; i < shapes.size(); i++) changed += shapes[i]->curves() == before[i] ? QChar('-') : QChar("ABC"[i]);
            QVERIFY2(changed == expected, qPrintable(QString("%1, action %2: changed %3, expected %4").arg(name).arg(int(what)).arg(changed, expected)));
        }
    };
    auto group = [&] (const QList<KoShape*> &members) {
        KoShapeGroup *g = new KoShapeGroup();
        KUndo2Command *cmd = new KUndo2Command();
        canvas->shapeController()->addShapeDirect(g, members.first()->parent(), cmd);
        new KoShapeGroupCommand(g, members, true, cmd);
        canvas->addCommand(cmd);
        image->waitForDone();
        QTest::qWait(50);
        selection()->deselectAll();
        return g;
    };

    // Apply to Whole layer: the whole layer, selected or not
    check("off, nothing", false, "ABC", [] (QList<VCurveShape*> &) {});
    check("off, one selected", false, "ABC", [&] (QList<VCurveShape*> &s) { selection()->select(s[0]); });
    check("off, with a group", false, "ABC", [&] (QList<VCurveShape*> &s) { group({s[0], s[1]}); });
    // Apply to Selection, nothing selected: nothing
    check("on, nothing", true, "---", [] (QList<VCurveShape*> &) {});
    // Selection: however the stroke was selected
    check("direct", true, "A--", [&] (QList<VCurveShape*> &s) { selection()->select(s[0]); });
    check("click", true, "A--", [&] (QList<VCurveShape*> &s) {
        const QPointF p = s[0]->absoluteOutlineRect().center();
        mouse(tools->toolById(canvas, "InteractionTool"), p, p);
    });
    check("rubber band", true, "A--", [&] (QList<VCurveShape*> &s) {
        const QRectF r = s[0]->absoluteOutlineRect().adjusted(-5, -5, 5, 5);
        mouse(tools->toolById(canvas, "InteractionTool"), r.topLeft(), r.bottomRight());
    });
    check("back from the brush", true, "A--", [&] (QList<VCurveShape*> &s) {
        selection()->select(s[0]);
        tools->switchToolRequested("VStrokeTool");
        QTest::qWait(100);
    });
    check("back from another layer", true, "A--", [&] (QList<VCurveShape*> &s) {
        selection()->select(s[0]);
        KisNodeSP current = viewManager->activeNode();
        nodes->slotUiActivatedNode(raster);
        QTest::qWait(100);
        nodes->slotUiActivatedNode(current);
        QTest::qWait(100);
    });
    // a group stands for the strokes in it
    check("group", true, "AB-", [&] (QList<VCurveShape*> &s) { selection()->select(group({s[0], s[1]})); });
    check("group clicked", true, "AB-", [&] (QList<VCurveShape*> &s) {
        group({s[0], s[1]});
        const QPointF p = s[0]->absoluteOutlineRect().center();
        mouse(tools->toolById(canvas, "InteractionTool"), p, p);
    });
    // a selection with nothing to edit never turns into the whole layer
    check("hidden", true, "---", [&] (QList<VCurveShape*> &s) {
        selection()->select(s[0]);
        s[0]->setVisible(false);
    });

    // the panels: the buttons are in Edit Shapes, and the brushes keep their options
    tools->switchToolRequested("VEditTool");
    QTest::qWait(100);
    VEditTool *vectorEdit = dynamic_cast<VEditTool*>(tools->toolById(canvas, "VEditTool"));
    QVERIFY(vectorEdit);
    Q_FOREACH (QPointer<QWidget> widget, vectorEdit->optionWidgets()) {
        QVERIFY(widget);
        Q_FOREACH (QPushButton *button, widget->findChildren<QPushButton*>()) {
            QVERIFY2(button->text() != "Apply", "Vector Edit lost its Apply buttons");
        }
    }
    bool hasSelectedOnly = false;
    Q_FOREACH (QPointer<QWidget> widget, vectorEdit->optionWidgets()) {
        Q_FOREACH (QCheckBox *box, widget->findChildren<QCheckBox*>()) {
            hasSelectedOnly |= box->text() == "Selected strokes only";
        }
    }
    QVERIFY(hasSelectedOnly);

    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(100);
    VPointEditTool *pointEdit = dynamic_cast<VPointEditTool*>(tools->toolById(canvas, "VPointEditTool"));
    QVERIFY(pointEdit);
    QPushButton *thicker = nullptr;
    QPushButton *setRadius = nullptr;
    QPushButton *opacitySet = nullptr;
    int applyButtons = 0;
    Q_FOREACH (QPointer<QWidget> widget, pointEdit->optionWidgets()) {
        if (!thicker) thicker = widget->findChild<QPushButton*>("btnThicker");
        if (!setRadius) setRadius = widget->findChild<QPushButton*>("btnSetRadius");
        if (!opacitySet) opacitySet = widget->findChild<QPushButton*>("btnOpacitySet");
        applyButtons += widget->findChildren<QPushButton*>("btnApply").size();
    }
    QVERIFY(thicker && setRadius && opacitySet);
    QCOMPARE(applyButtons, 3);

    KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
    layer->setNodeProperty("freehandvector", true);
    image->addNode(layer);
    image->waitForDone();
    nodes->slotUiActivatedNode(layer);
    QTest::qWait(50);
    paste(curveSvg(20), viewManager);
    paste(curveSvg(60), viewManager);
    QList<VCurveShape*> strokes;
    Q_FOREACH (KoShape *s, layer->shapes()) strokes << dynamic_cast<VCurveShape*>(s);
    std::sort(strokes.begin(), strokes.end(), [] (VCurveShape *a, VCurveShape *b) {
        return a->absoluteOutlineRect().y() < b->absoluteOutlineRect().y();
    });
    QCOMPARE(strokes.size(), 2);
    const VCurveData curveA = strokes[0]->curves();
    const VCurveData curveB = strokes[1]->curves();
    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(100);
    pointEdit->setActsOnWholeLayer(false);
    pointEdit->setFalloffRadius(0);
    selection()->deselectAll();
    selection()->select(strokes[0]);

    // whole selected stroke, one undo step, and back
    int steps = doc->undoStack()->index();
    thicker->click();
    image->waitForDone();
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QVERIFY(qAbs(strokes[0]->curves().radii.first() - curveA.radii.first() * 1.25) < 1e-6);
    QVERIFY(qAbs(strokes[0]->curves().radii.last() - curveA.radii.last() * 1.25) < 1e-6);
    QVERIFY(strokes[1]->curves() == curveB);
    doc->undoStack()->undo();
    image->waitForDone();
    QVERIFY(strokes[0]->curves() == curveA);

    // only the selected points: click a point of the selected stroke
    const QPointF pointAt = strokes[0]->absoluteTransformation().map(curveA.positions[20]);
    mouse(pointEdit, pointAt, pointAt);
    QCOMPARE(pointEdit->selectedPoints(strokes[0]), QVector<int>{20});
    doc->undoStack()->index();
    pointEdit->actionSettings().setRadius = 7;
    setRadius->click();
    image->waitForDone();
    QVERIFY(qAbs(strokes[0]->curves().radii[20] - 7) < 1e-6);
    QVERIFY(strokes[0]->curves().radii[5] == curveA.radii[5]);
    doc->undoStack()->undo();
    image->waitForDone();
    QVERIFY(strokes[0]->curves() == curveA);

    // the same on Smooth: points outside stay
    pointEdit->actionSettings().smoothPosition = true;
    pointEdit->applyToSelected(VEditActions::Whole::Smooth);
    image->waitForDone();
    QVERIFY(strokes[0]->curves().positions[5] == curveA.positions[5]);
    QVERIFY(strokes[0]->curves().positions[20] != curveA.positions[20]);
    doc->undoStack()->undo();
    image->waitForDone();

    // opacity set, to the whole layer
    pointEdit->setActsOnWholeLayer(true);
    pointEdit->actionSettings().affectOpacity = true;
    pointEdit->actionSettings().affectDensity = false;
    pointEdit->actionSettings().setOpacity = 0.25;
    opacitySet->click();
    image->waitForDone();
    QVERIFY(qAbs(strokes[0]->curves().attribute(VCurveOpacity::Attribute, 3, 1.0) - 0.25) < 1e-6);
    QVERIFY(qAbs(strokes[1]->curves().attribute(VCurveOpacity::Attribute, 3, 1.0) - 0.25) < 1e-6);
    pointEdit->setActsOnWholeLayer(false);

    // every number box in both tools is a slider, easy to drag with a tablet
    for (KoToolBase *tool : {static_cast<KoToolBase*>(vectorEdit), static_cast<KoToolBase*>(pointEdit)}) {
        Q_FOREACH (QPointer<QWidget> widget, tool->optionWidgets()) {
            Q_FOREACH (QDoubleSpinBox *box, widget->findChildren<QDoubleSpinBox*>()) {
                QVERIFY2(qobject_cast<KisDoubleSliderSpinBox*>(box), qPrintable(box->toolTip()));
                // 0.01 for values up to 1, else 0.5 or 1
                KisDoubleSliderSpinBox *slider = qobject_cast<KisDoubleSliderSpinBox*>(box);
                QVERIFY(box->maximum() > 1.0 ? slider->fastSliderStep() >= 0.5 : qFuzzyCompare(slider->fastSliderStep(), 0.01));
                // the arrow keys and the wheel too
                QCOMPARE(box->singleStep(), box->maximum() > 1.0 ? 1.0 : 0.01);
            }
            Q_FOREACH (QSpinBox *box, widget->findChildren<QSpinBox*>()) {
                QVERIFY2(qobject_cast<KisSliderSpinBox*>(box), qPrintable(box->toolTip()));
            }
        }
    }
    // Subdivide's options: Steps capped at the most, smoothing on by default
    Q_FOREACH (QPointer<QWidget> widget, pointEdit->optionWidgets()) {
        if (KisSliderSpinBox *steps = widget->findChild<KisSliderSpinBox*>("spnSubdivideSteps")) {
            QCOMPARE(steps->maximum(), VCurveData::MaxSubdivideSteps);
            // only the chosen method's slider shows
            QComboBox *method = widget->findChild<QComboBox*>("cmbSubdivideMethod");
            QVERIFY(method);
            const int before = method->currentIndex();
            method->setCurrentIndex(method->findData(int(VEditSettings::SubdivideMethod::Distance)));
            QVERIFY(steps->isHidden());
            method->setCurrentIndex(method->findData(int(VEditSettings::SubdivideMethod::Steps)));
            QVERIFY(!steps->isHidden());
            method->setCurrentIndex(before);
        }
    }
    QVERIFY(VEditSettings().subdivideSmooth);

    // each Vector Edit mode keeps its own brush size
    tools->switchToolRequested("VEditTool");
    QTest::qWait(100);
    vectorEdit->setMode(VEditTool::Mode::Smooth);
    vectorEdit->setBrushSize(12);
    vectorEdit->setMode(VEditTool::Mode::Trim);
    vectorEdit->setBrushSize(80);
    QCOMPARE(vectorEdit->settings().size, 80.0);
    vectorEdit->setMode(VEditTool::Mode::Smooth);
    QCOMPARE(vectorEdit->settings().size, 12.0);
    QCOMPARE(vectorEdit->brushSize(VEditTool::Mode::Trim), 80.0);

    // the size goes down to 0, in the slider and by the shortcuts (tenths below 1)
    Q_FOREACH (QPointer<QWidget> widget, vectorEdit->optionWidgets()) {
        if (QDoubleSpinBox *size = widget->findChild<QDoubleSpinBox*>("dblSize")) QCOMPARE(size->minimum(), 0.0);
    }
    vectorEdit->setBrushSize(2);
    vectorEdit->changeBrushSize(false);
    QCOMPARE(vectorEdit->settings().size, 1.0);
    vectorEdit->changeBrushSize(false);
    QVERIFY(qAbs(vectorEdit->settings().size - 0.9) < 1e-9);
    vectorEdit->setBrushSize(0.1);
    vectorEdit->changeBrushSize(false);
    QCOMPARE(vectorEdit->settings().size, 0.0);
    vectorEdit->changeBrushSize(false);
    QCOMPARE(vectorEdit->settings().size, 0.0);
    vectorEdit->changeBrushSize(true);
    QVERIFY(qAbs(vectorEdit->settings().size - 0.1) < 1e-9);
    vectorEdit->setBrushSize(0.95);
    vectorEdit->changeBrushSize(true);
    QVERIFY(qAbs(vectorEdit->settings().size - 1.0) < 1e-9);
    vectorEdit->changeBrushSize(true);
    QCOMPARE(vectorEdit->settings().size, 2.0);
    vectorEdit->setBrushSize(-3);
    QCOMPARE(vectorEdit->settings().size, 0.0);
    vectorEdit->setBrushSize(12);

    // one point selected: the context menu sets its type; Smooth gives a line point handles
    tools->switchToolRequested("VPointEditTool");
    QTest::qWait(100);
    selection()->deselectAll();
    selection()->select(strokes[0]);
    pointEdit->selectPoint(strokes[0], 20);
    const VCurveData beforeType = strokes[0]->curves();
    QMenu *menu = pointEdit->popupActionsMenu();
    QVERIFY(menu);
    QAction *smooth = nullptr;
    QStringList names;
    Q_FOREACH (QAction *action, menu->actions()) {
        if (action->isSeparator()) continue;
        names << action->objectName();
        if (action->objectName() == "pathpoint-smooth") smooth = action;
    }
    QCOMPARE(names, QStringList({"pathpoint-corner", "pathpoint-smooth", "pathpoint-symmetric",
                                 "pathpoint-curve", "pathpoint-line"}));
    steps = doc->undoStack()->index();
    smooth->trigger();
    image->waitForDone();
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    const VCurveData smoothed = strokes[0]->curves();
    QCOMPARE(smoothed.segment(19), VCurveData::CurveSegment);
    QCOMPARE(smoothed.segment(20), VCurveData::CurveSegment);
    QCOMPARE(VPointEdit::pointType(smoothed, 20), VPointEdit::PointType::Smooth);

    // the handles show only where the points are sparse on screen: zoom in until they do
    const QTransform toDocument = strokes[0]->absoluteTransformation();
    auto toView = [&] () { return toDocument * view->canvasBase()->viewConverter()->documentToView(); };
    for (int i = 0; i < 12 && !VPointEditTool::showsHandles(smoothed, 20, toView()); i++) {
        view->zoomManager()->slotZoomIn();
        QTest::qWait(20);
    }
    QVERIFY(VPointEditTool::showsHandles(smoothed, 20, toView()));

    // drag the outgoing handle: the incoming one stays in line, one undo step
    const QPointF handle = toDocument.map(smoothed.handlesOut[20]);
    steps = doc->undoStack()->index();
    mouse(pointEdit, handle, handle + QPointF(0, 6));
    image->waitForDone();
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    const VCurveData dragged = strokes[0]->curves();
    QVERIFY(QLineF(toDocument.map(dragged.handlesOut[20]), handle + QPointF(0, 6)).length() < 1e-6);
    QCOMPARE(dragged.positions[20], smoothed.positions[20]);
    const QPointF p = dragged.positions[20];
    const QPointF in = dragged.handlesIn[20] - p;
    const QPointF out = dragged.handlesOut[20] - p;
    QVERIFY(qAbs(in.x() * out.y() - in.y() * out.x()) < 1e-6);
    QCOMPARE(pointEdit->selectedPoints(strokes[0]), QVector<int>{20});
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    image->waitForDone();
    QVERIFY(strokes[0]->curves() == beforeType);
    // nothing selected: the popup palette as usual
    pointEdit->selectPoint(strokes[0], 20);
    selection()->deselectAll();
    QVERIFY(!pointEdit->popupActionsMenu());

    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VEditApplyTest)

#include "VEditApplyTest.moc"
