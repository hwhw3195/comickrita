/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeLayerTest.h"

#include <QElapsedTimer>
#include <cstdio>
#include <QtGlobal>
// the stress log's CPU time and heap size: Linux only (zero elsewhere)
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <malloc.h>
#endif

#include <QBuffer>

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDomDocument>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <testui.h>

#include <KisDocument.h>
#include <KisPart.h>
#include <KoColorBackground.h>
#include <KoPathShape.h>
#include <KoProperties.h>
#include <KoShapeGroup.h>
#include <KoShapeRegistry.h>
#include <KoSvgPaste.h>
#include <commands_new/KisMergeLabeledLayersCommand.h>
#include <SvgWriter.h>
#include <commands/kis_image_layer_add_command.h>
#include <kis_command_utils.h>
#include <kis_filter_strategy.h>
#include <kis_mimedata.h>
#include <kis_processing_applicator.h>
#include <kis_shape_controller.h>
#include <kis_shape_layer.h>
#include <KisShapeLayerHooks.h>
#include <KoShapeStroke.h>
#include <kundo2command.h>
#include <testutil.h>
#include <kis_layer_utils.h>
#include <kis_paint_layer.h>
#include <KoColorSpaceRegistry.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <tiles3/kis_tile_data_store.h>

#include "VCurveConverter.h"
#include "VFreehandLayer.h"
#include "VLiquify.h"
#include "VGeomCurveShape.h"
#include "VPointEdit.h"
#include "VPointEditTool.h"
#include "VStrokePresetStore.h"
#include "VCurveTrim.h"
#include "VStrokeShape.h"
#include "VStrokeShapeFactory.h"
#include "VStrokeTool.h"
#include "VStrokeToolSwitcher.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

namespace {

VStrokeShape *strokeShape(KisPaintOpPresetSP preset, qreal pixelsPerPoint, const QPointF &offset, int seed)
{
    VStrokeData data;
    data.preset = preset;
    data.brushSize = 20;
    data.opacity = 0.9;
    data.seed = seed;
    data.perStrokeSeed = seed * 7;

    const QVector<VStrokePrimitive> stroke =
        VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(offset.x(), offset.y()));
    VStrokeShape *shape = VStrokeShape::fromImagePixels(stroke, pixelsPerPoint, data);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    return shape;
}

KoPathShape *pathShape()
{
    KoPathShape *path = new KoPathShape();
    path->setShapeId(KoPathShapeId);
    path->moveTo(QPointF(5, 5));
    path->lineTo(QPointF(5, 55));
    path->lineTo(QPointF(20, 55));
    path->close();
    path->normalize();
    path->setBackground(toQShared(new KoColorBackground(Qt::red)));
    return path;
}

void progress(const QString &message)
{
    // stderr is not buffered: survives the watchdog abort
    std::fprintf(stderr, "[%d renders] %s\n", VStrokeRenderCache::totalRenders(), qPrintable(message));
    std::fflush(stderr);
}

/// TestUtil::MaskParent::waitForImageAndShapeLayers(), but fails instead of hanging
/**
 * As KisImage::waitForDone(), with the GUI thread handling events meanwhile.
 * A job that hands its work to the GUI thread (a shape layer's transform in
 * an image scale) blocks on it unless the GUI thread is already in
 * waitForDone() when the job gets there: if the job is first, a plain
 * waitForDone() waits for it forever.
 */
void waitForImageWithEvents(KisImageSP image)
{
    image->requestStrokeEnd();
    while (!image->tryBarrierLock(true)) {
        QTest::qWait(10);
    }
    image->unlock();
}

bool waitForIdle(TestUtil::MaskParent &p, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    qApp->processEvents();
    waitForImageWithEvents(p.image);
    KisLayerUtils::forceAllDelayedNodesUpdate(p.image->root());
    do {
        QTest::qWait(500);
        if (timer.elapsed() > timeoutMs) {
            progress("image did not become idle");
            return false;
        }
    } while (!p.image->tryBarrierLock(true));
    p.image->unlock();
    return true;
}

/**
 * Process resources over time, written to ~/persistent/vstroke_stress.log
 * when that folder exists (the container mount) and to stderr.
 */
class ResourceLog
{
public:
    ResourceLog() {
        const QString dir = QDir::homePath() + "/persistent";
        if (QDir(dir).exists()) {
            m_file.setFileName(dir + "/vstroke_stress.log");
            m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        }
        m_timer.start();
        line("phase | strokes | renders | wall ms | cpu ms | rss MB | peak rss MB | heap in use MB | tiles | threads | live shapes | live caches | cached image MB");
    }

    void sample(const QString &phase, int strokes, int renders) {
#ifdef Q_OS_LINUX
        rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        const qint64 cpuMs = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000 +
                             (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000;
        // freed memory the allocator keeps is in RSS but not in mallinfo2's in-use bytes
        const struct mallinfo2 heap = mallinfo2();
        const double heapMB = (heap.uordblks + heap.hblkhd) / 1048576.0;
#else
        const qint64 cpuMs = 0;
        const double heapMB = 0.0;
#endif
        line(QString("%1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11 | %12 | %13")
             .arg(phase).arg(strokes).arg(renders).arg(m_timer.elapsed()).arg(cpuMs)
             .arg(status("VmRSS") / 1024.0, 0, 'f', 1).arg(status("VmHWM") / 1024.0, 0, 'f', 1)
             .arg(heapMB, 0, 'f', 1).arg(KisTileDataStore::instance()->numTiles())
             .arg(status("Threads"))
             .arg(VStrokeShape::liveCount()).arg(VStrokeRenderCache::liveCaches())
             .arg(VStrokeRenderCache::liveImageBytes() / 1048576.0, 0, 'f', 1));
    }

    void line(const QString &text) {
        std::fprintf(stderr, "%s\n", qPrintable(text));
        std::fflush(stderr);
        if (m_file.isOpen()) {
            m_file.write(text.toUtf8() + "\n");
            m_file.flush();
        }
    }

private:
    static qint64 status(const char *key) {
        QFile f("/proc/self/status");
        if (!f.open(QIODevice::ReadOnly)) return -1;
        Q_FOREACH (const QByteArray &l, f.readAll().split('\n')) {
            if (l.startsWith(key)) {
                return l.mid(qstrlen(key) + 1).trimmed().split(' ').first().toLongLong();
            }
        }
        return -1;
    }

    QFile m_file;
    QElapsedTimer m_timer;
};

void waitForRenders(KisShapeLayer *layer)
{
    Q_FOREACH (KoShape *shape, layer->shapes()) {
        if (VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(shape)) {
            stroke->waitForRendering();
        }
    }
}

/// two paint devices hold the same picture, within \p fuzzy per channel; empty when they do
QString compareDevices(KisPaintDeviceSP actual, KisPaintDeviceSP expected, const QString &name, int fuzzy = 2)
{
    const QRect ra = actual->exactBounds();
    const QRect re = expected->exactBounds();
    if (re.isEmpty()) return "the expected device is empty";
    if (ra.isEmpty()) return QString("empty, expected pixels in %1,%2 %3x%4").arg(re.x()).arg(re.y()).arg(re.width()).arg(re.height());

    const QRect rc = ra | re;
    const QImage a = actual->convertToQImage(nullptr, rc.x(), rc.y(), rc.width(), rc.height());
    const QImage e = expected->convertToQImage(nullptr, rc.x(), rc.y(), rc.width(), rc.height());
    int failing = 0;
    for (int y = 0; y < rc.height(); y++) {
        const QRgb *la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb *le = reinterpret_cast<const QRgb*>(e.constScanLine(y));
        for (int x = 0; x < rc.width(); x++) {
            if (qAbs(qRed(la[x]) - qRed(le[x])) > fuzzy || qAbs(qGreen(la[x]) - qGreen(le[x])) > fuzzy ||
                qAbs(qBlue(la[x]) - qBlue(le[x])) > fuzzy || qAbs(qAlpha(la[x]) - qAlpha(le[x])) > fuzzy) {
                failing++;
            }
        }
    }
    // a few antialiased edge pixels may differ when a layer re-renders
    if (failing <= re.width() * re.height() / 1000) return QString();

    const QString dir = QDir::homePath() + "/persistent";
    if (QDir(dir).exists()) {
        a.save(dir + "/vstroke_" + name + "_actual.png");
        e.save(dir + "/vstroke_" + name + "_expected.png");
    }
    return QString("%1 pixels differ; bounds %2,%3 %4x%5, expected %6,%7 %8x%9")
        .arg(failing).arg(ra.x()).arg(ra.y()).arg(ra.width()).arg(ra.height())
        .arg(re.x()).arg(re.y()).arg(re.width()).arg(re.height());
}

/// pixels painted in \p actual where \p expected has nothing: what an edit left behind
int leftoverPixels(KisPaintDeviceSP actual, KisPaintDeviceSP expected, QRect *where = nullptr)
{
    const QRect rc = actual->exactBounds() | expected->exactBounds();
    if (rc.isEmpty()) return 0;
    const QImage a = actual->convertToQImage(nullptr, rc.x(), rc.y(), rc.width(), rc.height());
    const QImage e = expected->convertToQImage(nullptr, rc.x(), rc.y(), rc.width(), rc.height());
    int count = 0;
    QRect bounds;
    for (int y = 0; y < rc.height(); y++) {
        const QRgb *la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb *le = reinterpret_cast<const QRgb*>(e.constScanLine(y));
        for (int x = 0; x < rc.width(); x++) {
            if (qAlpha(la[x]) > 24 && qAlpha(le[x]) == 0) {
                count++;
                bounds |= QRect(rc.x() + x, rc.y() + y, 1, 1);
            }
        }
    }
    if (where) *where = bounds;
    return count;
}

void comparePositions(const QVector<VStrokePrimitive> &actual, const QVector<VStrokePrimitive> &expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (int i = 0; i < actual.size(); i++) {
        const QVector<QPointF> a = actual[i].positions();
        const QVector<QPointF> e = expected[i].positions();
        for (int j = 0; j < a.size(); j++) {
            QVERIFY2(QLineF(a[j], e[j]).length() < 1e-3,
                     qPrintable(QString("primitive %1 point %2: (%3, %4) instead of (%5, %6)")
                                .arg(i).arg(j).arg(a[j].x()).arg(a[j].y()).arg(e[j].x()).arg(e[j].y())));
        }
    }
}

/// a document with an image at \p dpi and one vector layer
struct VectorDocument
{
    VectorDocument(qreal dpi, const QRect &bounds = QRect(0, 0, 800, 600))
        : doc(KisPart::instance()->createDocument()),
          p(bounds)
    {
        p.image->setResolution(dpi / 72.0, dpi / 72.0);
        doc->setCurrentImage(p.image);
        layer = new KisShapeLayer(doc->shapeController(), p.image, "vector", 255);
        p.image->addNode(layer);
    }

