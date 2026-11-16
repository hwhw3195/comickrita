/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QMimeData>
#include <QTimer>
#include <QtEndian>
#include <QtTest/qtestspontaneevent.h>

#include <KoColorSpaceRegistry.h>
#include <KoPointerEvent.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
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
#include <kis_shape_controller.h>
#include <kis_shape_layer.h>

#include <testui.h>

#include "VCurveShape.h"
#include "VWindowTestUtils.h"

/**
 * The selection shapes of the shape select tool (the stock DefaultTool), by
 * its real events: the rectangle as before, an ellipse by drag direction like
 * the rectangle, a lasso and a polygon selecting what they fully contain, and
 * an option making all of them select what they touch.
 */
class VSelectShapesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSelectionShapes();
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

void press(KoToolBase *tool, const QPointF &at, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(), Qt::LeftButton, Qt::LeftButton, modifiers);
    KoPointerEvent pointer(&event, at);
    tool->mousePressEvent(&pointer);
}

void move(KoToolBase *tool, const QPointF &to, Qt::KeyboardModifiers modifiers = Qt::NoModifier, Qt::MouseButtons buttons = Qt::LeftButton)
{
    QMouseEvent event(QEvent::MouseMove, QPointF(), Qt::NoButton, buttons, modifiers);
    KoPointerEvent pointer(&event, to);
    tool->mouseMoveEvent(&pointer);
}

void release(KoToolBase *tool, const QPointF &at, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(), Qt::LeftButton, Qt::NoButton, modifiers);
    QSpontaneKeyEvent::setSpontaneous(&event);
    KoPointerEvent pointer(&event, at);
    tool->mouseReleaseEvent(&pointer);
}

void drag(KoToolBase *tool, const QVector<QPointF> &points, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    press(tool, points.first(), modifiers);
    for (int i = 1; i < points.size(); i++) move(tool, points[i], modifiers);
    release(tool, points.last(), modifiers);
}

void click(KoToolBase *tool, const QPointF &at, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    press(tool, at, modifiers);
    release(tool, at, modifiers);
}

void doubleClick(KoToolBase *tool, const QPointF &at)
{
    click(tool, at);
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    KoPointerEvent pointer(&event, at);
    tool->mouseDoubleClickEvent(&pointer);
    release(tool, at);
}

void key(KoToolBase *tool, int code)
{
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
    tool->keyPressEvent(&event);
}

/// a polygon around \p rect, a lasso when drawn with many points
QVector<QPointF> around(const QRectF &rect, int perSide = 8)
{
    const QVector<QPointF> corners = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
    QVector<QPointF> points;
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < perSide; i++) {
            points << corners[c] + (corners[(c + 1) % 4] - corners[c]) * qreal(i) / perSide;
        }
    }
    return points;
}
}

