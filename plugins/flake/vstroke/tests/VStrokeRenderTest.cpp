/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeRenderTest.h"

#include <QPainter>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <QTest>

#include <kistest.h>

#include <KoColorBackground.h>
#include <KoCanvasBase.h>
#include <KoShapeManager.h>
#include <KoUnit.h>
#include <KoViewConverter.h>

#include "VCurveOpacity.h"
#include "VStrokeShape.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

namespace {

const qreal PixelsPerPoint = 300.0 / 72.0;
const QSize CanvasSize(420, 240);

VStrokeShape *strokeShape(KisPaintOpPresetSP preset, const QString &compositeOp = COMPOSITE_OVER)
{
    VStrokeData settings;
    settings.preset = preset;
    settings.brushSize = 20;
    settings.opacity = 0.9;
    settings.seed = 11;
    settings.perStrokeSeed = 22;
    settings.compositeOp = compositeOp;

    VStrokeShape *shape = VStrokeShape::fromImagePixels(testStroke(), PixelsPerPoint, settings);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    return shape;
}

/// as KisShapeLayerCanvas and KoShapeManager set up the painter, for a tile at \p tileOrigin
void paintShape(const KoShape *shape, QImage *image, const QPoint &tileOrigin = QPoint())
{
    QPainter painter(image);
    const QTransform documentToView = QTransform::fromScale(PixelsPerPoint, PixelsPerPoint);
    painter.setTransform(shape->absoluteTransformation() * documentToView *
                         QTransform::fromTranslate(-tileOrigin.x(), -tileOrigin.y()));
    shape->paint(painter);
}

QImage transparentImage()
{
    QImage image(CanvasSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    return image;
}

/// what painting the shape should give: its fresh rendering drawn at its offset
QImage expectedImage(const VStrokeShape *shape, const QPoint &tileOrigin = QPoint())
{
    const VStrokeRenderKey key{shape->renderTransform(), 0};
    const VStrokeRenderCache::Entry entry =
        VStrokeRenderCache::render(key, shape->renderSettings(), shape->imagePixelPrimitives());

    QImage image = transparentImage();
    QPainter painter(&image);
    painter.drawImage(entry.offset - tileOrigin, entry.image);
    return image;
}

int alphaSum(const QImage &image)
{
    int sum = 0;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            sum += qAlpha(image.pixel(x, y));
        }
    }
    return sum;
}

}

/// the alpha of \p entry in a square of \p half pixels around \p at
int alphaAround(const VStrokeRenderCache::Entry &entry, const QPointF &at, int half)
{
    int sum = 0;
    const QPoint c = at.toPoint() - entry.offset;
    for (int y = c.y() - half; y <= c.y() + half; y++) {
        for (int x = c.x() - half; x <= c.x() + half; x++) {
            if (entry.image.rect().contains(x, y)) sum += qAlpha(entry.image.pixel(x, y));
        }
    }
    return sum;
}

void VStrokeRenderTest::initTestCase()
{
    ensurePaintOps();
}

void VStrokeRenderTest::testCacheIsBlittedOneToOne()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));

    // the first paint shows the outline and requests the render
    QImage first = transparentImage();
    paintShape(shape.data(), &first);
    QVERIFY(alphaSum(first) > 0);
    shape->waitForRendering();

    // in two different tiles
    Q_FOREACH (const QPoint &tile, QVector<QPoint>({QPoint(), QPoint(37, -11)})) {
        QImage painted = transparentImage();
        paintShape(shape.data(), &painted, tile);
        QCOMPARE(painted, expectedImage(shape.data(), tile));
    }
    QVERIFY(first != expectedImage(shape.data()));
}

void VStrokeRenderTest::testCloneUsesSharedRendering()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();

    // a clone made for painting shows the rendering right away
    QScopedPointer<KoShape> clone(shape->cloneShape());
    QImage painted = transparentImage();
    paintShape(clone.data(), &painted);
    QCOMPARE(painted, expectedImage(shape.data()));
}

