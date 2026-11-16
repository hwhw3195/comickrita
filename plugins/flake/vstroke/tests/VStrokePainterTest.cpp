/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokePainterTest.h"

#include <QDomDocument>
#include <QImageReader>
#include <QTest>

#include <kistest.h>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_paint_device.h>

#include "VStrokeCurves.h"
#include "VStrokePainter.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

namespace {

VStrokeRenderSettings renderSettings(KisPaintOpPresetSP preset, int seed, int perStrokeSeed)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    VStrokeRenderSettings s;
    s.preset = preset;
    s.paintColor = KoColor(QColor(20, 40, 160), cs);
    s.backgroundColor = KoColor(Qt::white, cs);
    s.opacity = 0.8;
    s.seed = seed;
    s.perStrokeSeed = perStrokeSeed;
    s.startDistance = KisDistanceInitInfo(QPointF(40, 120), 0.0, 0);
    return s;
}

QImage render(const VStrokeRenderSettings &settings, const QVector<VStrokePrimitive> &stroke)
{
    KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    if (!VStrokePainter::render(device, settings, stroke)) return QImage();
    return device->convertToQImage(nullptr, QRect(0, 0, 420, 240));
}

int coverage(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            if (qAlpha(line[x])) count++;
        }
    }
    return count;
}

QVector<VStrokePrimitive> withSizeMultiplier(QVector<VStrokePrimitive> stroke, qreal multiplier)
{
    for (VStrokePrimitive &p : stroke) {
        p.pi1.setSizeMultiplier(multiplier);
        p.pi2.setSizeMultiplier(multiplier);
    }
    return stroke;
}

QVector<VStrokePrimitive> withDensity(QVector<VStrokePrimitive> stroke, qreal scale, qreal offset)
{
    for (VStrokePrimitive &p : stroke) {
        p.pi1.setDensity(scale, offset);
        p.pi2.setDensity(scale, offset);
    }
    return stroke;
}

qint64 alphaSum(const QImage &image)
{
    qint64 sum = 0;
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) sum += qAlpha(line[x]);
    }
    return sum;
}

bool isEmpty(const QImage &image)
{
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            if (qAlpha(line[x])) return false;
        }
    }
    return true;
}


/// cspbrush with a watercolor edge and no color mixing: indirect painting, through a stroke buffer
KisPaintOpPresetSP waterEdgePreset()
{
    QImageReader reader(QString(CSPBRUSH_DATA_DIR) + "mypen.kpp", "PNG");
    QDomDocument doc;
    if (!doc.setContent(reader.text("preset"))) return KisPaintOpPresetSP();
    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    for (QDomElement p = doc.documentElement().firstChildElement("param"); !p.isNull(); p = p.nextSiblingElement("param")) {
        settings->setProperty(p.attribute("name"), p.text());
    }
    settings->setProperty("paintop", "cspbrush");
    settings->setProperty("CSP/BrushUseWaterColor", false);
    settings->setProperty("CSP/BrushUseWaterEdge", 1);
    settings->setProperty("CSP/BrushWaterEdgeRadius", 3);
    return makePreset(settings, "vstroke_test_csp_water_edge");
}

QVector<VStrokePrimitive> line(const QPointF &from, const QPointF &to, int steps)
{
    QVector<VStrokePrimitive> stroke;
    KisPaintInformation previous = info(from, 0.8, 0);
    for (int i = 1; i <= steps; i++) {
        const KisPaintInformation next = info(from + (to - from) * i / steps, 0.8, i * 10);
        stroke << VStrokePrimitive::line(previous, next);
        previous = next;
    }
    return stroke;
}

}

void VStrokePainterTest::initTestCase()
{
    ensurePaintOps();
}

void VStrokePainterTest::testPrimitiveXmlRoundTrip()
{
    Q_FOREACH (const VStrokePrimitive &p, testStroke()) {
        QDomDocument doc;
        QDomElement e = doc.createElement("p");
        p.toXML(doc, e);
        const VStrokePrimitive q = VStrokePrimitive::fromXML(e);

        QCOMPARE(q.type, p.type);
        QCOMPARE(q.positions(), p.positions());
        QCOMPARE(q.pi1.pressure(), p.pi1.pressure());
        QCOMPARE(q.pi1.currentTime(), p.pi1.currentTime());
        if (p.type != VStrokePrimitive::Point) {
            QCOMPARE(q.pi2.pressure(), p.pi2.pressure());
        }
    }
}