    /// a stock path and a stroke, rendered
    bool fill(bool withPath = true) {
        if (withPath) layer->addShape(pathShape());
        layer->addShape(strokeShape(pixelBrushPreset(), p.image->xRes(), QPointF(60, 40), 3));
        layer->setDirty();
        return settle();
    }

    bool settle() {
        if (!waitForIdle(p)) return false;
        waitForRenders(layer.data());
        return waitForIdle(p);
    }

    VStrokeShape *stroke() const {
        Q_FOREACH (KoShape *shape, layer->shapes()) {
            if (VStrokeShape *s = dynamic_cast<VStrokeShape*>(shape)) return s;
        }
        return nullptr;
    }

    QScopedPointer<KisDocument> doc;
    TestUtil::MaskParent p;
    KisShapeLayerSP layer;
};

QByteArray saveSvg(const QList<KoShape*> &shapes, const QSizeF &size)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    SvgWriter writer(shapes);
    writer.save(buffer, size);
    return buffer.data();
}

/// as KisShapeLayer::loadLayer() does
QList<KoShape*> loadSvg(const QByteArray &svg, const VectorDocument &d)
{
    QBuffer buffer;
    buffer.setData(svg);
    buffer.open(QIODevice::ReadOnly);
    QSizeF fragmentSize;
    return KisShapeLayer::createShapesFromSvg(&buffer, QString(), d.p.image->bounds(), 72.0 * d.p.image->xRes(),
                                              d.doc->shapeController()->resourceManager(), true, &fragmentSize);
}

/**
 * Adds the node \p makeNode returns to \p image in a stroke job, as the
 * duplicate and paste actions do. \p makeNode runs in that job too: the
 * duplicate action clones there.
 */
KisNodeSP addNodeInStroke(KisImageSP image, std::function<KisNodeSP()> makeNode)
{
    KisNodeSP node;
    KisProcessingApplicator applicator(image, nullptr, KisProcessingApplicator::NONE,
                                       KisImageSignalVector(), kundo2_noi18n("add node"));
    applicator.applyCommand(new KisCommandUtils::LambdaCommand([image, makeNode, &node] () -> KUndo2Command* {
        node = makeNode();
        return new KisImageLayerAddCommand(image, node, image->root(), image->root()->lastChild(), false, false);
    }), KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    applicator.end();
    image->waitForDone();
    return node;
}

/**
 * The stroke as VStrokeTool::commitStroke() makes it: from image pixels,
 * with the live stroke \p device as its rendering, added to \p layer.
 */
VStrokeShape *commitLikeTool(KisShapeLayer *layer, qreal pixelsPerPoint, const QVector<VStrokePrimitive> &primitives,
                             KisPaintDeviceSP device)
{
    VStrokeData data;
    data.preset = pixelBrushPreset();
    data.brushSize = 20;
    data.seed = 5;

    VStrokeShape *shape = VStrokeShape::fromImagePixels(primitives, pixelsPerPoint, data);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    shape->adoptRendering(device);
    layer->addShape(shape);
    return shape;
}

}

void VStrokeLayerTest::initTestCase()
{
    ensurePaintOps();
    if (!KoShapeRegistry::instance()->value(VStrokeShapeId)) {
        // addFactory() also maps the XML element names, add() alone does not
        KoShapeRegistry::instance()->addFactory(new VStrokeShapeFactory());
    }
}

/// stock shapes only: the clone crash was not specific to vector strokes
void VStrokeLayerTest::testClonePlainShapeLayer()
{
    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    TestUtil::MaskParent p(QRect(0, 0, 400, 300));
    p.image->setResolution(100.0 / 72.0, 100.0 / 72.0);
    doc->setCurrentImage(p.image);

    KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), p.image, "vector", 255);
    p.image->addNode(layer);
    layer->addShape(pathShape());
    layer->setDirty();
    QVERIFY(waitForIdle(p));

    KisImageSP clone = p.image->clone(true);
    QVERIFY(clone);
    QVERIFY(waitForIdle(p));
}

void VStrokeLayerTest::testCloneImageForAutosave()
{
    progress(QString("clone test: %1").arg("start"));
    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    progress(QString("clone test: %1").arg("document"));
    TestUtil::MaskParent p(QRect(0, 0, 800, 600));
    // not 72 dpi: the layer copy then resets its cache before it has an image,
    // which made KisImage::clone() delete the half-built image (upstream 6f43b89f81)
    p.image->setResolution(100.0 / 72.0, 100.0 / 72.0);
    doc->setCurrentImage(p.image);
    progress(QString("clone test: %1").arg("image set"));

    KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), p.image, "vector", 255);
    p.image->addNode(layer);
    progress(QString("clone test: %1").arg("layer added"));
    for (int i = 0; i < 5; i++) {
        progress(QString("clone test: %1").arg("adding shape") + QString::number(i));
        layer->addShape(strokeShape(pixelBrushPreset(), p.image->xRes(), QPointF(i * 40, i * 20), i + 1));
    }
    layer->addShape(pathShape());
    layer->setDirty();
    progress(QString("clone test: %1").arg("shapes added"));

    // let the layer paint, the brush renders finish and their updates land
    QVERIFY(waitForIdle(p));
    progress(QString("clone test: %1").arg("first wait done"));
    waitForRenders(layer.data());
    progress(QString("clone test: %1").arg("renders done"));
    QVERIFY(waitForIdle(p));
    progress(QString("clone test: %1").arg("second wait done"));

    // as KisDocument::lockAndCloneForSaving() does on autosave
    for (int i = 0; i < 5; i++) {
        progress(QString("clone test: %1").arg("cloning") + QString::number(i));
        KisImageSP clone = p.image->clone(true);
        QVERIFY(clone);
        QCOMPARE(clone->root()->childCount(), p.image->root()->childCount());
        QVERIFY(waitForIdle(p));
    }

    // a free-transformed layer (as TransformShapeLayerDeferred leaves it), with
    // renders finishing around the clones
    progress("clone test: transforming the layer");
    layer->setTransformation(QTransform::fromScale(1.3, 0.8) * QTransform().rotate(12) * QTransform::fromTranslate(40, 10));
    layer->setDirty();
    for (int i = 0; i < 20; i++) {
        Q_FOREACH (KoShape *shape, layer->shapes()) {
            shape->setTransformation(shape->transformation() * QTransform::fromTranslate(1, 0));
            shape->update();
        }
        QTest::qWait(i * 5);
        progress(QString("clone test: transformed clone %1").arg(i));
        KisImageSP clone = p.image->clone(true);
        QVERIFY(clone);
    }
    QVERIFY(waitForIdle(p));
    progress("clone test: done");
}

void VStrokeLayerTest::testStrokeOnTransformedLayer_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::newRow("72 dpi") << 72.0;
    QTest::newRow("300 dpi") << 300.0;
}

/// a stroke drawn on a free-transformed layer stays where it was drawn
void VStrokeLayerTest::testStrokeOnTransformedLayer()
{
    QFETCH(qreal, dpi);
    VectorDocument d(dpi);
    d.layer->setTransformation(QTransform::fromScale(1.3, 0.8) * QTransform().rotate(12) * QTransform::fromTranslate(40, 10));
    QVERIFY(d.settle());

    // stands in for the live stroke: any pixels will do
    KisPaintDeviceSP live = new KisPaintDevice(d.p.image->colorSpace());
    live->fill(QRect(100, 100, 40, 30), KoColor(Qt::green, live->colorSpace()));

    const QVector<VStrokePrimitive> drawn =
        VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(100, 80));
    const int rendersBefore = VStrokeRenderCache::totalRenders();
    VStrokeShape *shape = commitLikeTool(d.layer.data(), d.p.image->xRes(), drawn, live);
    QVERIFY(d.settle());

    comparePositions(shape->imagePixelPrimitives(), drawn);
    // the live stroke was used as the rendering, not rendered again
    QCOMPARE(VStrokeRenderCache::totalRenders(), rendersBefore);
    QCOMPARE(d.layer->original()->exactBounds(), QRect(100, 100, 40, 30));
}

void VStrokeLayerTest::testDuplicateLayer_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::newRow("72 dpi") << 72.0;
    QTest::newRow("300 dpi") << 300.0;
}

/// Layer > Duplicate: the copy shows the same shapes, stock and vstroke
void VStrokeLayerTest::testDuplicateLayer()
{
    QFETCH(qreal, dpi);
    VectorDocument d(dpi);
    QVERIFY(d.fill());

    KisShapeLayerSP source = d.layer;
    KisNodeSP copy = addNodeInStroke(d.p.image, [source] () { return source->clone(); });
    QVERIFY(copy);
    QVERIFY(waitForIdle(d.p));
    waitForRenders(dynamic_cast<KisShapeLayer*>(copy.data()));
    QVERIFY(waitForIdle(d.p));

    const QString diff = compareDevices(copy->original(), d.layer->original(), "duplicate");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

void VStrokeLayerTest::testPasteLayer_data()
{
    QTest::addColumn<qreal>("sourceDpi");
    QTest::addColumn<qreal>("targetDpi");
    QTest::addColumn<bool>("clipboard");
    QTest::newRow("clipboard, 72 to 72 dpi") << 72.0 << 72.0 << true;
    QTest::newRow("clipboard, 300 to 300 dpi") << 300.0 << 300.0 << true;
    QTest::newRow("clipboard, 300 to 150 dpi") << 300.0 << 150.0 << true;
    QTest::newRow("drag and drop, 300 to 150 dpi") << 300.0 << 150.0 << false;
}

/**
 * Copy a layer into another document. Drag and drop keeps the image pixels;
 * the clipboard (a deep copy without the source image) keeps the size in
 * points, as for stock shapes.
 */
void VStrokeLayerTest::testPasteLayer()
{
    QFETCH(qreal, sourceDpi);
    QFETCH(qreal, targetDpi);
    QFETCH(bool, clipboard);
    VectorDocument source(sourceDpi);
    QVERIFY(source.fill());

    QScopedPointer<QMimeData> mime(clipboard ?
        KisMimeData::mimeForLayersDeepCopy({source.layer}, source.p.image, true) :
        KisMimeData::mimeForLayers({source.layer}, source.p.image));
    QVERIFY(mime);

    VectorDocument target(targetDpi);
    KisShapeController *controller = dynamic_cast<KisShapeController*>(target.doc->shapeController());
    bool copyNode = true;
    const KisNodeList nodes = KisMimeData::loadNodesFast(mime.data(), target.p.image, controller, copyNode);
    QCOMPARE(nodes.size(), 1);
    KisNodeSP pasted = nodes.first();
    addNodeInStroke(target.p.image, [pasted] () { return pasted; });
    KisShapeLayer *pastedLayer = dynamic_cast<KisShapeLayer*>(pasted.data());
    QVERIFY(waitForIdle(target.p));
    waitForRenders(pastedLayer);
    QVERIFY(waitForIdle(target.p));

    VStrokeShape *stroke = nullptr;
    Q_FOREACH (KoShape *shape, pastedLayer->shapes()) {
        if (!stroke) stroke = dynamic_cast<VStrokeShape*>(shape);
    }
    QVERIFY(stroke);
    QVERIFY(qFuzzyCompare(stroke->resolution(), targetDpi / 72.0));

    if (clipboard && sourceDpi != targetDpi) {
        const qreal scale = targetDpi / sourceDpi;
        comparePositions(stroke->imagePixelPrimitives(),
                         VStrokePrimitive::mapped(source.stroke()->imagePixelPrimitives(), QTransform::fromScale(scale, scale)));
        QVERIFY(!pasted->original()->exactBounds().isEmpty());
    } else {
        comparePositions(stroke->imagePixelPrimitives(), source.stroke()->imagePixelPrimitives());
        const QString diff = compareDevices(pasted->original(), source.layer->original(), "paste_layer");
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }
}

void VStrokeLayerTest::testMergeLabeledLayersReference_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::newRow("72 dpi") << 72.0;
    QTest::newRow("300 dpi") << 300.0;
}