void VStrokeRenderTest::testMoveRerenders()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();
    const QImage before = expectedImage(shape.data());

    // move by 3 image pixels, as the Move tool does: the rendering is copied
    // to the new place, the same pixels a new render gives (ACTIVE 7.2)
    const int renders = VStrokeRenderCache::totalRenders();
    shape->setTransformation(shape->transformation() * QTransform::fromTranslate(3.0 / PixelsPerPoint, -2.0 / PixelsPerPoint));
    QImage painted = transparentImage();
    paintShape(shape.data(), &painted);
    shape->waitForRendering();
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);
    const QImage expected = expectedImage(shape.data());
    QCOMPARE(painted, expected);
    QVERIFY(expected != before);

    // half a pixel more: it renders anew, showing the old rendering meanwhile
    shape->setTransformation(shape->transformation() * QTransform::fromTranslate(0.5 / PixelsPerPoint, 0));
    QImage moving = transparentImage();
    paintShape(shape.data(), &moving);
    QVERIFY(alphaSum(moving) > 0);
    shape->waitForRendering();
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders + 1);
    QImage settled = transparentImage();
    paintShape(shape.data(), &settled);
    QCOMPARE(settled, expectedImage(shape.data()));
}

namespace {
/// a canvas that does nothing, for a shape manager of its own
class NullCanvas : public KoCanvasBase
{
public:
    NullCanvas() : KoCanvasBase(nullptr) {}
    void gridSize(QPointF *, QSizeF *) const override {}
    bool snapToGrid() const override { return false; }
    void setCursor(const QCursor &) override {}
    void addCommand(KUndo2Command *command) override { delete command; }
    KoShapeManager *shapeManager() const override { return nullptr; }
    KoSelectedShapesProxy *selectedShapesProxy() const override { return nullptr; }
    void updateCanvas(const QRectF &) override {}
    KoToolProxy *toolProxy() const override { return nullptr; }
    const KoViewConverter *viewConverter() const override { return &m_converter; }
    KoViewConverter *viewConverter() override { return &m_converter; }
    QWidget *canvasWidget() override { return nullptr; }
    const QWidget *canvasWidget() const override { return nullptr; }
    KoUnit unit() const override { return KoUnit(); }
private:
    KoViewConverter m_converter;
};
}

void VStrokeRenderTest::testMovedCurvesReindexed()
{
    // curves moved within the same size (a Perspective box move): the shape
    // manager finds the stroke at its new place, where a repaint looks for
    // the shapes of each tile
    VStrokeShape *shape = strokeShape(pixelBrushPreset());
    NullCanvas canvas;
    KoShapeManager manager(&canvas);
    manager.addShape(shape, KoShapeManager::AddWithoutRepaint);
    const QRectF before = shape->boundingRect();
    QVERIFY(manager.shapesAt(before).contains(shape));

    const QPointF by(before.width() * 3, before.height() * 3);
    VCurveData moved = shape->curves();
    moved.transform(QTransform::fromTranslate(by.x(), by.y()));
    const QSizeF size = shape->size();
    shape->setCurves(moved);
    QCOMPARE(shape->size(), size);
    const QRectF after = shape->boundingRect();
    // the outline's part of the bounds: where the stroke now is
    const QRectF there = before.translated(by);
    QVERIFY(after.intersects(there));
    QVERIFY2(manager.shapesAt(there).contains(shape), "the moved stroke isn't found at its new place");
    manager.remove(shape);
    delete shape;
}

void VStrokeRenderTest::testEraseStrokeErases()
{
    QScopedPointer<VStrokeShape> eraser(strokeShape(pixelBrushPreset(), COMPOSITE_ERASE));

    QImage dummy = transparentImage();
    paintShape(eraser.data(), &dummy);
    eraser->waitForRendering();

    QImage painted(CanvasSize, QImage::Format_ARGB32);
    painted.fill(Qt::red);
    const int opaque = alphaSum(painted);
    paintShape(eraser.data(), &painted);

    QVERIFY(alphaSum(painted) < opaque);
    // only where the stroke is: far corner untouched
    QCOMPARE(qAlpha(painted.pixel(0, 0)), 255);
}

namespace {
int maxDifference(const QImage &a, const QImage &b)
{
    int result = 0;
    for (int y = 0; y < a.height(); y++) {
        for (int x = 0; x < a.width(); x++) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            result = qMax(result, qAbs(qRed(pa) - qRed(pb)));
            result = qMax(result, qAbs(qGreen(pa) - qGreen(pb)));
            result = qMax(result, qAbs(qBlue(pa) - qBlue(pb)));
            result = qMax(result, qAbs(qAlpha(pa) - qAlpha(pb)));
        }
    }
    return result;
}