void VStrokePainterTest::testDeterminism_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel-scatter") << "paintbrush";
    QTest::newRow("csp-spray") << "cspbrush";
}

void VStrokePainterTest::testDeterminism()
{
    QFETCH(QString, paintop);

    KisPaintOpPresetSP preset = paintop == "cspbrush" ? cspBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);

    const QVector<VStrokePrimitive> stroke = testStroke();

    const QImage first = render(renderSettings(preset, 1234, 5678), stroke);
    QVERIFY2(!first.isNull(), "no paintop for the preset");
    QVERIFY(!isEmpty(first));

    const QImage second = render(renderSettings(preset, 1234, 5678), stroke);
    QVERIFY(first == second);

    // the brush really is random, otherwise the check above proves nothing
    const QImage reseeded = render(renderSettings(preset, 4321, 5678), stroke);
    QVERIFY(first != reseeded);
}

void VStrokePainterTest::testSizeMultiplier_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel") << "paintbrush";
    QTest::newRow("csp") << "cspbrush";
}

void VStrokePainterTest::testSizeMultiplier()
{
    QFETCH(QString, paintop);

    KisPaintInformation a = info(QPointF(0, 0), 0.5, 0);
    KisPaintInformation b = info(QPointF(10, 0), 0.5, 10);
    QCOMPARE(a.sizeMultiplier(), 1.0);
    b.setSizeMultiplier(0.5);
    QCOMPARE(KisPaintInformation(b).sizeMultiplier(), 0.5);
    // interpolated between samples, as dabs along a line are
    QCOMPARE(KisPaintInformation::mix(0.5, a, b).sizeMultiplier(), 0.75);

    KisPaintOpPresetSP preset = paintop == "cspbrush" ? cspBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);
    const QVector<VStrokePrimitive> stroke = testStroke();

    const QImage plain = render(renderSettings(preset, 1234, 5678), stroke);
    const QImage one = render(renderSettings(preset, 1234, 5678), withSizeMultiplier(stroke, 1.0));
    const QImage half = render(renderSettings(preset, 1234, 5678), withSizeMultiplier(stroke, 0.5));
    QVERIFY(!isEmpty(plain));
    QVERIFY(one == plain);
    QVERIFY2(coverage(half) < coverage(plain) * 3 / 4,
             qPrintable(QString("half %1, plain %2").arg(coverage(half)).arg(coverage(plain))));
    QVERIFY(!isEmpty(half));
}

void VStrokePainterTest::testDensity_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel") << "paintbrush";
    QTest::newRow("csp") << "cspbrush";
}

void VStrokePainterTest::testDensity()
{
    QFETCH(QString, paintop);

    KisPaintInformation a = info(QPointF(0, 0), 0.5, 0);
    KisPaintInformation b = info(QPointF(10, 0), 0.5, 10);
    QCOMPARE(a.densityScale(), 1.0);
    QCOMPARE(a.densityOffset(), 0.0);
    QCOMPARE(a.applyDensity(0.3), 0.3);
    b.setDensity(0.0, 1.0);
    QCOMPARE(KisPaintInformation(b).densityOffset(), 1.0);
    QCOMPARE(b.applyDensity(0.3), 1.0);
    // interpolated between samples, as dabs along a line are
    const KisPaintInformation middle = KisPaintInformation::mix(0.5, a, b);
    QCOMPARE(middle.densityScale(), 0.5);
    QCOMPARE(middle.densityOffset(), 0.5);
    QCOMPARE(middle.applyDensity(0.2), 0.6);

    KisPaintOpPresetSP preset = paintop == "cspbrush" ? cspBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);
    const QVector<VStrokePrimitive> stroke = testStroke();

    const QImage plain = render(renderSettings(preset, 1234, 5678), stroke);
    const QImage asDrawn = render(renderSettings(preset, 1234, 5678), withDensity(stroke, 1.0, 0.0));
    const QImage light = render(renderSettings(preset, 1234, 5678), withDensity(stroke, 0.0, 0.15));
    const QImage solid = render(renderSettings(preset, 1234, 5678), withDensity(stroke, 0.0, 1.0));
    QVERIFY(!isEmpty(plain));
    QVERIFY(asDrawn == plain);
    QVERIFY2(alphaSum(light) < alphaSum(plain) * 6 / 10,
             qPrintable(QString("light %1, plain %2").arg(alphaSum(light)).arg(alphaSum(plain))));
    QVERIFY(!isEmpty(light));
    QVERIFY2(alphaSum(solid) >= alphaSum(plain),
             qPrintable(QString("solid %1, plain %2").arg(alphaSum(solid)).arg(alphaSum(plain))));
}

