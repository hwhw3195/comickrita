/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <simpletest.h>

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPointer>
#include <QTimer>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KoCanvasBase.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoPointerEvent.h>
#include <KoSelectedShapesProxy.h>
#include <KoSelection.h>
#include <KoToolManager.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <QtTest/qtestspontaneevent.h>
#include <actions/KisPasteActionFactories.h>
#include <actions/kis_selection_action_factories.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <commands/kis_set_global_selection_command.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_paint_layer.h>
#include <KoColor.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <kis_tool.h>

#include <testui.h>

#include "VCurveShape.h"
#include "VPointEditTool.h"
#include "VWindowTestUtils.h"

/**
 * After a document closes, nothing may keep its image: ~KisDocument checks
 * with a safe assert. Each row does something on a freehand layer, closes the
 * window as quitting does, and checks the image was freed. The clipboard rows
 * ask for every clipboard format after a Copy of layers, as Windows does when
 * Krita quits: application/zip saves the clip in a temporary document, whose
 * image KisDocument::serializeToNativeByteArray() used to keep.
 * The file: rows work on a .kra given by VLIFETIME_KRA and skip without one.
 */
class VImageLifetimeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testImageFreedAtClose_data();
    void testImageFreedAtClose();
};

namespace {
using VWindowTest::encode;

/// a line of 40 points from (\p x, \p y) to the right, as a freehand curve in SVG
QByteArray curveSvg(qreal x, qreal y)
{
    QVector<double> positions, radii;
    QString d;
    for (int i = 0; i < 40; i++) {
        positions << x + i * 4 << y;
        radii << 2;
        d += QString("%1%2,%3 ").arg(i ? "L" : "M").arg(x + i * 4).arg(y);
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

void settle(KisImageSP image)
{
    QTest::qWait(150);
    image->waitForDone();
    QTest::qWait(150);
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
    QSpontaneKeyEvent::setSpontaneous(&release);
    KoPointerEvent end(&release, to);
    tool->endPrimaryAction(&end);
}

QString g_asserts;
QtMessageHandler g_previousHandler = nullptr;

/// keeps the safe asserts, which KRITA_NO_ASSERT_MSG turns into warnings
void catchAsserts(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains("SAFE ASSERT") && !message.contains("this->brush()") && !message.contains("\"brush\"")
        && !message.contains("g_sanityDefaultFormatIsSet")) {
        g_asserts += message + QLatin1Char('\n');
    }
    if (g_previousHandler) g_previousHandler(type, context, message);
}

/// runs deferred deletes until none are left (the application's event loop does this after a close)
void flushDeletes()
{
    for (int i = 0; i < 10; i++) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}
}

void VImageLifetimeTest::testImageFreedAtClose_data()
{
    QTest::addColumn<bool>("fromFile");
    QTest::addColumn<QStringList>("steps");
    QTest::newRow("nothing") << false << QStringList();
    QTest::newRow("paste") << false << QStringList{"paste"};
    QTest::newRow("brush") << false << QStringList{"brush"};
    QTest::newRow("points copy, paste") << false << QStringList{"paste", "pointsCopy"};
    QTest::newRow("shapes copy, paste") << false << QStringList{"paste", "shapesCopy"};
    QTest::newRow("selection copy, paste") << false << QStringList{"paste", "selectionCopy"};
    QTest::newRow("selection cut, paste") << false << QStringList{"paste", "selectionCut"};
    QTest::newRow("brush, selection cut, paste") << false << QStringList{"brush", "selectionCut"};
    // what Windows does when the application quits: it asks for every clipboard format
    QTest::newRow("brush, selection copy, clipboard formats") << false << QStringList{"brush", "selectionCopyOnly", "clipboardFormats"};
    QTest::newRow("raster selection copy, clipboard formats") << false << QStringList{"rasterSelectionCopy", "clipboardFormats"};
    QTest::newRow("transform") << false << QStringList{"paste", "transform"};
    QTest::newRow("transform, undo") << false << QStringList{"paste", "transform", "undo"};
    QTest::newRow("selection transform") << false << QStringList{"paste", "selectionTransform"};
    QTest::newRow("brush, transform") << false << QStringList{"brush", "transform"};
    QTest::newRow("points copy, transform") << false << QStringList{"paste", "pointsCopy", "transform"};
    QTest::newRow("all") << false << QStringList{"paste", "brush", "pointsCopy", "shapesCopy", "selectionCopy", "transform", "selectionTransform"};
    // a user's file, given by VLIFETIME_KRA (skipped without one), with a new freehand layer on it
    QTest::newRow("file: nothing") << true << QStringList();
    QTest::newRow("file: the user's steps") << true << QStringList{"paste", "brush", "pointsCopy", "transform"};
    QTest::newRow("file: all") << true << QStringList{"paste", "brush", "pointsCopy", "shapesCopy", "selectionCopy", "transform", "selectionTransform"};
}

void VImageLifetimeTest::testImageFreedAtClose()
{
    if (!g_previousHandler) g_previousHandler = qInstallMessageHandler(&catchAsserts);
    QFETCH(bool, fromFile);
    QFETCH(QStringList, steps);
    const QString path = QString::fromLocal8Bit(qgetenv("VLIFETIME_KRA"));
    if (fromFile && path.isEmpty()) QSKIP("set VLIFETIME_KRA to a .kra file");

    qputenv("KRITA_NO_ASSERT_MSG", "1");
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, [] () {
        if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
    });
    dismiss.start(50);
    QApplication::clipboard()->setMimeData(new QMimeData());
    // the user's transform settings: Accurate with Instant Preview
    KConfigGroup transformConfig = KSharedConfig::openConfig()->group("KisToolTransform");
    transformConfig.writeEntry("useOverlayPreviewStyle", false);
    transformConfig.writeEntry("forceLodMode", true);