/// the stroke, rendered, painted over a canvas of \p below
QImage paintedOver(const QString &compositeOp, const QColor &below,
                   QImage::Format format = QImage::Format_ARGB32_Premultiplied)
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset(), compositeOp));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();

    QImage painted(CanvasSize, format);
    painted.fill(below);
    paintShape(shape.data(), &painted);
    return painted;
}
}

void VStrokeRenderTest::testBlendModeOverStrokeBelow_data()
{
    QTest::addColumn<QString>("op");
    QTest::addColumn<int>("mode");
    QTest::newRow("multiply") << QString(COMPOSITE_MULT) << int(QPainter::CompositionMode_Multiply);
    QTest::newRow("screen") << QString(COMPOSITE_SCREEN) << int(QPainter::CompositionMode_Screen);
    QTest::newRow("overlay") << QString(COMPOSITE_OVERLAY) << int(QPainter::CompositionMode_Overlay);
    QTest::newRow("darken") << QString(COMPOSITE_DARKEN) << int(QPainter::CompositionMode_Darken);
    QTest::newRow("lighten") << QString(COMPOSITE_LIGHTEN) << int(QPainter::CompositionMode_Lighten);
    QTest::newRow("dodge") << QString(COMPOSITE_DODGE) << int(QPainter::CompositionMode_ColorDodge);
    QTest::newRow("burn") << QString(COMPOSITE_BURN) << int(QPainter::CompositionMode_ColorBurn);
    QTest::newRow("hard light") << QString(COMPOSITE_HARD_LIGHT) << int(QPainter::CompositionMode_HardLight);
    QTest::newRow("soft light") << QString(COMPOSITE_SOFT_LIGHT_SVG) << int(QPainter::CompositionMode_SoftLight);
    QTest::newRow("soft light photoshop") << QString(COMPOSITE_SOFT_LIGHT_PHOTOSHOP) << int(QPainter::CompositionMode_SoftLight);
    QTest::newRow("difference") << QString(COMPOSITE_DIFF) << int(QPainter::CompositionMode_Difference);
    QTest::newRow("exclusion") << QString(COMPOSITE_EXCLUSION) << int(QPainter::CompositionMode_Exclusion);
    QTest::newRow("addition") << QString(COMPOSITE_ADD) << int(QPainter::CompositionMode_Plus);
}

void VStrokeRenderTest::testBlendModeOverStrokeBelow()
{
    QFETCH(QString, op);
    QFETCH(int, mode);

    QVERIFY(VStrokeData::canBlendWith(op));

    // the stroke is (20, 40, 160): this canvas tells every mode from Normal in some channel
    const QColor below(200, 200, 100);
    const QImage blended = paintedOver(op, below);
    QVERIFY2(maxDifference(blended, paintedOver(COMPOSITE_OVER, below)) > 10, qPrintable(op));
    QCOMPARE(blended.pixel(0, 0), below.rgb());

    // the rendering, drawn in that mode
    QImage expected(CanvasSize, QImage::Format_ARGB32_Premultiplied);
    expected.fill(below);
    {
        QPainter painter(&expected);
        painter.setCompositionMode(QPainter::CompositionMode(mode));
        painter.drawImage(0, 0, paintedOver(COMPOSITE_OVER, Qt::transparent));
    }
    const int difference = maxDifference(blended, expected);
    QVERIFY2(difference <= 1, qPrintable(QString::number(difference)));
}

void VStrokeRenderTest::testBlendModeOverEmptyIsNormal()
{
    const QImage normal = paintedOver(COMPOSITE_OVER, Qt::transparent);
    QVERIFY(alphaSum(normal) > 0);
    const int difference = maxDifference(paintedOver(COMPOSITE_MULT, Qt::transparent), normal);
    QVERIFY2(difference <= 1, qPrintable(QString::number(difference))); // Qt's rounding
}

void VStrokeRenderTest::testKritaBlendMode_data()
{
    QTest::addColumn<QString>("op");
    QTest::newRow("color") << QString(COMPOSITE_COLOR);
    QTest::newRow("divide") << QString(COMPOSITE_DIVIDE);
    QTest::newRow("saturation") << QString(COMPOSITE_SATURATION);
    QTest::newRow("luminosity sai") << QString(COMPOSITE_LUMINOSITY_SAI);
    QTest::newRow("hue") << QString(COMPOSITE_HUE);
}