/// fill tools with "reference: color labeled layers" see vector layers
void VStrokeLayerTest::testMergeLabeledLayersReference()
{
    QFETCH(qreal, dpi);
    VectorDocument d(dpi);
    QVERIFY(d.fill());
    d.layer->setColorLabelIndex(1);

    // as the fill tools run it: in a stroke job
    KisPaintDeviceSP reference = KisMergeLabeledLayersCommand::createRefPaintDevice(d.p.image, "reference");
    KisProcessingApplicator applicator(d.p.image, nullptr, KisProcessingApplicator::NONE,
                                       KisImageSignalVector(), kundo2_noi18n("fill"));
    applicator.applyCommand(new KisMergeLabeledLayersCommand(d.p.image, reference, {1}),
                            KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    applicator.end();
    d.p.image->waitForDone();

    const QString diff = compareDevices(reference, d.layer->original(), "labeled_reference");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

void VStrokeLayerTest::testPasteStroke_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::addColumn<bool>("grouped");
    QTest::newRow("72 dpi") << 72.0 << false;
    QTest::newRow("300 dpi") << 300.0 << false;
    QTest::newRow("300 dpi, in a group") << 300.0 << true;
}

/// copy one stroke and paste it into another vector layer
void VStrokeLayerTest::testPasteStroke()
{
    QFETCH(qreal, dpi);
    QFETCH(bool, grouped);
    VectorDocument d(dpi);
    QVERIFY(d.fill(false));
    VStrokeShape *original = d.stroke();
    QVERIFY(original);

    // KoDrag::setSvg(), then KisPasteActionFactory with KoSvgPaste
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        KoShape *baked = original->cloneShapeAndBakeAbsoluteTransform();
        QScopedPointer<KoShape> top(baked);
        if (grouped) {
            KoShapeGroup *group = new KoShapeGroup();
            group->addShape(baked);
            top.take();
            top.reset(group);
        }
        const QRectF bounds = original->boundingRect();
        SvgWriter writer({top.data()});
        writer.save(buffer, QSizeF(bounds.right(), bounds.bottom()));
    }
    const QList<KoShape*> shapes = KoSvgPaste::fetchShapesFromData(buffer.data(), d.p.image->bounds(),
                                                                   72.0 * d.p.image->xRes());
    QCOMPARE(shapes.size(), 1);
    VStrokeShape *pasted = dynamic_cast<VStrokeShape*>(shapes.first());
    if (grouped) {
        KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shapes.first());
        QVERIFY(group);
        QCOMPARE(group->shapeCount(), 1);
        pasted = dynamic_cast<VStrokeShape*>(group->shapes().first());
    }
    QVERIFY(pasted);

    KisShapeLayerSP layer2 = new KisShapeLayer(d.doc->shapeController(), d.p.image, "vector 2", 255);
    d.p.image->addNode(layer2);
    layer2->addShape(shapes.first());

    QVERIFY2(qFuzzyCompare(pasted->resolution(), original->resolution()),
             qPrintable(QString("pasted at %1 dpi").arg(72.0 * pasted->resolution())));
    comparePositions(pasted->imagePixelPrimitives(), original->imagePixelPrimitives());

    // drawn with the same brush settings, not with a preset found by name
    QCOMPARE(pasted->data().preset, original->data().preset);
    QVERIFY(waitForIdle(d.p));
    waitForRenders(layer2.data());
    QVERIFY(waitForIdle(d.p));
    const QString diff = compareDevices(layer2->original(), d.layer->original(), "paste_stroke");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

/// strokes drawn with the same settings share one preset, which goes away with the last of them
void VStrokeLayerTest::testPresetSharing()
{
    const int liveBefore = VStrokePresetStore::instance()->liveCount();
    {
        VectorDocument d(300.0);
        KisPaintOpPresetSP pixel = pixelBrushPreset();
        // separate snapshots of the same settings, as every new stroke of the tool has
        VStrokeShape *a = strokeShape(pixel, d.p.image->xRes(), QPointF(0, 0), 1);
        VStrokeShape *b = strokeShape(pixel->clone().dynamicCast<KisPaintOpPreset>(), d.p.image->xRes(), QPointF(50, 0), 2);
        VStrokeShape *c = strokeShape(cspBrushPreset(), d.p.image->xRes(), QPointF(0, 50), 3);
        d.layer->addShape(a);
        d.layer->addShape(b);
        d.layer->addShape(c);

        QVERIFY(a->data().presetDefinition);
        QCOMPARE(a->data().presetDefinition, b->data().presetDefinition);
        QCOMPARE(a->data().preset, b->data().preset);
        QVERIFY(a->data().presetDefinition != c->data().presetDefinition);
        QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore + 2);

        // a changed brush size is other settings
        KisPaintOpPresetSP bigger = pixel->clone().dynamicCast<KisPaintOpPreset>();
        bigger->settings()->setPaintOpSize(40);
        QScopedPointer<VStrokeShape> d2(strokeShape(bigger, d.p.image->xRes(), QPointF(50, 50), 4));
        QVERIFY(d2->data().presetDefinition != a->data().presetDefinition);
        d2.reset();
        QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore + 2);

        // deleting one stroke keeps the preset of the other
        d.layer->removeShape(a);
        delete a;
        QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore + 2);
        QVERIFY(d.settle());
    }
    QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore);
}

/// a saved layer holds each preset once; loading it renders the same pixels
void VStrokeLayerTest::testSavePresetDefinitions()
{
    const int liveBefore = VStrokePresetStore::instance()->liveCount();
    VectorDocument d(300.0);
    KisPaintOpPresetSP presets[] = {pixelBrushPreset(), cspBrushPreset()};
    for (int i = 0; i < 6; i++) {
        d.layer->addShape(strokeShape(presets[i % 2], d.p.image->xRes(), QPointF(i * 60, i * 30), i + 1));
    }
    QVERIFY(d.settle());
    KisPaintDeviceSP pixelsBefore = new KisPaintDevice(*d.layer->original());

    const QByteArray svg = saveSvg(d.layer->shapes(), d.p.image->bounds().size());
    QCOMPARE(svg.count("<krita:vstroke-preset "), 2);
    QCOMPARE(svg.count("krita:preset-ref=\""), 6);
    QVERIFY(svg.indexOf("<krita:vstroke-preset ") < svg.indexOf("krita:preset-ref="));

    // without the strokes, their presets are gone: loading cannot reuse them
    Q_FOREACH (KoShape *shape, d.layer->shapes()) {
        d.layer->removeShape(shape);
        delete shape;
    }
    QVERIFY(d.settle());
    QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore);

    const QList<KoShape*> loaded = loadSvg(svg, d);
    QCOMPARE(loaded.size(), 6);
    QCOMPARE(VStrokePresetStore::instance()->liveCount(), liveBefore + 2);
    Q_FOREACH (KoShape *shape, loaded) {
        VStrokeShape *stroke = dynamic_cast<VStrokeShape*>(shape);
        QVERIFY(stroke);
        QVERIFY(stroke->data().preset);
        d.layer->addShape(shape);
    }
    d.layer->setDirty();
    QVERIFY(d.settle());

    const QString diff = compareDevices(d.layer->original(), pixelsBefore, "reload_presets");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));

    // saving again writes the same definitions
    const QByteArray svg2 = saveSvg(d.layer->shapes(), d.p.image->bounds().size());
    QCOMPARE(svg2.count("<krita:vstroke-preset "), 2);
}

/// a preset of a missing brush engine survives loading and saving again
void VStrokeLayerTest::testMissingPaintop()
{
    VectorDocument d(300.0);
    QVERIFY(d.fill(false));
    QByteArray svg = saveSvg(d.layer->shapes(), d.p.image->bounds().size());
    QVERIFY(svg.contains("paintopid=\"paintbrush\""));
    svg.replace("paintopid=\"paintbrush\"", "paintopid=\"vstroke_test_missing\"");

    const QList<KoShape*> loaded = loadSvg(svg, d);
    QCOMPARE(loaded.size(), 1);
    QScopedPointer<VStrokeShape> stroke(dynamic_cast<VStrokeShape*>(loaded.first()));
    QVERIFY(stroke);
    QVERIFY(!stroke->data().preset);
    QVERIFY(stroke->data().presetDefinition);

    auto presetElement = [] (const QByteArray &data) {
        QDomDocument doc;
        doc.setContent(data);
        QDomElement e = doc.documentElement().firstChildElement("defs").firstChildElement("krita:vstroke-preset");
        QString text;
        QTextStream stream(&text);
        e.firstChildElement("Preset").save(stream, -1);
        return text;
    };
    const QByteArray svg2 = saveSvg({stroke.data()}, d.p.image->bounds().size());
    QVERIFY(!presetElement(svg).isEmpty());
    QCOMPARE(presetElement(svg2), presetElement(svg));
}