void VStrokePainterTest::testCurvesReplayAsDrawn_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel-scatter") << "paintbrush";
    QTest::newRow("csp-spray") << "cspbrush";
}

void VStrokePainterTest::testCurvesReplayAsDrawn()
{
    QFETCH(QString, paintop);
    const qreal brushSize = 20;

    // a stroke with a gap and repeated dabs, besides every primitive type
    QVector<VStrokePrimitive> stroke = testStroke();
    KisPaintInformation tilted = info(QPointF(100, 200), 0.4, 300);
    tilted.setCanvasRotation(30);
    tilted.setCanvasMirroredH(true);
    const int afterGap = stroke.size();
    stroke << VStrokePrimitive::point(tilted) << VStrokePrimitive::point(tilted);
    stroke << VStrokePrimitive::line(tilted, info(QPointF(140, 210), 0.7, 316));

    const VCurveData curves = VStrokeCurves::fromPrimitives(stroke, brushSize);
    QCOMPARE(curves.curveCount(), 3);
    QVERIFY(!curves.attributes.contains("x-tilt")); // all default: left out
    stroke[afterGap].restartsStroke = true; // only the gap restarts it, not the repeated dab

    const QVector<VStrokePrimitive> replayed = VStrokeCurves::toPrimitives(curves, brushSize);
    QCOMPARE(replayed.size(), stroke.size());
    for (int i = 0; i < stroke.size(); i++) {
        const VStrokePrimitive &a = replayed[i];
        const VStrokePrimitive &e = stroke[i];
        QCOMPARE(a.type, e.type);
        QCOMPARE(a.restartsStroke, e.restartsStroke);
        QCOMPARE(a.positions(), e.positions());
        QCOMPARE(a.pi1.pressure(), e.pi1.pressure());
        QCOMPARE(a.pi1.currentTime(), e.pi1.currentTime());
        QCOMPARE(a.pi1.canvasRotation(), e.pi1.canvasRotation());
        QCOMPARE(a.pi1.canvasMirroredH(), e.pi1.canvasMirroredH());
        QCOMPARE(a.pi1.sizeMultiplier(), 1.0);
        if (e.type != VStrokePrimitive::Point) {
            QCOMPARE(a.pi2.pressure(), e.pi2.pressure());
            QCOMPARE(a.pi2.currentTime(), e.pi2.currentTime());
        }
    }

    KisPaintOpPresetSP preset = paintop == "cspbrush" ? cspBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);
    const QImage drawn = render(renderSettings(preset, 1234, 5678), stroke);
    QVERIFY(!isEmpty(drawn));
    QVERIFY(render(renderSettings(preset, 1234, 5678), replayed) == drawn);
}

void VStrokePainterTest::testGapBetweenPiecesStaysEmpty_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("csp-ribbon") << "ribbon";
    QTest::newRow("pixel-scatter") << "paintbrush";
}