/// the modes QPainter doesn't have: the same result as Krita's blend of the same pixels
void VStrokeRenderTest::testKritaBlendMode()
{
    QFETCH(QString, op);
    QVERIFY(VStrokeData::canBlendWith(op));

    const QColor below(200, 200, 100);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect rect(QPoint(), CanvasSize);

    KisPaintDeviceSP stroke = new KisPaintDevice(cs);
    stroke->convertFromQImage(paintedOver(COMPOSITE_OVER, Qt::transparent, QImage::Format_ARGB32), nullptr);
    KisPaintDeviceSP canvas = new KisPaintDevice(cs);
    canvas->fill(rect, KoColor(below, cs));
    {
        KisPainter gc(canvas);
        gc.setCompositeOpId(op);
        gc.bitBlt(QPoint(), stroke, rect);
    }
    const QImage expected = canvas->convertToQImage(nullptr, rect);

    // what the layer paints into, and premultiplied
    for (QImage::Format format : {QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied}) {
        const QImage blended = paintedOver(op, below, format);
        QVERIFY2(maxDifference(blended, paintedOver(COMPOSITE_OVER, below, format)) > 10, qPrintable(op));
        const int difference = maxDifference(blended, expected);
        QVERIFY2(difference <= 2, qPrintable(QString("%1: %2").arg(op).arg(difference)));
    }
}

void VStrokeRenderTest::testKritaBlendModeKeepsClipAndOpacity()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset(), COMPOSITE_DIVIDE));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();

    const QColor below(100, 100, 100);
    auto paintWith = [&] (const QRect &clip, qreal opacity) {
        QImage painted(CanvasSize, QImage::Format_ARGB32);
        painted.fill(below);
        QPainter painter(&painted);
        painter.setClipRect(clip);
        painter.setOpacity(opacity);
        painter.setTransform(shape->absoluteTransformation() * QTransform::fromScale(PixelsPerPoint, PixelsPerPoint));
        shape->paint(painter);
        return painted;
    };

    const QRect all(QPoint(), CanvasSize);
    const QRect left(0, 0, CanvasSize.width() / 2, CanvasSize.height());
    const QImage whole = paintWith(all, 1.0);
    const QImage clipped = paintWith(left, 1.0);
    int outside = 0;
    int inside = 0;
    for (int y = 0; y < whole.height(); y++) {
        for (int x = 0; x < whole.width(); x++) {
            if (left.contains(x, y)) inside += clipped.pixel(x, y) != whole.pixel(x, y);
            else outside += clipped.pixel(x, y) != below.rgb();
        }
    }
    QCOMPARE(outside, 0);
    QCOMPARE(inside, 0);

    // half the opacity is half way between the canvas and the full result
    const QImage half = paintWith(all, 0.5);
    int worst = 0;
    for (int y = 0; y < whole.height(); y++) {
        for (int x = 0; x < whole.width(); x++) {
            worst = qMax(worst, qAbs(qRed(half.pixel(x, y)) - (below.red() + qRed(whole.pixel(x, y))) / 2));
        }
    }
    QVERIFY(maxDifference(whole, half) > 10);
    QVERIFY2(worst <= 40, qPrintable(QString::number(worst)));
}

void VStrokeRenderTest::testUnknownBlendModeIsNormal()
{
    const QString unknown("no such mode");
    QVERIFY(!VStrokeData::canBlendWith(unknown));
    QCOMPARE(paintedOver(unknown, QColor(200, 200, 200)), paintedOver(COMPOSITE_OVER, QColor(200, 200, 200)));
}

void VStrokeRenderTest::testMissingPresetPaintsOutline()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(KisPaintOpPresetSP()));

    QImage painted = transparentImage();
    paintShape(shape.data(), &painted);
    QVERIFY(alphaSum(painted) > 0);
}