/// Edit > Clear on a vector layer: everything, or what the selection covers
void VStrokeLayerTest::testClearLayer()
{
    VectorDocument d(300.0);
    QVERIFY(d.fill());
    VStrokeShape *stroke = d.stroke();
    QVERIFY(stroke);

    QCOMPARE(VStrokeTool::shapesToClear(d.layer.data(), KisSelectionSP()).size(), 2);

    auto selectionOf = [] (const QRect &rect) {
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(rect);
        return selection;
    };
    const QRect strokePixels = QTransform::fromScale(d.p.image->xRes(), d.p.image->yRes())
            .mapRect(stroke->absoluteOutlineRect()).toAlignedRect();

    QCOMPARE(VStrokeTool::shapesToClear(d.layer.data(), selectionOf(d.p.image->bounds())).size(), 2);

    const QList<KoShape*> covered = VStrokeTool::shapesToClear(d.layer.data(), selectionOf(strokePixels.adjusted(-2, -2, 2, 2)));
    QCOMPARE(covered.size(), 1);
    QCOMPARE(covered.first(), static_cast<KoShape*>(stroke));

    // partly covered strokes stay
    QVERIFY(VStrokeTool::shapesToClear(d.layer.data(), selectionOf(strokePixels.adjusted(10, 10, -10, -10))).isEmpty());
    const QRect leftHalf(strokePixels.topLeft(), QSize(strokePixels.width() / 2, strokePixels.height()));
    QVERIFY(VStrokeTool::shapesToClear(d.layer.data(), selectionOf(leftHalf)).isEmpty());
}

/// a freehand layer names its own icon; a stock one the stock icon, by an empty name
void VStrokeLayerTest::testLayerIcon()
{
    VFreehandLayer::installHooks();
    VectorDocument d(72.0);
    QCOMPARE(KisShapeLayerHooks::layerIcon(d.layer.data()), QString());
    VFreehandLayer::setFreehand(d.layer.data());
    QCOMPARE(KisShapeLayerHooks::layerIcon(d.layer.data()), QString("freehandVectorLayer"));
}

/// a color-mixing brush draws vector strokes as if mixing were off
void VStrokeLayerTest::testColorMixingPreset()
{
    const QString key = "CSP/BrushUseWaterColor";
    KisPaintOpPresetSP plain = cspBrushPreset();
    QVERIFY(plain);
    QVERIFY(!plain->settings()->getBool(key, false));
    QCOMPARE(VStrokeTool::vectorPreset(plain), plain);
    QCOMPARE(VStrokeTool::vectorPreset(pixelBrushPreset())->paintOp().id(), QString("paintbrush"));

    KisPaintOpPresetSP mixing = plain->clone().dynamicCast<KisPaintOpPreset>();
    mixing->settings()->setProperty(key, true);
    QVERIFY(VStrokeTool::unsupportedPresetReason(mixing).isEmpty());
    QVERIFY(VStrokeTool::unsupportedPresetReason(pixelBrushPreset()).isEmpty());

    // other engines are refused: they would draw, but ignore a point's thickness
    for (const char *id : {"mypaintbrush", "colorsmudge", "sketchbrush"}) {
        KisPaintOpPresetSP other = pixelBrushPreset();
        other->setPaintOp(KoID(id));
        QVERIFY2(!VStrokeTool::unsupportedPresetReason(other).isEmpty(), id);
    }

    KisPaintOpPresetSP vector = VStrokeTool::vectorPreset(mixing);
    QVERIFY(vector != mixing);
    QVERIFY(!vector->settings()->getBool(key, true));
    QVERIFY(mixing->settings()->getBool(key, false)); // the user's preset is unchanged

    // draws as the same brush without mixing would
    VectorDocument a(300.0);
    VectorDocument b(300.0);
    a.layer->addShape(strokeShape(vector, a.p.image->xRes(), QPointF(60, 40), 3));
    b.layer->addShape(strokeShape(plain, b.p.image->xRes(), QPointF(60, 40), 3));
    a.layer->setDirty();
    b.layer->setDirty();
    QVERIFY(a.settle());
    QVERIFY(b.settle());
    QVERIFY(!a.layer->original()->exactBounds().isEmpty());
    const QString diff = compareDevices(a.layer->original(), b.layer->original(), "color-mixing");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));

    // and is saved without mixing
    const QList<KoShape*> loaded = loadSvg(saveSvg(a.layer->shapes(), a.p.image->bounds().size()), a);
    QCOMPARE(loaded.size(), 1);
    QScopedPointer<VStrokeShape> stroke(dynamic_cast<VStrokeShape*>(loaded.first()));
    QVERIFY(stroke && stroke->data().preset);
    QVERIFY(!stroke->data().preset->settings()->getBool(key, true));
}

void VStrokeLayerTest::testLiveOverlay_data()
{
    QTest::addColumn<bool>("erase");
    QTest::newRow("draw") << false;
    QTest::newRow("erase") << true;
}

/// the live stroke paints, or erases, inside its layer; the committed shape replaces it
void VStrokeLayerTest::testLiveOverlay()
{
    QFETCH(bool, erase);
    VectorDocument d(300.0);
    QVERIFY(d.fill(false));
    const QRect strokePixels = d.layer->original()->exactBounds();
    QVERIFY(!strokePixels.isEmpty());
    const QPoint inside = strokePixels.center();
    KoColor before;
    d.layer->original()->pixel(inside.x(), inside.y(), &before);
    QVERIFY(before.opacityU8() > 0);

    // the live stroke covers the middle of the existing stroke
    const QRect live(inside - QPoint(20, 20), QSize(40, 40));
    VStrokeTool::LiveStroke stroke;
    stroke.layer = d.layer;
    stroke.device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    stroke.device->fill(live, KoColor(Qt::green, stroke.device->colorSpace()));
    stroke.compositeOp = erase ? COMPOSITE_ERASE : COMPOSITE_OVER;

    d.layer->setLiveOverlay(VStrokeTool::liveOverlay({stroke}));
    d.layer->updateLiveOverlay(live);
    QVERIFY(d.settle());

    KoColor pixel;
    d.layer->original()->pixel(inside.x(), inside.y(), &pixel);
    if (erase) {
        QCOMPARE(pixel.opacityU8(), quint8(0));
    } else {
        QCOMPARE(pixel.toQColor(), QColor(Qt::green));
    }
    if (!erase) {
        // the committed shape replaces the overlay: same pixels
        const QVector<VStrokePrimitive> drawn = VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(inside.x() - 20, inside.y() - 20));
        commitLikeTool(d.layer.data(), d.p.image->xRes(), drawn, stroke.device);
    }
    d.layer->setLiveOverlay(KisShapeLayer::LiveOverlay());
    d.layer->updateLiveOverlay(live);
    QVERIFY(d.settle());

    d.layer->original()->pixel(inside.x(), inside.y(), &pixel);
    if (erase) {
        // no shape was committed: the stroke below is back
        QCOMPARE(pixel.toQColor(), before.toQColor());
    } else {
        QCOMPARE(pixel.toQColor(), QColor(Qt::green));
    }
}

/// a live stroke blends with what is below it in its layer, as the committed one will
void VStrokeLayerTest::testLiveOverlayBlends()
{
    VectorDocument d(300.0);
    const QRect live(100, 100, 40, 40);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    VStrokeTool::LiveStroke below;
    below.layer = d.layer;
    below.device = new KisPaintDevice(cs);
    below.device->fill(live, KoColor(QColor(200, 200, 200), cs));

    VStrokeTool::LiveStroke stroke = below;
    stroke.device = new KisPaintDevice(cs);
    stroke.device->fill(live, KoColor(QColor(128, 128, 128), cs));
    stroke.compositeOp = COMPOSITE_MULT;

    d.layer->setLiveOverlay(VStrokeTool::liveOverlay({below, stroke}));
    d.layer->updateLiveOverlay(live);
    QVERIFY(d.settle());

    KoColor pixel;
    d.layer->original()->pixel(120, 120, &pixel);
    QVERIFY2(qAbs(pixel.toQColor().blue() - 100) <= 2, qPrintable(QString::number(pixel.toQColor().blue())));

    d.layer->setLiveOverlay(KisShapeLayer::LiveOverlay());
    d.layer->updateLiveOverlay(live);
    QVERIFY(d.settle());
}

/// a committed stroke blends with the strokes below it on the layer, not with other layers
void VStrokeLayerTest::testCommittedStrokeBlends()
{
    VectorDocument d(72.0);
    const QRect area(100, 100, 40, 40);
    const QVector<VStrokePrimitive> drawn = VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(100, 100));
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    KisPaintDeviceSP lower = new KisPaintDevice(cs);
    lower->fill(area, KoColor(QColor(200, 200, 200), cs));
    commitLikeTool(d.layer.data(), d.p.image->xRes(), drawn, lower);

    VStrokeData data;
    data.preset = pixelBrushPreset();
    data.brushSize = 20;
    data.compositeOp = COMPOSITE_MULT;
    VStrokeShape *upper = VStrokeShape::fromImagePixels(drawn, d.p.image->xRes(), data);
    upper->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(128, 128, 128))));
    KisPaintDeviceSP upperPixels = new KisPaintDevice(cs);
    upperPixels->fill(area, KoColor(QColor(128, 128, 128), cs));
    upper->adoptRendering(upperPixels);
    upper->setZIndex(1); // above the first, as the tool's addShape() makes it
    d.layer->addShape(upper);
    d.layer->setDirty();
    QVERIFY(d.settle());

    KoColor pixel;
    d.layer->original()->pixel(120, 120, &pixel);
    QVERIFY2(qAbs(pixel.toQColor().blue() - 100) <= 2, qPrintable(QString::number(pixel.toQColor().blue())));
}