/// pieces of one stroke, as a trim leaves them: nothing paints across the gap between them
void VStrokePainterTest::testGapBetweenPiecesStaysEmpty()
{
    QFETCH(QString, paintop);
    const qreal brushSize = 20;

    auto piece = [] (qreal from, qreal to) {
        QVector<VStrokePrimitive> stroke;
        KisPaintInformation previous = info(QPointF(from, 120), 0.8, from);
        for (qreal x = from + 10; x <= to; x += 10) {
            const KisPaintInformation next = info(QPointF(x, 120), 0.8, x);
            stroke << VStrokePrimitive::line(previous, next);
            previous = next;
        }
        return stroke;
    };
    const VCurveData curves = VStrokeCurves::fromPrimitives(piece(20, 100) + piece(320, 400), brushSize);
    QCOMPARE(curves.curveCount(), 2);

    KisPaintOpPresetSP preset = paintop == "ribbon" ? ribbonBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);
    const QImage image = render(renderSettings(preset, 1234, 5678), VStrokeCurves::toPrimitives(curves, brushSize));
    QVERIFY(!isEmpty(image));
    QVERIFY(!isEmpty(image.copy(0, 0, 110, image.height())));
    QVERIFY(!isEmpty(image.copy(310, 0, 110, image.height())));
    // farther from either piece than the brush reaches
    QVERIFY(isEmpty(image.copy(190, 0, 40, image.height())));
}

void VStrokePainterTest::testEditedRadiusScalesDabs()
{
    const qreal brushSize = 20;
    VCurveData curves = VStrokeCurves::fromPrimitives(testStroke(), brushSize);
    for (qreal &r : curves.radii) r *= 0.5;

    const QVector<VStrokePrimitive> replayed = VStrokeCurves::toPrimitives(curves, brushSize);
    QCOMPARE(replayed[1].pi2.sizeMultiplier(), 0.5);

    KisPaintOpPresetSP preset = pixelBrushPreset();
    const QImage full = render(renderSettings(preset, 1234, 5678), testStroke());
    const QImage thin = render(renderSettings(preset, 1234, 5678), replayed);
    QVERIFY(coverage(thin) < coverage(full) * 3 / 4);
}

/// a piece of a stroke painted over another piece (a pen-lift tip, a trim's piece) doesn't
/// clear it: a watercolor edge rewrites whole dab rectangles of the device it paints on
void VStrokePainterTest::testLaterPieceKeepsEarlierInk()
{
    KisPaintOpPresetSP preset = waterEdgePreset();
    QVERIFY(preset);
    QVERIFY(!preset->settings()->paintIncremental());
    const QVector<VStrokePrimitive> body = line(QPointF(20, 120), QPointF(300, 120), 28);
    QVector<VStrokePrimitive> tip = line(QPointF(160, 128), QPointF(170, 128), 2);
    tip.first().restartsStroke = true;

    const VStrokeRenderSettings settings = renderSettings(preset, 1234, 5678);
    const QImage alone = render(settings, body);
    const QImage both = render(settings, body + tip);
    QVERIFY(!isEmpty(alone));
    int cleared = 0;
    for (int y = 0; y < alone.height(); y++) {
        for (int x = 0; x < alone.width(); x++) {
            if (qAlpha(both.pixel(x, y)) + 8 < qAlpha(alone.pixel(x, y))) cleared++;
        }
    }
    QCOMPARE(cleared, 0);
}

/// sub-paths the freehand helper starts a hair away go on with the stroke; pieces apart restart
void VStrokePainterTest::testTipsInsideTheInkContinue()
{
    const qreal brushSize = 20; // radius 8 at pressure 0.8
    auto withSecond = [&] (const QPointF &start) {
        QVector<VStrokePrimitive> stroke = line(QPointF(20, 120), QPointF(100, 120), 8);
        stroke += line(start, start + QPointF(0.5, 0.3), 3);
        return VStrokeCurves::fromPrimitives(stroke, brushSize);
    };
    auto restarts = [&] (const VCurveData &curves) {
        int count = 0;
        for (const VStrokePrimitive &p : VStrokeCurves::toPrimitives(curves, brushSize)) count += p.restartsStroke;
        return count;
    };
    const VCurveData jitter = withSecond(QPointF(100.02, 120.01));
    QCOMPARE(jitter.curveCount(), 2);
    QCOMPARE(restarts(jitter), 0);
    QCOMPARE(restarts(withSecond(QPointF(105, 120))), 0); // within the radius
    QCOMPARE(restarts(withSecond(QPointF(130, 120))), 1); // a trim's gap
}

KISTEST_MAIN(VStrokePainterTest)