void VSelectShapesTest::testSelectionShapes()
{
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());

    KisImageSP image = new KisImage(0, 400, 400, KoColorSpaceRegistry::instance()->rgb8(), "select");
    KisDocument *doc = VWindowTest::addDocument(image);
    KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), image, "vector", OPACITY_OPAQUE_U8);
    layer->setNodeProperty("freehandvector", true);
    image->addNode(layer);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KoCanvasBase *canvas = view->canvasBase();
    viewManager->nodeManager()->slotUiActivatedNode(layer);
    QTest::qWait(50);

    // A and B level, C going down to the right, with its bounding box mostly empty
    paste(curveSvg(40, 40), viewManager);
    paste(curveSvg(40, 100), viewManager);
    paste(curveSvg(40, 160, 4), viewManager);
    QList<VCurveShape*> shapes;
    Q_FOREACH (KoShape *s, layer->shapes()) shapes << dynamic_cast<VCurveShape*>(s);
    QCOMPARE(shapes.size(), 3);
    std::sort(shapes.begin(), shapes.end(), [] (VCurveShape *a, VCurveShape *b) {
        return a->absoluteOutlineRect().y() < b->absoluteOutlineRect().y();
    });
    const QRectF a = shapes[0]->absoluteOutlineRect();
    const QRectF b = shapes[1]->absoluteOutlineRect();
    const QRectF c = shapes[2]->absoluteOutlineRect();

    tools->switchToolRequested("InteractionTool");
    QTRY_COMPARE(tools->activeToolId(), QString("InteractionTool"));
    KoToolBase *tool = tools->toolById(canvas, "InteractionTool");
    QVERIFY(tool);
    KoSelection *selection = canvas->selectedShapesProxy()->selection();

    QComboBox *shapeCombo = nullptr;
    Q_FOREACH (QPointer<QWidget> option, tool->optionWidgets()) {
        if (!option) continue;
        if (!shapeCombo) shapeCombo = option->findChild<QComboBox*>("cmbSelectionShape");
    }
    QVERIFY(shapeCombo);
    QCOMPARE(shapeCombo->count(), 4);

    auto selected = [&] () {
        QString result;
        for (int i = 0; i < shapes.size(); i++) {
            if (selection->isSelected(shapes[i])) result += QChar("ABC"[i]);
        }
        return result;
    };
    auto start = [&] (int shape) {
        selection->deselectAll();
        shapeCombo->setCurrentIndex(shape);
    };
    enum { Rectangle, Ellipse, Lasso, Polygon };

    // the rectangle stays as it was: left to right covers, right to left crosses
    start(Rectangle);
    drag(tool, {a.topLeft() - QPointF(5, 5), a.bottomRight() + QPointF(5, 5)});
    QCOMPARE(selected(), QString("A"));
    start(Rectangle);
    drag(tool, {QPointF(a.center().x() - 10, a.top() - 5), QPointF(a.center().x() + 10, a.bottom() + 5)});
    QCOMPARE(selected(), QString());
    start(Rectangle);
    drag(tool, {QPointF(a.center().x() + 10, a.top() - 5), QPointF(a.center().x() - 10, a.bottom() + 5)});
    QCOMPARE(selected(), QString("A"));

    // an ellipse takes the same directions, by the outline: B is thin and fits in a tall one
    const QRectF aroundB = b.adjusted(-40, -28, 40, 28);
    start(Ellipse);
    drag(tool, {aroundB.topLeft(), aroundB.center(), aroundB.bottomRight()});
    QCOMPARE(selected(), QString("B"));
    start(Ellipse);
    drag(tool, {QPointF(b.center().x() - 10, b.top() - 5), QPointF(b.center().x() + 10, b.bottom() + 5)});
    QCOMPARE(selected(), QString());
    start(Ellipse);
    drag(tool, {QPointF(b.center().x() + 10, b.top() - 5), QPointF(b.center().x() - 10, b.bottom() + 5)});
    QCOMPARE(selected(), QString("B"));
    // an ellipse just too small for a box whose corners it misses
    start(Ellipse);
    drag(tool, {a.topLeft() - QPointF(1, 1), a.bottomRight() + QPointF(1, 1)});
    QCOMPARE(selected(), QString());

    // a lasso takes what is fully inside, in any direction; Shift adds
    start(Lasso);
    drag(tool, around(a.adjusted(-5, -5, 5, 5)));
    QCOMPARE(selected(), QString("A"));
    drag(tool, around(b.adjusted(-5, -5, 5, 5)), Qt::ShiftModifier);
    QCOMPARE(selected(), QString("AB"));
    start(Lasso);
    drag(tool, around(a.adjusted(40, -5, -40, 5)));
    QCOMPARE(selected(), QString());

    // a triangle around the slanted C, leaving out the bounding box a rectangle would need
    const QVector<QPointF> triangle = {c.topLeft() + QPointF(-8, -12), c.topRight() + QPointF(12, c.height() - 12) + QPointF(8, 12),
                                       c.bottomLeft() + QPointF(-8, 12)};
    start(Lasso);
    drag(tool, triangle);
    QCOMPARE(selected(), QString("C"));

    // a polygon: the corners by clicks, finished by a double click
    start(Polygon);
    click(tool, triangle[0]);
    click(tool, triangle[1]);
    QCOMPARE(selected(), QString());
    move(tool, triangle[2], Qt::NoModifier, Qt::NoButton);
    doubleClick(tool, triangle[2]);
    QCOMPARE(selected(), QString("C"));

    // by clicking the first corner again; the next drag starts a new one
    start(Polygon);
    const QVector<QPointF> box = around(a.adjusted(-5, -5, 5, 5), 1);
    for (const QPointF &corner : box) click(tool, corner);
    QCOMPARE(selected(), QString());
    click(tool, box.first());
    QCOMPARE(selected(), QString("A"));

    // by Enter; Shift on the first corner adds, as a drag does
    const QVector<QPointF> boxB = around(b.adjusted(-5, -5, 5, 5), 1);
    click(tool, boxB[0], Qt::ShiftModifier);
    for (int i = 1; i < boxB.size(); i++) click(tool, boxB[i]);
    key(tool, Qt::Key_Return);
    QCOMPARE(selected(), QString("AB"));

    // Escape cancels and selects nothing, and so does switching the shape midway
    start(Polygon);
    for (const QPointF &corner : box) click(tool, corner);
    key(tool, Qt::Key_Escape);
    QCOMPARE(selected(), QString());
    click(tool, box.first());
    shapeCombo->setCurrentIndex(Rectangle);
    QCOMPARE(selected(), QString());
    drag(tool, {a.topLeft() - QPointF(5, 5), a.bottomRight() + QPointF(5, 5)});
    QCOMPARE(selected(), QString("A"));

    // leaving the tool drops an unfinished polygon
    start(Polygon);
    click(tool, box[0]);
    click(tool, box[1]);
    tools->switchToolRequested("KritaShape/KisToolBrush");
    QTest::qWait(100);
    tools->switchToolRequested("InteractionTool");
    QTRY_COMPARE(tools->activeToolId(), QString("InteractionTool"));
    for (const QPointF &corner : box) click(tool, corner);
    key(tool, Qt::Key_Return);
    QCOMPARE(selected(), QString("A"));

    // a press on a stroke selects it with the rectangle and the ellipse
    start(Rectangle);
    click(tool, b.center());
    QCOMPARE(selected(), QString("B"));
    start(Ellipse);
    click(tool, b.center());
    QCOMPARE(selected(), QString("B"));

    // with a lasso or a polygon it starts the area instead: a tap selects nothing
    start(Lasso);
    click(tool, b.center());
    QCOMPARE(selected(), QString());
    start(Lasso);
    // started on B, around C: B stays where it is and isn't fully inside
    const QVector<QPointF> fromStroke = {b.center(), triangle[1], triangle[2], triangle[0]};
    const QPointF centerBefore = shapes[1]->absolutePosition();
    drag(tool, fromStroke);
    QCOMPARE(selected(), QString("C"));
    QCOMPARE(shapes[1]->absolutePosition(), centerBefore);
    start(Polygon);
    click(tool, b.center());
    QCOMPARE(selected(), QString());
    for (const QPointF &corner : box) click(tool, corner);
    key(tool, Qt::Key_Escape);
    QCOMPARE(selected(), QString());

    // the checkbox: every shape also takes what it only touches
    QCheckBox *touching = shapeCombo->parentWidget()->findChild<QCheckBox*>("chkSelectionTouching");
    QVERIFY(touching);
    const QVector<QPointF> partOfA = around(a.adjusted(40, -5, -40, 5));
    start(Rectangle);
    QVERIFY(touching->isEnabled());
    start(Ellipse);
    QVERIFY(touching->isEnabled());
    start(Lasso);
    QVERIFY(touching->isEnabled());
    drag(tool, partOfA);
    QCOMPARE(selected(), QString());
    touching->setChecked(true);
    start(Lasso);
    drag(tool, partOfA);
    QCOMPARE(selected(), QString("A"));
    start(Polygon);
    QVERIFY(touching->isEnabled());
    const QVector<QPointF> partOfACorners = around(a.adjusted(40, -5, -40, 5), 1);
    for (const QPointF &corner : partOfACorners) click(tool, corner);
    key(tool, Qt::Key_Return);
    QCOMPARE(selected(), QString("A"));
    touching->setChecked(false);
    start(Polygon);
    for (const QPointF &corner : partOfACorners) click(tool, corner);
    key(tool, Qt::Key_Return);
    QCOMPARE(selected(), QString());

    // ticked, a rectangle and an ellipse touch whichever way they are dragged
    const QPointF touchLeft(a.center().x() - 10, a.top() - 5);
    const QPointF touchRight(a.center().x() + 10, a.bottom() + 5);
    const QPointF touchRightTop(a.center().x() + 10, a.top() - 5);
    const QPointF touchLeftBottom(a.center().x() - 10, a.bottom() + 5);
    touching->setChecked(true);
    for (int shape : {int(Rectangle), int(Ellipse)}) {
        start(shape);
        drag(tool, {touchLeft, touchRight});
        QCOMPARE(selected(), QString("A"));
        start(shape);
        drag(tool, {touchRightTop, touchLeftBottom});
        QCOMPARE(selected(), QString("A"));
    }
    // by the line, not its box: this one starts outside C's box and ends in it, away from the slanted line
    start(Rectangle);
    drag(tool, {c.bottomLeft() + QPointF(-10, 10), c.bottomLeft() + QPointF(20, -20)});
    QCOMPARE(selected(), QString());

    // unticked again, the drag direction is back
    touching->setChecked(false);
    for (int shape : {int(Rectangle), int(Ellipse)}) {
        start(shape);
        drag(tool, {touchLeft, touchRight});
        QCOMPARE(selected(), QString());
        start(shape);
        drag(tool, {touchRightTop, touchLeftBottom});
        QCOMPARE(selected(), QString("A"));
    }

    selection->deselectAll();
    image->waitForDone();
    delete view;
    KisPart::instance()->removeMainWindow(window);
    delete window;
    KisPart::instance()->removeDocument(doc);
    QTest::qWait(50);
}

KISTEST_MAIN(VSelectShapesTest)

#include "VSelectShapesTest.moc"