/// a mode QPainter doesn't have, live and committed, on the layer's real image
void VStrokeLayerTest::testKritaBlendModeOnLayer()
{
    VectorDocument d(72.0);
    const QRect area(100, 100, 40, 40);
    const QVector<VStrokePrimitive> drawn = VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(100, 100));
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    KisPaintDeviceSP lowerPixels = new KisPaintDevice(cs);
    lowerPixels->fill(area, KoColor(QColor(100, 100, 100), cs));
    commitLikeTool(d.layer.data(), d.p.image->xRes(), drawn, lowerPixels);

    KisPaintDeviceSP upperPixels = new KisPaintDevice(cs);
    upperPixels->fill(area, KoColor(QColor(200, 200, 200), cs));

    const int divided = 128; // 100 / 200
    auto pixelBlue = [&] () {
        KoColor pixel;
        d.layer->original()->pixel(120, 120, &pixel);
        return pixel.toQColor().blue();
    };

    VStrokeTool::LiveStroke live;
    live.layer = d.layer;
    live.device = upperPixels;
    live.compositeOp = COMPOSITE_DIVIDE;
    d.layer->setDirty();
    QVERIFY(d.settle());
    d.layer->setLiveOverlay(VStrokeTool::liveOverlay({live}));
    d.layer->updateLiveOverlay(area);
    QVERIFY(d.settle());
    QVERIFY2(qAbs(pixelBlue() - divided) <= 2, qPrintable(QString::number(pixelBlue())));
    d.layer->setLiveOverlay(KisShapeLayer::LiveOverlay());
    d.layer->updateLiveOverlay(area);
    QVERIFY(d.settle());
    QVERIFY2(qAbs(pixelBlue() - 100) <= 1, qPrintable(QString::number(pixelBlue())));

    VStrokeData data;
    data.preset = pixelBrushPreset();
    data.brushSize = 20;
    data.compositeOp = COMPOSITE_DIVIDE;
    VStrokeShape *upper = VStrokeShape::fromImagePixels(drawn, d.p.image->xRes(), data);
    upper->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(200, 200, 200))));
    upper->adoptRendering(upperPixels);
    upper->setZIndex(1); // above the first, as the tool's addShape() makes it
    d.layer->addShape(upper);
    d.layer->setDirty();
    QVERIFY(d.settle());
    QVERIFY2(qAbs(pixelBlue() - divided) <= 2, qPrintable(QString::number(pixelBlue())));
}

/// a live stroke on a lower layer stays under the layers above it
void VStrokeLayerTest::testLiveOverlayLayerOrder()
{
    VectorDocument d(300.0);
    KisPaintLayerSP above = new KisPaintLayer(d.p.image, "above", OPACITY_OPAQUE_U8);
    above->paintDevice()->fill(QRect(0, 0, 200, 200), KoColor(Qt::blue, above->colorSpace()));
    d.p.image->addNode(above);
    QVERIFY(d.settle());

    VStrokeTool::LiveStroke stroke;
    stroke.layer = d.layer;
    stroke.device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    stroke.device->fill(QRect(100, 100, 200, 200), KoColor(Qt::green, stroke.device->colorSpace()));
    d.layer->setLiveOverlay(VStrokeTool::liveOverlay({stroke}));
    d.layer->updateLiveOverlay(stroke.device->extent());
    QVERIFY(d.settle());

    KoColor pixel;
    d.layer->original()->pixel(150, 150, &pixel);
    QCOMPARE(pixel.toQColor(), QColor(Qt::green));
    d.p.image->projection()->pixel(150, 150, &pixel);
    QCOMPARE(pixel.toQColor(), QColor(Qt::blue));
    d.p.image->projection()->pixel(250, 250, &pixel);
    QCOMPARE(pixel.toQColor(), QColor(Qt::green));
}

/// the brush button draws on vector layers through the vector brush tool
void VStrokeLayerTest::testFreehandFlagSaved()
{
    VectorDocument d(300);
    QVERIFY(d.fill());
    d.layer->setNodeProperty("freehandvector", true);
    KisShapeLayerSP plain = new KisShapeLayer(d.doc->shapeController(), d.p.image, "plain", 255);
    d.p.image->addNode(plain);
    QVERIFY(d.settle());

    // Duplicate and in-process copies clone the node, properties included
    QVERIFY(d.layer->clone()->nodeProperties().boolProperty("freehandvector", false));

    QTemporaryDir dir;
    const QString path = dir.filePath("freehand.kra");
    QVERIFY(d.doc->exportDocumentSync(path, "application/x-krita"));

    QScopedPointer<KisDocument> loaded(KisPart::instance()->createDocument());
    QVERIFY(loaded->loadNativeFormat(path));
    KisNodeSP vector = TestUtil::findNode(loaded->image()->root(), "vector");
    KisNodeSP stock = TestUtil::findNode(loaded->image()->root(), "plain");
    QVERIFY(vector && stock);
    QVERIFY(vector->nodeProperties().boolProperty("freehandvector", false));
    QVERIFY(!stock->nodeProperties().boolProperty("freehandvector", false));
}

namespace {
template <class T>
QList<T*> shapesOf(KisShapeLayer *layer)
{
    QList<T*> result;
    Q_FOREACH (KoShape *shape, layer->shapes()) {
        if (T *t = dynamic_cast<T*>(shape)) result << t;
    }
    return result;
}
}

void VStrokeLayerTest::testFreehandHooks()
{
    VFreehandLayer::installHooks();
    VectorDocument d(72);
    VFreehandLayer::setFreehand(d.layer.data());
    KisShapeLayerSP plain = new KisShapeLayer(d.doc->shapeController(), d.p.image, "plain", 255);

    // the shape tools, paste and drop: stock paths convert on freehand layers only
    QList<KoShape*> shapes = {pathShape()};
    KisShapeLayerHooks::convertShapes(shapes, plain.data());
    QVERIFY(!dynamic_cast<VGeomCurveShape*>(shapes[0]));
    KisShapeLayerHooks::convertShapes(shapes, nullptr);
    QVERIFY(!dynamic_cast<VGeomCurveShape*>(shapes[0]));
    KisShapeLayerHooks::convertShapes(shapes, d.layer.data());
    QVERIFY(dynamic_cast<VGeomCurveShape*>(shapes[0]));

    // pasted freehand curves need a freehand layer; stock shapes do not
    QList<KoShape*> stock = {pathShape()};
    QVERIFY(KisShapeLayerHooks::needsNewLayer(shapes, plain.data()));
    QVERIFY(!KisShapeLayerHooks::needsNewLayer(shapes, d.layer.data()));
    QVERIFY(!KisShapeLayerHooks::needsNewLayer(stock, plain.data()));
    QScopedPointer<KoShape> stroke(strokeShape(pixelBrushPreset(), 1.0, QPointF(10, 10), 1));
    QVERIFY(VFreehandLayer::containsCurves({stroke.data()}));

    KisShapeLayerSP added = new KisShapeLayer(d.doc->shapeController(), d.p.image, "Vector Layer 3", 255);
    KisShapeLayerHooks::initNewLayer(added.data(), stock);
    QVERIFY(!VFreehandLayer::isFreehand(added.data()));
    KisShapeLayerHooks::initNewLayer(added.data(), shapes);
    QVERIFY(VFreehandLayer::isFreehand(added.data()));
    QCOMPARE(static_cast<KisNode*>(added.data())->name(), QString("Freehand Vector Layer 3"));

    qDeleteAll(shapes);
    qDeleteAll(stock);
}

void VStrokeLayerTest::testLiquifyCommands()
{
    // at 144 dpi: dabs come in image pixels, curves are in points
    VFreehandLayer::installHooks();
    VectorDocument d(144);
    VFreehandLayer::setFreehand(d.layer.data());
    KisShapeLayerSP plain = new KisShapeLayer(d.doc->shapeController(), d.p.image, "plain", 255);
    QList<KoShape*> shapes = {pathShape()};
    KisShapeLayerHooks::convertShapes(shapes, d.layer.data());
    VCurveShape *curve = dynamic_cast<VCurveShape*>(shapes[0]);
    QVERIFY(curve);
    d.layer->addShape(curve);
    QVERIFY(d.settle());

    QVERIFY(KisShapeLayerHooks::isLiquifyTarget(d.layer.data()));
    QVERIFY(!KisShapeLayerHooks::isLiquifyTarget(plain.data()));

    const VCurveData original = curve->curves();
    const QPointF point = curve->absoluteTransformation().map(original.positions[0]);
    const QPointF pointInPixels = point * 2.0;
    const QVector<KisShapeLayerHooks::LiquifyDab> dabs = {{pointInPixels, QPointF(10, 0), 30.0, false, 1.0}};

    // the preview pass and the final pass of one stroke: the dabs apply once
    const quint64 stroke = KisShapeLayerHooks::newLiquifyStroke();
    QScopedPointer<KUndo2Command> preview(KisShapeLayerHooks::liquifyCommand(d.layer.data(), dabs, stroke));
    QScopedPointer<KUndo2Command> final(KisShapeLayerHooks::liquifyCommand(d.layer.data(), dabs, stroke));
    QVERIFY(preview && final);
    preview->redo();
    const VCurveData once = curve->curves();
    QVERIFY(once != original);
    // the point under the dab's center moved by the whole offset: 10 px, 5 pt
    QVERIFY(QLineF(curve->absoluteTransformation().map(once.positions[0]), point + QPointF(5, 0)).length() < 1e-6);
    QCOMPARE(once.pointCount(), original.pointCount());
    // while the stroke runs, a pass's undo leaves the curves to the next pass
    preview->undo();
    QVERIFY(curve->curves() == once);
    final->redo();
    QVERIFY(curve->curves() == once);
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), true);

    // after the stroke, the last command holds the change; the preview's undo changes nothing
    final->undo();
    QVERIFY(curve->curves() == original);
    preview->undo();
    QVERIFY(curve->curves() == original);
    final->redo();
    QVERIFY(curve->curves() == once);

    // the next stroke starts from the applied one and leaves it applied
    QScopedPointer<KUndo2Command> next(KisShapeLayerHooks::liquifyCommand(d.layer.data(), dabs,
                                                                          KisShapeLayerHooks::newLiquifyStroke()));
    next->redo();
    const VCurveData twice = curve->curves();
    QVERIFY(twice != once);
    // pushed further: the dab is 5 pt from the point now
    QVERIFY(curve->absoluteTransformation().map(twice.positions[0]).x() > point.x() + 9);
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), true);
    next->undo();
    QVERIFY(curve->curves() == once);

    final->undo();
    QVERIFY(curve->curves() == original);

    // a stroke that ends without keeping its commands: the curves go back
    QScopedPointer<KUndo2Command> dropped(KisShapeLayerHooks::liquifyCommand(d.layer.data(), dabs,
                                                                             KisShapeLayerHooks::newLiquifyStroke()));
    dropped->redo();
    QVERIFY(curve->curves() != original);
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), false);
    QVERIFY(curve->curves() == original);
}