    KisImageSP image;
    KisDocument *doc = nullptr;
    if (fromFile) {
        doc = KisPart::instance()->createDocument();
        KisPart::instance()->addDocument(doc);
        QVERIFY(doc->openPath(path, KisDocument::DontAddToRecent));
        image = doc->image();
    } else {
        image = new KisImage(0, 500, 500, KoColorSpaceRegistry::instance()->rgb8(), "lifetime");
        doc = VWindowTest::addDocument(image);
        // a new document's background
        image->addNode(new KisPaintLayer(image, "background", OPACITY_OPAQUE_U8));
    }
    KisImageWSP watch = image;
    KisShapeLayerSP freehand = new KisShapeLayer(doc->shapeController(), image, "freehand", OPACITY_OPAQUE_U8);
    freehand->setNodeProperty("freehandvector", true);
    image->addNode(freehand);
    image->waitForDone();

    KisView *view = VWindowTest::openWindow(doc);
    KisMainWindow *window = view->mainWindow();
    if (fromFile) VWindowTest::waitForRenders(image, 60000);
    KisViewManager *viewManager = window->viewManager();
    KoToolManager *tools = KoToolManager::instance();
    KisNodeManager *nodes = viewManager->nodeManager();
    KoCanvasBase *canvas = viewManager->canvasBase();
    auto activate = [&] () {
        nodes->slotUiActivatedNode(freehand);
        nodes->slotImageRequestNodeReselection(freehand, {freehand});
        QTest::qWait(50);
    };
    auto firstCurve = [&] () -> VCurveShape* {
        Q_FOREACH (KoShape *shape, freehand->shapes()) {
            if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) return curve;
        }
        return nullptr;
    };
    auto pasteAction = [&] () {
        KisPasteActionFactory().run(KisPasteActionFactory::Flags(), viewManager);
        settle(image);
    };
    activate();

    for (const QString &step : steps) {
        const int before = freehand->shapes().size();
        if (step == "paste") {
            VWindowTest::paste(curveSvg(100, 100), viewManager);
            settle(image);
            QCOMPARE(freehand->shapes().size(), before + 1);
        } else if (step == "brush") {
            KisPaintOpPresetSP preset = KisPaintOpRegistry::instance()->defaultPreset(KoID("paintbrush"),
                                                                                      KisGlobalResourcesInterface::instance());
            QVERIFY(preset);
            canvas->resourceManager()->setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
            tools->switchToolRequested("VStrokeTool");
            QTRY_COMPARE(tools->activeToolId(), QString("VStrokeTool"));
            KisTool *brush = dynamic_cast<KisTool*>(tools->toolById(canvas, "VStrokeTool"));
            QVERIFY(brush);
            drag(brush, QPointF(100, 200), QPointF(220, 240));
            settle(image);
            QCOMPARE(freehand->shapes().size(), before + 1);
        } else if (step == "pointsCopy") {
            tools->switchToolRequested("VPointEditTool");
            QTest::qWait(150);
            VPointEditTool *tool = dynamic_cast<VPointEditTool*>(tools->toolById(canvas, "VPointEditTool"));
            QVERIFY(tool);
            VCurveShape *line = firstCurve();
            QVERIFY(line);
            KoSelection *selection = canvas->selectedShapesProxy()->selection();
            selection->deselectAll();
            selection->select(line);
            tool->selectPoints(line, {10, 11, 12, 13, 14, 15});
            QVERIFY(tool->hasSelection());
            KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
            settle(image);
            pasteAction();
            QCOMPARE(freehand->shapes().size(), before + 1);
        } else if (step == "shapesCopy") {
            tools->switchToolRequested("InteractionTool");
            QTest::qWait(150);
            KoSelection *selection = canvas->selectedShapesProxy()->selection();
            selection->deselectAll();
            selection->select(firstCurve());
            KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
            settle(image);
            pasteAction();
            QCOMPARE(freehand->shapes().size(), before + 1);
        } else if (step == "transform" || step == "selectionTransform") {
            KisSelectionSP selection;
            if (step == "selectionTransform") {
                selection = new KisSelection();
                selection->pixelSelection()->select(QRect(0, 0, 130, 300));
                KisSetGlobalSelectionCommand(image, selection).redo();
                settle(image);
            }
            tools->switchToolRequested("KritaShape/KisToolBrush");
            settle(image);
            activate();
            tools->switchToolRequested("KisToolTransform");
            // settle() would end the stroke: waitForDone() asks the tool to
            QTest::qWait(600);
            KisTool *transform = dynamic_cast<KisTool*>(tools->toolById(canvas, "KisToolTransform"));
            QVERIFY(transform);
            for (int i = 0; i < 3; i++) {
                transform->setProperty("translateX", transform->property("translateX").toDouble() + 20);
                transform->setProperty("translateY", transform->property("translateY").toDouble() + 10);
                QVERIFY(QMetaObject::invokeMethod(transform, "slotEditingFinished"));
                QVERIFY(QMetaObject::invokeMethod(transform, "requestImageRecalculation"));
                QTest::qWait(300);
            }
            QMetaObject::invokeMethod(transform, "requestStrokeEnd");
            settle(image);
            VWindowTest::waitForRenders(image, 20000);
            if (selection) {
                KisSetGlobalSelectionCommand(image, KisSelectionSP()).redo();
                settle(image);
            }
            tools->switchToolRequested("KritaShape/KisToolBrush");
            settle(image);
            activate();
        } else if (step == "selectionCopyOnly" || step == "rasterSelectionCopy") {
            KisNodeSP target = freehand;
            if (step == "rasterSelectionCopy") {
                target = image->root()->firstChild();
                KisPaintLayer *raster = dynamic_cast<KisPaintLayer*>(target.data());
                QVERIFY(raster);
                raster->paintDevice()->fill(QRect(50, 50, 200, 200), KoColor(Qt::red, raster->colorSpace()));
                raster->setDirty();
            }
            tools->switchToolRequested("KritaShape/KisToolLine");
            QTest::qWait(150);
            nodes->slotUiActivatedNode(target);
            nodes->slotImageRequestNodeReselection(target, {target});
            QTest::qWait(50);
            KisSelectionSP selection = new KisSelection();
            selection->pixelSelection()->select(QRect(0, 0, 300, 300));
            KisSetGlobalSelectionCommand(image, selection).redo();
            settle(image);
            KisCutCopyActionFactory().run(KisCutCopyActionFactory::Flags(), viewManager);
            settle(image);
            KisSetGlobalSelectionCommand(image, KisSelectionSP()).redo();
            settle(image);
            activate();
        } else if (step == "clipboardFormats") {
            const QMimeData *data = QApplication::clipboard()->mimeData();
            QVERIFY(data);
            QVERIFY2(data->hasFormat("application/x-krita-node-internal-pointer"), "no layers on the clipboard");
            g_asserts.clear();
            Q_FOREACH (const QString &format, data->formats()) {
                const QByteArray bytes = data->data(format);
                Q_UNUSED(bytes);
                QVERIFY2(!g_asserts.contains("sanityCheckPointer"),
                         qPrintable(QString("asking the clipboard for %1: %2").arg(format, g_asserts)));
            }
            QVERIFY2(g_asserts.isEmpty(), qPrintable("clipboard formats: " + g_asserts));
        } else if (step == "undo") {
            doc->undoStack()->undo();
            settle(image);
        } else if (step == "selectionCopy" || step == "selectionCut") {
            tools->switchToolRequested("KritaShape/KisToolLine");
            QTest::qWait(150);
            activate();
            KisSelectionSP selection = new KisSelection();
            selection->pixelSelection()->select(QRect(0, 0, 300, 300));
            KisSetGlobalSelectionCommand(image, selection).redo();
            settle(image);
            KisCutCopyActionFactory().run(step == "selectionCut" ? KisCutCopyActionFactory::CutClip : KisCutCopyActionFactory::Flags(),
                                          viewManager);
            settle(image);
            KisSetGlobalSelectionCommand(image, KisSelectionSP()).redo();
            settle(image);
            pasteAction();
            activate();
        }
    }
    settle(image);

    // close as quitting does: the window's own close, which closes its views
    // and (deferred) itself; the last view's close removes the document (deleteLater)
    bool heldInDocumentDestructor = false;
    connect(doc, &QObject::destroyed, [&] () { heldInDocumentDestructor = watch.isValid(); });
    QPointer<KisDocument> docAlive = doc;
    QPointer<KisMainWindow> windowAlive = window;
    freehand.clear();
    image.clear();
    doc->setModified(false);
    window->close();
    for (int i = 0; i < 100 && docAlive; i++) {
        flushDeletes();
        QTest::qWait(50);
    }
    flushDeletes();
    if (windowAlive) {
        KisPart::instance()->removeMainWindow(windowAlive);
        delete windowAlive.data();
    }
    QVERIFY2(!docAlive, "the document was not deleted");

    const bool leaked = watch.isValid();
    // the clipboard was left as the steps left it until now: it may be what holds the image
    QApplication::clipboard()->setMimeData(new QMimeData());
    flushDeletes();
    const bool leakedWithoutClipboard = watch.isValid();

    QVERIFY2(!heldInDocumentDestructor && !leaked,
             qPrintable(QString("image held: in ~KisDocument %1, after the close %2, after clearing the clipboard %3")
                        .arg(heldInDocumentDestructor).arg(leaked).arg(leakedWithoutClipboard)));
}

KISTEST_MAIN(VImageLifetimeTest)

#include "VImageLifetimeTest.moc"
