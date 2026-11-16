/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeParityTest.h"

#include <QPainter>
#include <QTest>

#include <kistest.h>

#include <KoCanvasResourceProvider.h>
#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <KisAsynchronousStrokeUpdateHelper.h>
#include <kis_resources_snapshot.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

#include "stroke_testing_utils.h"

#include "VStrokeShape.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

namespace {

const QRect Bounds(0, 0, 420, 240);
const QColor PaintColor(20, 40, 160);
const qreal Opacity = 0.7;

/// the stroke through Krita's freehand stroke, on a paint layer
QImage paintRaster(KisPaintOpPresetSP preset, const QVector<VStrokePrimitive> &primitives)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(0, Bounds.width(), Bounds.height(), cs, "parity");
    KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer);

    QScopedPointer<KoCanvasResourceProvider> manager(utils::createResourceManager(image, layer, QString()));
    manager->setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    manager->setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(KoColor(PaintColor, cs)));
    manager->setResource(KoCanvasResource::Opacity, Opacity);

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, manager.data());

    const KisDistanceInitInfo startDist =
        VStrokePainter::startDistance(preset, primitives.first().pi1.pos(), 0.0);
    KisFreehandStrokeInfo *info = new KisFreehandStrokeInfo(KisDistanceInitInfo(startDist).makeDistInfo());

    KisStrokeId id = image->startStroke(
        new FreehandStrokeStrategy(resources, info, kundo2_noi18n("parity")));
    Q_FOREACH (const VStrokePrimitive &p, primitives) {
        switch (p.type) {
        case VStrokePrimitive::Point:
            image->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1));
            break;
        case VStrokePrimitive::Line:
            image->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1, p.pi2));
            break;
        case VStrokePrimitive::Curve:
            image->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1, p.control1, p.control2, p.pi2));
            break;
        }
    }
    // as KisToolFreehandHelper::endPaint() ends the update stream: flush the queued dabs
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();

    return layer->paintDevice()->convertToQImage(cs->profile(), Bounds);
}

/// the stroke as a vector stroke, painted into a tile like KisShapeLayerCanvas paints it
QImage paintVector(KisPaintOpPresetSP preset, const QVector<VStrokePrimitive> &primitives)
{
    const qreal pixelsPerPoint = 300.0 / 72.0;

    VStrokeData data;
    data.preset = preset;
    data.opacity = Opacity;
    data.brushSize = preset->settings()->paintOpSize();
    QScopedPointer<VStrokeShape> shape(VStrokeShape::fromImagePixels(primitives, pixelsPerPoint, data));
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(PaintColor)));

    QImage tile(Bounds.size(), QImage::Format_ARGB32);
    tile.fill(0);
    {
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(shape->absoluteTransformation() * QTransform::fromScale(pixelsPerPoint, pixelsPerPoint));
        shape->paint(painter); // requests the render
    }
    shape->waitForRendering();

    tile.fill(0);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(shape->absoluteTransformation() * QTransform::fromScale(pixelsPerPoint, pixelsPerPoint));
    shape->paint(painter);
    return tile;
}

/// largest difference of alpha-weighted (premultiplied) channels
int maxPremultipliedDifference(const QImage &a, const QImage &b, QPoint *where)
{
    int maxDiff = 0;
    for (int y = 0; y < a.height(); y++) {
        for (int x = 0; x < a.width(); x++) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            const int aa = qAlpha(pa);
            const int ab = qAlpha(pb);
            int diff = qAbs(aa - ab);
            diff = qMax(diff, qAbs(qRed(pa) * aa / 255 - qRed(pb) * ab / 255));
            diff = qMax(diff, qAbs(qGreen(pa) * aa / 255 - qGreen(pb) * ab / 255));
            diff = qMax(diff, qAbs(qBlue(pa) * aa / 255 - qBlue(pb) * ab / 255));
            if (diff > maxDiff) {
                maxDiff = diff;
                *where = QPoint(x, y);
            }
        }
    }
    return maxDiff;
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

/// soft round pixel brush without scatter
KisPaintOpPresetSP plainPixelBrushPreset()
{
    KisPaintOpPresetSP preset = pixelBrushPreset();
    KisScatterOptionData scatter;
    scatter.read(preset->settings().data());
    scatter.isChecked = false;
    scatter.write(preset->settings().data());
    return preset;
}

KisPaintOpPresetSP plainCspBrushPreset()
{
    KisPaintOpPresetSP preset = cspBrushPreset();
    if (preset) {
        preset->settings()->setProperty("CSP/BrushUseSpray", 0);
    }
    return preset;
}

}

void VStrokeParityTest::initTestCase()
{
    ensurePaintOps();
}

void VStrokeParityTest::testRasterVectorParity_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel") << "paintbrush";
    QTest::newRow("csp") << "cspbrush";
}

void VStrokeParityTest::testRasterVectorParity()
{
    QFETCH(QString, paintop);
    KisPaintOpPresetSP preset = paintop == "cspbrush" ? plainCspBrushPreset() : plainPixelBrushPreset();
    QVERIFY(preset);

    const QVector<VStrokePrimitive> stroke = testStroke();
    const QImage raster = paintRaster(preset, stroke);
    const QImage vector = paintVector(preset, stroke);
    QVERIFY(alphaSum(raster) > 0);

    QPoint where;
    const int diff = maxPremultipliedDifference(raster, vector, &where);
    if (diff > 1) {
        raster.save(QString(FILES_OUTPUT_DIR) + "/parity_" + paintop + "_raster.png");
        vector.save(QString(FILES_OUTPUT_DIR) + "/parity_" + paintop + "_vector.png");
    }
    QVERIFY2(diff <= 1, qPrintable(QString("difference %1 at (%2, %3): raster %4, vector %5, alpha sums %6 / %7")
                                   .arg(diff).arg(where.x()).arg(where.y())
                                   .arg(raster.pixel(where), 8, 16, QChar('0'))
                                   .arg(vector.pixel(where), 8, 16, QChar('0'))
                                   .arg(alphaSum(raster)).arg(alphaSum(vector))));
}

KISTEST_MAIN(VStrokeParityTest)