void VStrokeRenderTest::testPerPointOpacity()
{
    // the brush's rendering fades out along the stroke
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    const VStrokeRenderKey key{shape->renderTransform(), 0};
    const VStrokeRenderCache::Entry plain =
        VStrokeRenderCache::render(key, shape->renderSettings(), shape->imagePixelPrimitives());

    VCurveData d = shape->curves();
    const int n = d.pointCount();
    for (int i = 0; i < n; i++) d.setAttribute(VCurveOpacity::Attribute, i, 1.0 - qreal(i) / (n - 1), 1.0);
    shape->setCurves(d);
    const VStrokeRenderCache::Entry faded =
        VStrokeRenderCache::render(key, shape->renderSettings(), shape->imagePixelPrimitives());

    const QPointF start = shape->renderTransform().map(d.positions[0]);
    const QPointF middle = shape->renderTransform().map(d.positions[n / 2]);
    const QPointF end = shape->renderTransform().map(d.positions[n - 1]);
    QVERIFY(alphaAround(plain, end, 3) > 0);
    QVERIFY(alphaAround(faded, start, 3) > 0.9 * alphaAround(plain, start, 3));
    const qreal ratio = qreal(alphaAround(faded, middle, 3)) / alphaAround(plain, middle, 3);
    QVERIFY2(ratio > 0.3 && ratio < 0.7, qPrintable(QString::number(ratio)));
    QVERIFY(alphaAround(faded, end, 3) < 0.1 * alphaAround(plain, end, 3));
}

namespace {
/// the pixel brush without scatter, so it stays within its radius
KisPaintOpPresetSP unscatteredPreset()
{
    KisPaintOpPresetSP preset = pixelBrushPreset();
    KisScatterOptionData scatter;
    scatter.isChecked = false;
    scatter.write(preset->settings().data());
    return preset;
}

/// pixels of \p a and \p b differing by more than 2 levels (premultiplied), and the largest difference
QPair<int, int> premultipliedDifference(const QImage &a8, const QImage &b8)
{
    const QImage a = a8.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage b = b8.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int differing = 0;
    int largest = 0;
    for (int y = 0; y < b.height(); y++) {
        const QRgb *la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb *lb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < b.width(); x++) {
            const int d = qMax(qMax(qAbs(qAlpha(la[x]) - qAlpha(lb[x])), qAbs(qRed(la[x]) - qRed(lb[x]))),
                               qMax(qAbs(qGreen(la[x]) - qGreen(lb[x])), qAbs(qBlue(la[x]) - qBlue(lb[x]))));
            largest = qMax(largest, d);
            differing += d > 2;
        }
    }
    return qMakePair(differing, largest);
}
}

void VStrokeRenderTest::testMovedPointShowsLastRendering()
{
    // an Accurate point move (Liquify, Edit Shapes) with no transform motion
    // set: until the render lands, the stand-in is the last rendering drawn
    // as it is, never bent piece by piece (that shows notches at the joints)
    // and never the outline
    QScopedPointer<VStrokeShape> shape(strokeShape(unscatteredPreset()));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();
    const QImage before = expectedImage(shape.data());

    VCurveData moved = shape->curves();
    const int middle = moved.pointCount() / 2;
    const QTransform imageToLocal = shape->renderTransform().inverted();
    moved.positions[middle] += imageToLocal.map(QPointF(0, 8)) - imageToLocal.map(QPointF(0, 0));
    shape->setCurves(moved);
    QVERIFY(!shape->isRendersPaused());

    const int stale = VStrokeShape::staleShapePaints();
    const int outlines = VStrokeShape::outlinePaints();
    QImage standIn = transparentImage();
    paintShape(shape.data(), &standIn);
    QCOMPARE(VStrokeShape::staleShapePaints(), stale + 1);
    QCOMPARE(VStrokeShape::outlinePaints(), outlines);
    const QPair<int, int> d = premultipliedDifference(standIn, before);
    QVERIFY2(d.first == 0, qPrintable(QString("%1 pixels differ from the last rendering, by up to %2").arg(d.first).arg(d.second)));
    shape->waitForRendering();
    QImage painted = transparentImage();
    paintShape(shape.data(), &painted);
    QCOMPARE(painted, expectedImage(shape.data()));
}