void VStrokeLayerTest::testLiquifyLatePreviewPass()
{
    VFreehandLayer::installHooks();
    VectorDocument d(144);
    VFreehandLayer::setFreehand(d.layer.data());
    QList<KoShape*> shapes = {pathShape()};
    KisShapeLayerHooks::convertShapes(shapes, d.layer.data());
    VCurveShape *curve = dynamic_cast<VCurveShape*>(shapes[0]);
    QVERIFY(curve);
    d.layer->addShape(curve);
    QVERIFY(d.settle());

    const VCurveData original = curve->curves();
    const QPointF pointInPixels = curve->absoluteTransformation().map(original.positions[0]) * 2.0;
    QVector<KisShapeLayerHooks::LiquifyDab> early;
    for (int i = 0; i < 3; i++) early << KisShapeLayerHooks::LiquifyDab{pointInPixels, QPointF(10, 0), 30.0, false, 1.0};
    QVector<KisShapeLayerHooks::LiquifyDab> latest = early;
    for (int i = 0; i < 5; i++) latest << KisShapeLayerHooks::LiquifyDab{pointInPixels, QPointF(10, 0), 30.0, false, 1.0};

    // the pen moved on while a pass with the earlier dabs waited for its turn
    VLiquify::preview(d.layer.data(), latest);
    const VCurveData moved = curve->curves();
    QVERIFY(moved != original);
    const quint64 stroke = KisShapeLayerHooks::newLiquifyStroke();
    QScopedPointer<KUndo2Command> late(KisShapeLayerHooks::liquifyCommand(d.layer.data(), early, stroke));
    late->redo();
    QVERIFY(curve->curves() == moved);

    // the next moves go on from there, as do the stroke's last pass and its end
    latest << KisShapeLayerHooks::LiquifyDab{pointInPixels, QPointF(10, 0), 30.0, false, 1.0};
    VLiquify::preview(d.layer.data(), latest);
    const VCurveData further = curve->curves();
    QVERIFY(further != moved);
    QScopedPointer<KUndo2Command> last(KisShapeLayerHooks::liquifyCommand(d.layer.data(), latest, stroke));
    last->redo();
    QVERIFY(curve->curves() == further);
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), true);
    QVERIFY(curve->curves() == further);

    last->undo();
    QVERIFY(curve->curves() == original);
    last->redo();
    QVERIFY(curve->curves() == further);
    last->undo();
    late->undo();
    QVERIFY(curve->curves() == original);
}

void VStrokeLayerTest::testLiquifyUndoKeepsUntouchedStrokes()
{
    VFreehandLayer::installHooks();
    VectorDocument d(72);
    VFreehandLayer::setFreehand(d.layer.data());
    VStrokeShape *a = strokeShape(pixelBrushPreset(), d.p.image->xRes(), QPointF(60, 40), 3);
    VStrokeShape *b = strokeShape(pixelBrushPreset(), d.p.image->xRes(), QPointF(500, 300), 4);
    d.layer->addShape(a);
    d.layer->addShape(b);
    d.layer->setDirty();
    QVERIFY(d.settle());

    auto drag = [] (VStrokeShape *stroke) {
        const QPointF at = stroke->renderTransform().map(stroke->curves().positions.first());
        return QVector<KisShapeLayerHooks::LiquifyDab>(3, KisShapeLayerHooks::LiquifyDab{at, QPointF(10, 0), 30.0, false, 1.0});
    };
    const QVector<KisShapeLayerHooks::LiquifyDab> first = drag(a);
    const QVector<KisShapeLayerHooks::LiquifyDab> both = first + drag(b);
    const VCurveData aStart = a->curves();
    const VCurveData bStart = b->curves();

    // two drags of one Liquify session, each moving one stroke
    const int rendersBefore = VStrokeRenderCache::totalRenders();
    VLiquify::preview(d.layer.data(), first);
    VLiquify::preview(d.layer.data(), both);
    QVERIFY(d.settle());
    const VCurveData aMoved = a->curves();
    QVERIFY(aMoved != aStart);
    QVERIFY(b->curves() != bStart);
    const quint64 aRevision = a->revision();
    const quint64 bRevision = b->revision();
    // the canvas repaints in this test: moved strokes render
    QVERIFY(VStrokeRenderCache::totalRenders() > rendersBefore);

    // in-tool undo of the second drag: only its stroke changes; the other
    // keeps its revision, so its rendering stays and nothing re-renders it.
    // The stroke that goes back shows its rendering from before the drag
    int renders = VStrokeRenderCache::totalRenders();
    VLiquify::preview(d.layer.data(), first);
    QVERIFY(b->curves() == bStart);
    QVERIFY(b->revision() != bRevision);
    QVERIFY(a->curves() == aMoved);
    QCOMPARE(a->revision(), aRevision);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);

    // redo of the drag, then undo of both: the kept renderings, no render
    VLiquify::preview(d.layer.data(), both);
    QCOMPARE(a->revision(), aRevision);
    QVERIFY(d.settle());
    VLiquify::preview(d.layer.data(), {});
    QVERIFY(a->curves() == aStart);
    QVERIFY(b->curves() == bStart);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);

    // the drags again, kept as a command: undo and redo render nothing
    VLiquify::preview(d.layer.data(), both);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);
    const quint64 stroke = KisShapeLayerHooks::newLiquifyStroke();
    QScopedPointer<KUndo2Command> command(KisShapeLayerHooks::liquifyCommand(d.layer.data(), both, stroke));
    command->redo();
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), true);
    const VCurveData bMoved = b->curves();
    QVERIFY(a->curves() == aMoved);
    QVERIFY(d.settle());
    command->undo();
    QVERIFY(a->curves() == aStart);
    QVERIFY(b->curves() == bStart);
    QVERIFY(d.settle());
    command->redo();
    QVERIFY(a->curves() == aMoved);
    QVERIFY(b->curves() == bMoved);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);

    // the reused renderings are those of the curves: a render matches them
    const VStrokeRenderCache::Entry rendered =
        VStrokeRenderCache::render(VStrokeRenderKey{b->renderTransform(), b->revision()}, b->renderSettings(), b->imagePixelPrimitives());
    QImage painted(d.p.image->bounds().size(), QImage::Format_ARGB32_Premultiplied);
    painted.fill(Qt::transparent);
    {
        QPainter gc(&painted);
        // as KisShapeLayerCanvas sets up the painter
        gc.setTransform(b->absoluteTransformation() * QTransform::fromScale(d.p.image->xRes(), d.p.image->yRes()));
        b->paint(gc);
    }
    QImage expected(painted.size(), QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::transparent);
    {
        QPainter gc(&expected);
        gc.drawImage(rendered.offset, rendered.image);
    }
    QPoint differs;
    QVERIFY2(TestUtil::compareQImages(differs, painted, expected, 2, 2), qPrintable(QString("differs at %1,%2").arg(differs.x()).arg(differs.y())));
    command->undo();
}

void VStrokeLayerTest::testFastLiquifyRendersWhenMovesPause()
{
    // the transform tool's Fast mode: brush strokes Liquify moves show their
    // renderings bent and render nothing until the moves pause or end

    VFreehandLayer::installHooks();
    VectorDocument d(72);
    VFreehandLayer::setFreehand(d.layer.data());
    VStrokeShape *a = strokeShape(pixelBrushPreset(), d.p.image->xRes(), QPointF(60, 40), 3);
    VStrokeShape *b = strokeShape(pixelBrushPreset(), d.p.image->xRes(), QPointF(500, 300), 4);
    d.layer->addShape(a);
    d.layer->addShape(b);
    d.layer->setDirty();
    QVERIFY(d.settle());

    auto drag = [] (VStrokeShape *stroke, qreal dx) {
        const QPointF at = stroke->renderTransform().map(stroke->curves().positions.first());
        return QVector<KisShapeLayerHooks::LiquifyDab>(3, KisShapeLayerHooks::LiquifyDab{at, QPointF(dx, 0), 30.0, false, 1.0});
    };
    // a repaint of the layer, well within the pause
    auto repaint = [&] () {
        qApp->processEvents();
        d.layer->forceUpdateTimedNode();
        d.p.image->waitForDone();
        qApp->processEvents();
        waitForRenders(d.layer.data());
    };
    const VCurveData aStart = a->curves();
    const VCurveData bStart = b->curves();
    const QVector<KisShapeLayerHooks::LiquifyDab> first = drag(a, 10);
    const QVector<KisShapeLayerHooks::LiquifyDab> more = first + drag(a, 6);
    const QVector<KisShapeLayerHooks::LiquifyDab> both = more + drag(b, 10);

    // moving: no renders, though the layer repaints
    const int rendersBefore = VStrokeRenderCache::totalRenders();
    VLiquify::preview(d.layer.data(), first, true);
    repaint();
    VLiquify::preview(d.layer.data(), more, true);
    repaint();
    QVERIFY(a->curves() != aStart);
    QVERIFY(a->isRendersPaused());
    QVERIFY(!b->isRendersPaused());
    QCOMPARE(VStrokeRenderCache::totalRenders(), rendersBefore);

    // a pause: the moved stroke renders, and stops bending once it has
    QTest::qWait(400);
    QVERIFY(d.settle());
    QVERIFY(VStrokeRenderCache::totalRenders() > rendersBefore);
    QVERIFY(!a->isRendersPaused());
    const VCurveData aMoved = a->curves();
    const quint64 aRevision = a->revision();

    // the other stroke moves; its in-tool undo changes only it, and shows
    // its rendering from before: nothing renders, and the first stroke
    // keeps its revision (no stale rendering across the layer)
    VLiquify::preview(d.layer.data(), both, true);
    repaint();
    QVERIFY(b->isRendersPaused());
    int renders = VStrokeRenderCache::totalRenders();
    VLiquify::preview(d.layer.data(), more, true);
    QVERIFY(b->curves() == bStart);
    QCOMPARE(a->revision(), aRevision);
    QVERIFY(a->curves() == aMoved);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);

    // the stroke ends without keeping the change: the curves go back to
    // their kept renderings, with no render
    VLiquify::preview(d.layer.data(), both, true);
    repaint();
    renders = VStrokeRenderCache::totalRenders();
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), false);
    QVERIFY(a->curves() == aStart);
    QVERIFY(b->curves() == bStart);
    QVERIFY(d.settle());
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);
    QVERIFY(!a->isRendersPaused());
    QVERIFY(!b->isRendersPaused());

    // a stroke that keeps its change renders it once the stroke ends
    VLiquify::preview(d.layer.data(), first, true);
    repaint();
    renders = VStrokeRenderCache::totalRenders();
    const quint64 stroke = KisShapeLayerHooks::newLiquifyStroke();
    QScopedPointer<KUndo2Command> command(KisShapeLayerHooks::liquifyCommand(d.layer.data(), first, stroke));
    command->redo();
    KisShapeLayerHooks::liquifyPreviewDone(d.layer.data(), true);
    QVERIFY(!a->isRendersPaused());
    QVERIFY(d.settle());
    QVERIFY(VStrokeRenderCache::totalRenders() > renders);
    QVERIFY(!a->isRendersPaused());
    command->undo();
    QVERIFY(d.settle());

}

void VStrokeLayerTest::testConvertLayer()
{
    VectorDocument d(300);
    QVERIFY(d.fill());
    KoPathShape *path = shapesOf<KoPathShape>(d.layer.data()).value(0);
    QVERIFY(path);
    const QRectF before = path->absoluteOutlineRect();

    QScopedPointer<KUndo2Command> command(VFreehandLayer::convertCommand(d.layer.data(), d.doc->shapeController()));
    QVERIFY(command);
    command->redo();
    QVERIFY(d.settle());

    QVERIFY(VFreehandLayer::isFreehand(d.layer.data()));
    QVERIFY(shapesOf<KoPathShape>(d.layer.data()).isEmpty());
    QCOMPARE(shapesOf<VGeomCurveShape>(d.layer.data()).size(), 1);
    QCOMPARE(shapesOf<VStrokeShape>(d.layer.data()).size(), 1);
    // in the same place
    const QRectF after = shapesOf<VGeomCurveShape>(d.layer.data()).first()->absoluteOutlineRect();
    QVERIFY2(QLineF(after.center(), before.center()).length() < 0.5,
             qPrintable(QString("moved from %1,%2 to %3,%4").arg(before.center().x()).arg(before.center().y())
                        .arg(after.center().x()).arg(after.center().y())));
    QVERIFY(!VFreehandLayer::convertCommand(d.layer.data(), d.doc->shapeController()));

    command->undo();
    QVERIFY(d.settle());
    QVERIFY(!VFreehandLayer::isFreehand(d.layer.data()));
    QCOMPARE(shapesOf<KoPathShape>(d.layer.data()).size(), 1);
    QVERIFY(shapesOf<VGeomCurveShape>(d.layer.data()).isEmpty());
}

void VStrokeLayerTest::testFreehandLayerClips()
{
    // a freehand layer is a stock vector layer, so every layer feature works: inherit alpha here
    VectorDocument d(72, QRect(0, 0, 200, 100));
    d.p.layer->paintDevice()->fill(QRect(0, 0, 100, 100), KoColor(Qt::white, d.p.image->colorSpace()));
    VFreehandLayer::setFreehand(d.layer.data());

    KoPathShape *box = new KoPathShape();
    box->moveTo(QPointF(-10, -10));
    box->lineTo(QPointF(210, -10));
    box->lineTo(QPointF(210, 110));
    box->lineTo(QPointF(-10, 110));
    box->close();
    box->normalize();
    box->setBackground(toQShared(new KoColorBackground(Qt::blue)));
    QList<KoShape*> shapes = {box};
    QVERIFY(VCurveConverter::convertInPlace(shapes));
    d.layer->addShape(shapes.first());
    d.layer->disableAlphaChannel(true);
    d.layer->setDirty();
    d.p.layer->setDirty();
    QVERIFY(d.settle());

    const KisPaintDeviceSP projection = d.p.image->projection();
    QCOMPARE(projection->pixel(QPoint(50, 50)).toQColor(), QColor(Qt::blue));
    QCOMPARE(projection->pixel(QPoint(150, 50)).opacityU8(), quint8(OPACITY_TRANSPARENT_U8));
}

void VStrokeLayerTest::testEditLeavesNoStalePixels_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::addColumn<bool>("brushStroke");
    QTest::newRow("stroke 72 dpi") << 72.0 << true;
    QTest::newRow("stroke 300 dpi") << 300.0 << true;
    QTest::newRow("curve 72 dpi") << 72.0 << false;
    QTest::newRow("curve 300 dpi") << 300.0 << false;
}

/// dragging a point around, as the point edit tool does, leaves nothing of the old line behind
void VStrokeLayerTest::testEditLeavesNoStalePixels()
{
    QFETCH(qreal, dpi);
    QFETCH(bool, brushStroke);

    // a brush painting far outside its pressure-width outline, as spray
    // and scatter brushes do: here the recorded size is a tenth of the
    // preset's, so the rendering exceeds the outline margin
    auto makeShape = [brushStroke] (qreal pixelsPerPoint) -> VCurveShape* {
        if (brushStroke) {
            VStrokeData data;
            data.preset = pixelBrushPreset();
            data.brushSize = 2;
            data.seed = 3;
            const QVector<VStrokePrimitive> primitives =
                VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(60, 40));
            VStrokeShape *shape = VStrokeShape::fromImagePixels(primitives, pixelsPerPoint, data);
            shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
            return shape;
        }
        QScopedPointer<KoPathShape> path(new KoPathShape());
        path->moveTo(QPointF(30, 40));
        for (int i = 1; i <= 12; i++) path->lineTo(QPointF(30 + 15 * i, 40 + (i % 2) * 20));
        path->normalize();
        path->setStroke(KoShapeStrokeSP(new KoShapeStroke(6, Qt::darkGreen)));
        return VCurveConverter::fromPath(path.data());
    };

    VectorDocument d(dpi);
    VCurveShape *shape = makeShape(d.p.image->xRes());
    d.layer->addShape(shape);
    d.layer->setDirty();
    QVERIFY(d.settle());

    const VCurveData start = shape->curves();
    QVector<bool> selected(start.pointCount(), false);
    selected[start.pointCount() / 2] = true;
    const VPointEdit::Weights weights =
        VPointEdit::weights(start, selected, shape->absoluteTransformation(), 40.0);

    // mouse moves come faster than the renders and the layer repaints
    for (int step = 1; step <= 120; step++) {
        VCurveData data = start;
        VPointEdit::move(data, weights, QPointF(20 * qSin(step * 0.15), 0.8 * step));
        shape->setCurvesAndUpdate(data);
        if (step % 4 == 0) QTest::qWait(step % 12 == 0 ? 12 : 1);
        else qApp->processEvents();
    }
    QVERIFY(d.settle());
    QTest::qWait(200);
    QVERIFY(d.settle());

    // the same line drawn where it ended up, in a fresh document
    VectorDocument fresh(dpi);
    VCurveShape *expected = makeShape(fresh.p.image->xRes());
    expected->setCurves(shape->curves());
    expected->setTransformation(shape->transformation());
    fresh.layer->addShape(expected);
    fresh.layer->setDirty();
    QVERIFY(fresh.settle());

    for (KisPaintDeviceSP device : {KisPaintDeviceSP(d.layer->projection()), d.p.image->projection()}) {
        QRect where;
        const int leftover = leftoverPixels(device, device == d.p.image->projection() ? fresh.p.image->projection()
                                                                                          : fresh.layer->projection(),
                                            &where);
        if (leftover) {
            const QString dir = QDir::homePath() + "/persistent/";
            const QRect rc = where.adjusted(-60, -60, 60, 60);
            device->convertToQImage(nullptr, rc).save(dir + "vstroke_leftover_actual.png");
            (device == d.p.image->projection() ? fresh.p.image->projection() : fresh.layer->projection())
                ->convertToQImage(nullptr, rc).save(dir + "vstroke_leftover_expected.png");
        }
        QVERIFY2(leftover == 0, qPrintable(QString("%1 pixels left behind in %2,%3 %4x%5")
                                               .arg(leftover).arg(where.x()).arg(where.y())
                                               .arg(where.width()).arg(where.height())));
    }
    const QString name = QString("edit_stale_%1_%2").arg(brushStroke ? "stroke" : "curve").arg(int(dpi));
    QString difference = compareDevices(d.layer->projection(), fresh.layer->projection(), name + "_layer", 2);
    QVERIFY2(difference.isEmpty(), qPrintable("layer: " + difference));
    difference = compareDevices(d.p.image->projection(), fresh.p.image->projection(), name + "_image", 2);
    QVERIFY2(difference.isEmpty(), qPrintable("image: " + difference));
}

void VStrokeLayerTest::testTrimLeavesNoGhost_data()
{
    QTest::addColumn<qreal>("dpi");
    QTest::addColumn<bool>("drawnInSession");
    QTest::newRow("drawn in the session, 72 dpi") << 72.0 << true;
    QTest::newRow("drawn in the session, 300 dpi") << 300.0 << true;
    QTest::newRow("rendered by the shape, 72 dpi") << 72.0 << false;
}

/// trimming a stroke: its first rendering may have been the live stroke, which the shape did not render itself
void VStrokeLayerTest::testTrimLeavesNoGhost()
{
    QFETCH(qreal, dpi);
    QFETCH(bool, drawnInSession);

    auto makeData = [] () {
        VStrokeData data;
        data.preset = pixelBrushPreset();
        data.brushSize = 20;
        data.seed = 5;
        return data;
    };
    const QVector<VStrokePrimitive> drawn =
        VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(60, 40));

    VectorDocument d(dpi);
    VStrokeShape *shape;
    if (drawnInSession) {
        // stands in for the live stroke: any pixels over the whole stroke will do
        KisPaintDeviceSP live = new KisPaintDevice(d.p.image->colorSpace());
        live->fill(QRect(90, 90, 360, 130), KoColor(Qt::green, live->colorSpace()));
        shape = commitLikeTool(d.layer.data(), d.p.image->xRes(), drawn, live);
    } else {
        shape = VStrokeShape::fromImagePixels(drawn, d.p.image->xRes(), makeData());
        shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
        d.layer->addShape(shape);
    }
    d.layer->setDirty();
    QVERIFY(d.settle());
    QVERIFY(!d.layer->projection()->exactBounds().isEmpty());

    // the far end goes, as a trim cuts it
    const VCurveData before = shape->curves();
    const VCurveData after = VCurveTrim::extract(before, 0, 3);
    VCurveEditCommand edit({shape}, {before}, {after}, kundo2_noi18n("trim"));
    edit.redo();
    QVERIFY(d.settle());
    QTest::qWait(200);
    QVERIFY(d.settle());

    // the same stroke drawn trimmed, in a fresh document
    VectorDocument fresh(dpi);
    VStrokeShape *expected = VStrokeShape::fromImagePixels(drawn, fresh.p.image->xRes(), makeData());
    expected->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    expected->setCurves(shape->curves());
    expected->setTransformation(shape->transformation());
    fresh.layer->addShape(expected);
    fresh.layer->setDirty();
    QVERIFY(fresh.settle());

    QRect where;
    const int leftover = leftoverPixels(d.layer->projection(), fresh.layer->projection(), &where);
    QVERIFY2(leftover == 0, qPrintable(QString("%1 pixels left behind in %2,%3 %4x%5")
                                           .arg(leftover).arg(where.x()).arg(where.y())
                                           .arg(where.width()).arg(where.height())));
}