void VStrokeRenderTest::testFastLiquifyWarpsUntilRendered()
{
    // while Liquify's Fast mode moves a stroke, it renders nothing: a stroke
    // moved by whole pixels looks like its rendering moved, and renders when
    // the moves stop
    QScopedPointer<VStrokeShape> shape(strokeShape(unscatteredPreset()));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();
    const QImage before = expectedImage(shape.data());

    // 10, 5 image pixels, in shape-local units
    const QTransform imageToLocal = shape->renderTransform().inverted();
    const QPointF shift = imageToLocal.map(QPointF(10, 5)) - imageToLocal.map(QPointF(0, 0));
    shape->setMotion(VStrokeShape::Motion{true, QTransform()});
    VCurveData moved = shape->curves();
    moved.transform(QTransform::fromTranslate(shift.x(), shift.y()));
    shape->setCurves(moved);

    const int renders = VStrokeRenderCache::totalRenders();
    QImage warped = transparentImage();
    paintShape(shape.data(), &warped);
    shape->waitForRendering();
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders);

    QImage expected = transparentImage();
    {
        QPainter painter(&expected);
        painter.drawImage(QPoint(10, 5), before);
    }
    QVERIFY(alphaSum(expected) > 0);
    const QPair<int, int> d = premultipliedDifference(warped, expected);
    QVERIFY2(d.first == 0, qPrintable(QString("%1 pixels differ, by up to %2").arg(d.first).arg(d.second)));

    // the moves stop: it renders, and never shows the rendering at the old pose (the hop)
    shape->clearMotion();
    QVERIFY(!shape->isRendersPaused());
    QImage settling = transparentImage();
    paintShape(shape.data(), &settling);
    const QPair<int, int> s = premultipliedDifference(settling, warped);
    QVERIFY2(s.first == 0, qPrintable(QString("%1 pixels differ from the bent rendering, by up to %2").arg(s.first).arg(s.second)));
    shape->waitForRendering();
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders + 1);
    QTest::qWait(20);
    QVERIFY(!shape->isRendersPaused());
    QImage painted = transparentImage();
    paintShape(shape.data(), &painted);
    QCOMPARE(painted, expectedImage(shape.data()));

    // other points (a curve added): no bending; the stand-in is a rendering
    // of another pose drawn as it is, which the counter sees
    const int stale = VStrokeShape::staleShapePaints();
    VCurveData more = shape->curves();
    more.append(moved);
    shape->setCurves(more);
    QImage unbent = transparentImage();
    paintShape(shape.data(), &unbent);
    QVERIFY(VStrokeShape::staleShapePaints() > stale);
    shape->waitForRendering();
}

void VStrokeRenderTest::testScaledStrokeKeepsBrushWidth()
{
    // a stroke scaled by a transform keeps its brush size: until its new
    // rendering lands, the old one is bent to the new curves, as wide as
    // before, and not scaled with the shape
    QScopedPointer<VStrokeShape> shape(strokeShape(unscatteredPreset()));
    QImage dummy = transparentImage();
    paintShape(shape.data(), &dummy);
    shape->waitForRendering();
    const QImage before = expectedImage(shape.data());

    // taller, in shape-local units (about the stroke's top left)
    const QTransform oldRender = shape->renderTransform();
    shape->setTransformation(QTransform::fromScale(1.0, 1.4) * shape->transformation());
    QImage scaled = transparentImage();
    {
        // what drawing the old rendering under the new transform gives
        QPainter painter(&scaled);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(oldRender.inverted() * shape->renderTransform());
        painter.drawImage(QPoint(), before);
    }

    const int renders = VStrokeRenderCache::totalRenders();
    QImage standIn = transparentImage();
    paintShape(shape.data(), &standIn);
    shape->waitForRendering();
    QCOMPARE(VStrokeRenderCache::totalRenders(), renders + 1);
    const QImage fresh = expectedImage(shape.data());

    const qreal standInArea = alphaSum(standIn);
    const qreal freshArea = alphaSum(fresh);
    const qreal scaledArea = alphaSum(scaled);
    QVERIFY(freshArea > 0);
    QVERIFY2(qAbs(standInArea / freshArea - 1.0) < 0.12,
             qPrintable(QString("stand-in %1, fresh %2, scaled %3").arg(standInArea).arg(freshArea).arg(scaledArea)));
    QVERIFY2(scaledArea / freshArea > 1.2,
             qPrintable(QString("scaling the rendering should widen it: fresh %1, scaled %2").arg(freshArea).arg(scaledArea)));

    QImage painted = transparentImage();
    paintShape(shape.data(), &painted);
    QCOMPARE(painted, fresh);
}

KISTEST_MAIN(VStrokeRenderTest)