/// while points are dragged, the stroke keeps a brush rendering: it never falls back to the flat outline
void VStrokeLayerTest::testEditKeepsRendering()
{
    VectorDocument d(300.0);
    VStrokeData data;
    data.preset = pixelBrushPreset();
    data.brushSize = 20;
    const QVector<VStrokePrimitive> primitives =
        VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(60, 40));
    VStrokeShape *shape = VStrokeShape::fromImagePixels(primitives, d.p.image->xRes(), data);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    d.layer->addShape(shape);
    d.layer->setDirty();
    QVERIFY(d.settle());

    const VCurveData start = shape->curves();
    QVector<bool> selected(start.pointCount(), false);
    selected[start.pointCount() / 2] = true;
    const VPointEdit::Weights weights =
        VPointEdit::weights(start, selected, shape->absoluteTransformation(), 10.0);

    const int outlinesBefore = VStrokeShape::outlinePaints();
    const int rendersBefore = VStrokeRenderCache::totalRenders();
    const int steps = 40;
    for (int step = 1; step <= steps; step++) {
        VCurveData moved = start;
        VPointEdit::move(moved, weights, QPointF(0.5 * step, 0.3 * step));
        shape->setCurvesAndUpdate(moved);
        // the layer repaints between mouse moves; renders may lag behind
        QTest::qWait(30);
        QVERIFY(!d.layer->projection()->exactBounds().isEmpty());
    }
    QVERIFY(d.settle());
    QCOMPARE(VStrokeShape::outlinePaints() - outlinesBefore, 0);
    // one render at a time, the newest queued: never more than one per change
    QVERIFY(VStrokeRenderCache::totalRenders() - rendersBefore <= steps);
}

void VStrokeLayerTest::testToolSwitching()
{
    VectorDocument d(300.0);
    KisPaintLayerSP raster = new KisPaintLayer(d.p.image, "raster", OPACITY_OPAQUE_U8);
    d.p.image->addNode(raster);
    const QString brush = VStrokeToolSwitcher::BrushToolId;
    const QString vector = VStrokeToolSwitcher::VectorToolId;

    const QString select = VStrokeToolSwitcher::ShapeSelectToolId;

    // a stock vector layer: as stock Krita
    QCOMPARE(VStrokeToolSwitcher::switchTarget(brush, d.layer), QString());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(vector, d.layer), select);

    VFreehandLayer::setFreehand(d.layer.data());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(brush, d.layer), vector);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(brush, raster), QString());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(vector, raster), brush);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(vector, d.layer), QString());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(vector, KisNodeSP()), brush);
    // other tools are left alone
    QCOMPARE(VStrokeToolSwitcher::switchTarget("KritaShape/KisToolLine", d.layer), QString());

    // clicking a vector layer swaps the brush for the shape select tool first
    QCOMPARE(VStrokeToolSwitcher::effectiveToolId(select, true, true), brush);
    // picked by hand: no layer change
    QCOMPARE(VStrokeToolSwitcher::effectiveToolId(select, true, false), select);
    QCOMPARE(VStrokeToolSwitcher::effectiveToolId(select, false, true), select);
    QCOMPARE(VStrokeToolSwitcher::effectiveToolId(brush, false, true), brush);

    // no toolbox button: the code is never selected, but the tool stays active on any selection
    VStrokeToolFactory factory;
    const QString code = factory.activationShapeId();
    QVERIFY(!code.startsWith("flake/"));
    QVERIFY(!code.endsWith("/always"));
    QVERIFY(code.split(',').contains("flake/always"));

    // Edit Shapes and the point edit tool share the Edit Shapes button
    const QString edit = VStrokeToolSwitcher::EditShapesToolId;
    const QString points = VStrokeToolSwitcher::PointEditToolId;
    QCOMPARE(VStrokeToolSwitcher::switchTarget(edit, d.layer), points);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(points, d.layer), QString());
    // only stock paths selected (a dashed path stays stock): stock Edit Shapes
    QCOMPARE(VStrokeToolSwitcher::switchTarget(edit, d.layer, true), QString());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(points, d.layer, true), edit);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(points, raster), brush);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(edit, raster), QString());
    VPointEditToolFactory pointFactory;
    QCOMPARE(pointFactory.activationShapeId(), code);

    VectorDocument stock(300.0);
    QCOMPARE(VStrokeToolSwitcher::switchTarget(edit, stock.layer), QString());
    QCOMPARE(VStrokeToolSwitcher::switchTarget(points, stock.layer), edit);
}

/// Image > Scale to New Size with only the resolution changed: the strokes
/// keep their image pixels and render at the new resolution
void VStrokeLayerTest::testImageResolutionChange()
{
    VectorDocument d(300.0);
    QVERIFY(d.fill());
    VStrokeShape *stroke = d.stroke();
    const QVector<VStrokePrimitive> before = stroke->imagePixelPrimitives();
    KisPaintDeviceSP pixelsBefore = new KisPaintDevice(*d.layer->original());

    d.p.image->scaleImage(d.p.image->size(), 150.0 / 72.0, 150.0 / 72.0,
                          KisFilterStrategyRegistry::instance()->value("Bicubic"));
    // the scale transforms the shape layer in the GUI thread
    waitForImageWithEvents(d.p.image);
    QVERIFY(d.settle());

    QVERIFY2(qFuzzyCompare(stroke->resolution(), 150.0 / 72.0),
             qPrintable(QString("resolution %1 dpi").arg(72.0 * stroke->resolution())));
    comparePositions(stroke->imagePixelPrimitives(), before);
    const QString diff = compareDevices(d.layer->original(), pixelsBefore, "resolution_change");
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

void VStrokeLayerTest::testStress()
{
    const int iterations = qEnvironmentVariableIntValue("VSTROKE_STRESS_ITERATIONS") > 0 ?
        qEnvironmentVariableIntValue("VSTROKE_STRESS_ITERATIONS") : 12;
    const int strokesPerIteration = 60;

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    TestUtil::MaskParent p(QRect(0, 0, 2500, 10000));
    p.image->setResolution(300.0 / 72.0, 300.0 / 72.0);
    doc->setCurrentImage(p.image);

    KisShapeLayerSP layer = new KisShapeLayer(doc->shapeController(), p.image, "vector", 255);
    p.image->addNode(layer);

    QRandomGenerator rng(1234);
    KisPaintOpPresetSP presets[] = {pixelBrushPreset(), cspBrushPreset()};

    ResourceLog log;
    log.sample("start", 0, VStrokeRenderCache::totalRenders());
    const int rendersAtStart = VStrokeRenderCache::totalRenders();
    int strokesAdded = 0;
    int strokesChanged = 0;

    for (int it = 0; it < iterations; it++) {
        // add strokes all over the canvas; the layer repaints in image threads meanwhile
        for (int i = 0; i < strokesPerIteration; i++) {
            const QPointF offset(rng.bounded(2100), rng.bounded(9700));
            layer->addShape(strokeShape(presets[i % 2], p.image->xRes(), offset, rng.bounded(1 << 30)));
            strokesAdded++;
            if (i % 10 == 0) qApp->processEvents();
        }
        layer->setDirty();
        qApp->processEvents();

        // move and resize some of them while renders are running
        const QList<KoShape*> shapes = layer->shapes();
        for (int i = 0; i < shapes.size(); i += 3) {
            shapes[i]->update();
            shapes[i]->setTransformation(shapes[i]->transformation() * QTransform::fromTranslate(rng.bounded(20) - 10, 3));
            if (i % 2) shapes[i]->setSize(shapes[i]->size() * 1.1);
            shapes[i]->update();
            strokesChanged++;
        }
        qApp->processEvents();

        // free-transform the whole layer every few iterations
        if (it % 4 == 3) {
            layer->setTransformation(layer->transformation() * QTransform().rotate(3));
            layer->setDirty();
        }

        // autosave in the middle of it all
        {
            KisImageSP clone = p.image->clone(true);
            QVERIFY(clone);
            qApp->processEvents();
        }

        // delete a third of the strokes, some with renders still running
        for (int i = 0; i < shapes.size(); i += 3) {
            layer->removeShape(shapes[i]);
            delete shapes[i];
        }
        qApp->processEvents();
        p.image->waitForDone();

        log.sample(QString("iteration %1").arg(it), layer->shapes().size(), VStrokeRenderCache::totalRenders());
    }

    QVERIFY(waitForIdle(p, 60000));
    waitForRenders(layer.data());
    QVERIFY(waitForIdle(p, 60000));
    log.sample("settled", layer->shapes().size(), VStrokeRenderCache::totalRenders());
    QVERIFY(!layer->exactBounds().isEmpty());

    // no render loops: a stroke renders when added and after each change
    // (plus a few for layer transforms), not continuously
    const int renders = VStrokeRenderCache::totalRenders() - rendersAtStart;
    const int liveStrokes = layer->shapes().size();
    log.line(QString("renders: %1 for %2 added and %3 changes, %4 live strokes")
             .arg(renders).arg(strokesAdded).arg(strokesChanged).arg(liveStrokes));
    QVERIFY2(renders < 3 * (strokesAdded + strokesChanged) + 4 * liveStrokes * (iterations / 4 + 1),
             qPrintable(QString("%1 renders").arg(renders)));

    // steady state: re-render the same strokes many times; in-use heap and
    // tiles must stay flat, or renders leak
    const QList<KoShape*> kept = layer->shapes();
    for (int round = 0; round < 8; round++) {
        Q_FOREACH (KoShape *shape, kept) {
            shape->update();
            shape->setTransformation(shape->transformation() * QTransform::fromTranslate(round % 2 ? -2 : 2, 0));
            shape->update();
        }
        QVERIFY(waitForIdle(p, 60000));
        waitForRenders(layer.data());
        QVERIFY(waitForIdle(p, 60000));
        log.sample(QString("steady round %1").arg(round), layer->shapes().size(), VStrokeRenderCache::totalRenders());
    }

    // idle: nothing renders any more
    const int rendersBeforeIdle = VStrokeRenderCache::totalRenders();
    QTest::qWait(2000);
    QVERIFY(waitForIdle(p));
    QCOMPARE(VStrokeRenderCache::totalRenders(), rendersBeforeIdle);

    // delete everything: memory must come back down
    Q_FOREACH (KoShape *shape, layer->shapes()) {
        layer->removeShape(shape);
        delete shape;
    }
    QVERIFY(waitForIdle(p));
    QTest::qWait(1000);
    log.sample("all deleted", layer->shapes().size(), VStrokeRenderCache::totalRenders());
#ifdef Q_OS_LINUX
    malloc_trim(0);
#endif
    log.sample("after malloc_trim", layer->shapes().size(), VStrokeRenderCache::totalRenders());

    // nothing of the strokes is left behind
    QCOMPARE(VStrokeShape::liveCount(), 0);
    QCOMPARE(VStrokeRenderCache::liveCaches(), 0);
    QCOMPARE(VStrokeRenderCache::liveImageBytes(), qint64(0));
    QCOMPARE(VStrokePresetStore::instance()->liveCount(), 0);
}

KISTEST_MAIN(VStrokeLayerTest)
