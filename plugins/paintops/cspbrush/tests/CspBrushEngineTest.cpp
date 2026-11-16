/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushEngineTest.h"

#include <QBuffer>
#include <QFile>
#include <QDomDocument>
#include <QImageReader>
#include <QImageWriter>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTest>
#include <simpletest.h>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_algebra_2d.h>
#include <kis_fixed_paint_device.h>
#include <kis_global.h>
#include <kis_paint_device.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_painter.h>
#include <KoCompositeOpRegistry.h>
#include <kis_paint_information.h>
#include <kis_distance_information.h>
#include <kis_random_source.h>
#include <KisPerStrokeRandomSource.h>
#include <kis_properties_configuration.h>
#include <KisLocalStrokeResources.h>
#include <KoCanvasResourcesIds.h>
#include <KoLocalStrokeCanvasResources.h>
#include <kis_gbr_brush.h>
#include <kis_imagepipe_brush.h>
#include <KisGlobalResourcesInterface.h>
#include <resources/KoPattern.h>
#include <KisTextureOptionData.h>
#include <KisStandardOptionData.h>
#include <QSet>
#include <algorithm>
#include <functional>
#include <kis_pixel_selection.h>
#include <kis_selection.h>

#include "CspBrushData.h"
#include "CspBrushParams.h"
#include "CspColorMath.h"
#include "CspColorJitter.h"
#include "CspColorMixer.h"
#include "CspEffector.h"
#include "CspRibbonBuilder.h"
#include "CspStrokeBuffer.h"
#include "CspTextureStage.h"
#include "CspTipImport.h"
#include "CspTipSet.h"
#include "KisCspBrushPaintOp.h"
#include "KisCspBrushSettings.h"

void CspBrushEngineTest::testEffectorRoundTrip()
{
    // off with an empty string
    QVERIFY(!CspEffector::fromString(QString()).isActive());

    const CspEffector e = CspEffector::fromString(
        "on=p,f,r;p=0.3@0,0 0.5,0.6,c 1,1;r=-0.5;f=0.2@0,1 1,0;fl=200;fp=1;tml=500");
    QVERIFY(e.isEnabled(CspEffector::Pressure));
    QVERIFY(e.isEnabled(CspEffector::Fade));
    QVERIFY(e.isEnabled(CspEffector::Random));
    QVERIFY(!e.isEnabled(CspEffector::Tilt));
    QCOMPARE(e.minimum(CspEffector::Pressure), 0.3);
    QCOMPARE(e.minimum(CspEffector::Random), -0.5);
    QCOMPARE(e.curve(CspEffector::Pressure).size(), 3);
    QVERIFY(e.curve(CspEffector::Pressure)[1].isSetAsCorner());
    QVERIFY(!e.curve(CspEffector::Pressure)[0].isSetAsCorner());
    QCOMPARE(e.length(CspEffector::Fade), 200);
    QVERIFY(e.isPeriodic(CspEffector::Fade));
    QCOMPARE(e.length(CspEffector::Time), 500);
    QCOMPARE(e.length(CspEffector::Distance), CspEffector::defaultLength(CspEffector::Distance));

    const CspEffector again = CspEffector::fromString(e.toString());
    QCOMPARE(again.toString(), e.toString());

    // strings of the pre-fade format still parse
    const CspEffector old = CspEffector::fromString("on=p,v;p=0.3@0,0 0.5,0.6 1,1;t=1;v=0.92@0,0 1,1;r=0.85;tmax=100");
    QVERIFY(old.isEnabled(CspEffector::Velocity));
    QCOMPARE(old.minimum(CspEffector::Velocity), 0.92);
    QCOMPARE(old.curve(CspEffector::Pressure).size(), 3);
}

void CspBrushEngineTest::testEffectorValue()
{
    KisPaintInformation info(QPointF(0, 0), 0.0); // upright pen: CSP tilt 0
    // the minimum: 50% at zero pressure
    const CspEffector pressure = CspEffector::fromString("on=p;p=0.5");
    QCOMPARE(pressure.value(info), 0.5);
    info.setPressure(1.0);
    QCOMPARE(pressure.value(info), 1.0);
    QCOMPARE(CspEffector().value(info), 1.0);

    // inputs multiply: pressure (0 -> 0.5) x tilt (upright = 0 -> 0.2)
    info.setPressure(0.0);
    QVERIFY(qAbs(CspEffector::fromString("on=p,t;p=0.5;t=0.2").value(info) - 0.1) < 1e-9);

    // curves are smooth splines, not polylines
    const CspEffector smooth = CspEffector::fromString("on=p;p=0@0,0 0.5,0.8 1,1");
    info.setPressure(0.25);
    const qreal v = smooth.value(info);
    QVERIFY(v > 0.45 && v < 0.8);

    // a negative random minimum makes the factor negative sometimes, as in CSP
    const CspEffector random = CspEffector::fromString("on=r;r=-1");
    KisPaintInformation r(QPointF(0, 0), 1.0);
    r.setRandomSource(new KisRandomSource(3));
    qreal lowest = 1.0;
    for (int i = 0; i < 200; i++) lowest = qMin(lowest, random.value(r));
    QVERIFY(lowest < 0.0);
}

static qreal valueAtDab(const CspEffector &effector, int seqNo)
{
    KisPaintInformation info(QPointF(0, 0), 1.0);
    KisDistanceInformation distance = KisDistanceInitInfo(-1.0, -1.0, seqNo).makeDistInfo();
    KisPaintInformation::DistanceInformationRegistrar r = info.registerDistanceInformation(&distance);
    return effector.value(info);
}

void CspBrushEngineTest::testEffectorFade()
{
    // fade over 10 dabs, from 1 down to the 0.2 minimum
    CspEffector fade = CspEffector::fromString("on=f;f=0.2@0,1 1,0;fl=10");
    QVERIFY(qAbs(valueAtDab(fade, 0) - 1.0) < 1e-3);
    QVERIFY(qAbs(valueAtDab(fade, 10) - 0.2) < 1e-3);
    QVERIFY(qAbs(valueAtDab(fade, 50) - 0.2) < 1e-3);
    // periodic: starts over
    fade.setPeriodic(CspEffector::Fade, true);
    QVERIFY(qAbs(valueAtDab(fade, 50) - 1.0) < 1e-3);
}

void CspBrushEngineTest::testVelocityMapping()
{
    // matches sut2kpp.dynamics.csp_velocity_x: slow = 1, >= 2 px/ms = 0
    QCOMPARE(CspEffector::velocityFromKritaSpeed(0.0, 2.0, 0.3), 1.0);
    QCOMPARE(CspEffector::velocityFromKritaSpeed(2.0 / 30.0, 2.0, 0.3), 0.0);
    QCOMPARE(CspEffector::velocityFromKritaSpeed(1.0, 2.0, 0.3), 0.0);
    const qreal mid = CspEffector::velocityFromKritaSpeed(0.3 / 30.0, 2.0, 0.3);
    QVERIFY(qAbs(mid - (1.0 - std::log1p(1.0) / std::log1p(2.0 / 0.3))) < 1e-9);
}

void CspBrushEngineTest::testRepeatMethods_data()
{
    QTest::addColumn<int>("method");
    QTest::addColumn<QVector<int>>("expected");

    QTest::newRow("repeat") << int(CspBrushParams::Repeat) << QVector<int>{0, 1, 2, 0, 1, 2, 0};
    QTest::newRow("reverse") << int(CspBrushParams::Reverse) << QVector<int>{0, 1, 2, 1, 0, 1, 2};
    QTest::newRow("no repeat") << int(CspBrushParams::NoRepeat) << QVector<int>{0, 1, 2, 2, 2, 2, 2};
    QTest::newRow("one time only") << int(CspBrushParams::OneTimeOnly) << QVector<int>{0, 1, 2, -1, -1, -1, -1};
}

void CspBrushEngineTest::testRepeatMethods()
{
    QFETCH(int, method);
    QFETCH(QVector<int>, expected);

    CspBrushParams params;
    params.useMaterial = true;
    params.repeatMethod = method;
    for (int i = 0; i < 3; i++) {
        CspBrushParams::Tip tip;
        tip.image = QImage(4, 4, QImage::Format_ARGB32);
        tip.image.fill(Qt::black);
        params.tips << tip;
    }

    CspTipSet tips(params);
    QVector<int> got;
    for (int i = 0; i < expected.size(); i++) got << tips.nextTipIndex(KisRandomSourceSP());
    QCOMPARE(got, expected);
}

void CspBrushEngineTest::testColorMixing()
{
    using namespace CspColorMath;
    const Rgb red = {1, 0, 0};
    const Rgb roundTrip = oklabToSrgb(srgbToOklab(red));
    for (int i = 0; i < 3; i++) QVERIFY(qAbs(roundTrip[i] - red[i]) < 1e-3);

    const Rgb black = {0, 0, 0};
    const Rgb white = {1, 1, 1};
    QCOMPARE(mix(black, white, 0.5, Mode::Standard)[0], 0.5f);
    // OkLab midpoint of black and white: neutral gray, L = 0.5 (about sRGB 0.39)
    const Rgb oklab = mix(black, white, 0.5, Mode::Oklab);
    QVERIFY(qAbs(oklab[0] - oklab[1]) < 1e-3);
    QVERIFY(oklab[0] > 0.3 && oklab[0] < 0.6);
    // spectral: still neutral, darker (black pigment is strong)
    const Rgb perceptual = mix(black, white, 0.5, Mode::Perceptual);
    QVERIFY(qAbs(perceptual[0] - perceptual[1]) < 2e-3);
    QVERIFY(perceptual[0] > 0.1 && perceptual[0] < 0.6);

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor a(Qt::black, cs);
    const KoColor b(Qt::white, cs);
    QColor ends;
    mix(a, b, 0.0, Mode::Perceptual).toQColor(&ends);
    QCOMPARE(ends, QColor(Qt::black));
    mix(a, b, 1.0, Mode::Perceptual).toQColor(&ends);
    QCOMPARE(ends, QColor(Qt::white));
}

void CspBrushEngineTest::testSpectralMixing()
{
    using namespace CspColorMath;
    if (!hasSpectral()) QSKIP("built without libmypaint");

    const Rgb blue = {0, 0, 1};
    const Rgb yellow = {1, 1, 0};
    const Rgb red = {1, 0, 0};
    const Rgb green = {0.3f, 0.6f, 0.2f};

    const Rgb roundTrip = fromLogSpectrum(toLogSpectrum(green));
    for (int i = 0; i < 3; i++) QVERIFY(qAbs(roundTrip[i] - green[i]) < 2e-3);

    for (int i = 0; i < 3; i++) {
        QCOMPARE(mix(blue, yellow, 0.0, Mode::Perceptual)[i], blue[i]);
        QCOMPARE(mix(blue, yellow, 1.0, Mode::Perceptual)[i], yellow[i]);
    }

    // pigment-like: blue and yellow give green (RGB gives gray), red and yellow orange
    const Rgb by = mix(blue, yellow, 0.5, Mode::Perceptual);
    QVERIFY(by[1] > by[0] + 0.3 && by[1] > by[2]);
    const Rgb ry = mix(red, yellow, 0.5, Mode::Perceptual);
    QVERIFY(ry[0] > 0.95 && ry[1] > 0.3 && ry[1] < 0.6 && ry[2] < 0.05);
    QCOMPARE(mix(blue, yellow, 0.5, Mode::Standard)[1], 0.5f);
}

void CspBrushEngineTest::testParamsFromData()
{
    QImage tip(6, 4, QImage::Format_ARGB32);
    tip.fill(qRgba(128, 128, 128, 255));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    tip.save(&buffer, "PNG");

    CspBrushData d;
    d.set("BrushSize", "2.5");
    d.set("BrushSizeUnit", "2"); // mm, stored at 10 px per mm
    d.set("BrushUsePatternImage", "1");
    d.set("Tips/Count", 1);
    d.set("Tips/0/Name", "gray");
    d.set("Tips/0/Png", QString::fromLatin1(png.toBase64()));
    d.set("BrushAutoIntervalType", "0");
    d.set("BrushInterval", "25.0");
    d.set("BrushRibbon", "true");

    const CspBrushParams p = CspBrushParams::fromData(d);
    QCOMPARE(p.size, 250.0);
    QCOMPARE(p.tips.size(), 1);
    QVERIFY(p.tips[0].isGray);
    QCOMPARE(p.spacingFraction(), 0.25);
    QVERIFY(p.ribbon);
}

void CspBrushEngineTest::testVelocityCalibrationClamped()
{
    CspBrushData d;
    d.set("x-VelocityFast", "0");
    d.set("x-VelocityKnee", "0");
    const CspBrushParams p = CspBrushParams::fromData(d);
    QCOMPARE(p.velocityFast, 0.1);
    QCOMPARE(p.velocityKnee, 0.01);
    QVERIFY(qIsFinite(CspEffector::velocityFromKritaSpeed(0.05, p.velocityFast, p.velocityKnee)));
    QVERIFY(qIsFinite(CspEffector::velocityFromKritaSpeed(0.0, p.velocityFast, p.velocityKnee)));
}

void CspBrushEngineTest::testLineDirectionRotation()
{
    CspBrushParams params;
    params.angleInputs = CspBrushParams::AngleLineDirection;

    // A stroke going down the screen has drawing angle +90 deg (clockwise,
    // y down), so the tip's +x has to turn 90 deg clockwise: dab rotation
    // is counter-clockwise, hence -90 deg.
    KisPaintInformation info(QPointF(0, 10));
    info.overrideDrawingAngle(M_PI_2);
    QVERIFY(qAbs(KisCspBrushPaintOp::dabRotation(params, info, 0.5) - normalizeAngle(-M_PI_2)) < 1e-6);

    // CSP Angle is counter-clockwise and adds to the line direction
    params.angle = 90.0;
    QVERIFY(qAbs(KisCspBrushPaintOp::dabRotation(params, info, 0.5)) < 1e-6);
}

void CspBrushEngineTest::testDiameterClamped()
{
    CspBrushParams params;
    QCOMPARE(KisCspBrushPaintOp::dabDiameter(params, -0.5), 0.0);
    QCOMPARE(KisCspBrushPaintOp::dabDiameter(params, 0.5), 10.0);
    params.atLeast1Pixel = true;
    QCOMPARE(KisCspBrushPaintOp::dabDiameter(params, 0.0), 1.0);
}

void CspBrushEngineTest::testSprayDistribution()
{
    KisRandomSourceSP rs(new KisRandomSource(42));
    auto stats = [&](qreal deviation, qreal *meanSquared, qreal *outer) {
        const auto distance = KisCspBrushPaintOp::sprayRadialDistribution(deviation);
        const int n = 10000;
        qreal sum = 0.0;
        int far = 0;
        for (int i = 0; i < n; i++) {
            const qreal d = distance(rs);
            QVERIFY(d >= 0.0 && d <= 1.0);
            sum += d * d;
            if (d > 0.9) far++;
        }
        *meanSquared = sum / n;
        *outer = qreal(far) / n;
    };
    qreal meanSquared, outer;
    // no deviation: uniform over the spray area (E[r^2] = 1/2)
    stats(0.0, &meanSquared, &outer);
    QVERIFY(qAbs(meanSquared - 0.5) < 0.02);
    // negative deviation leans outwards, but no longer puts everything on the rim
    stats(-50.0, &meanSquared, &outer);
    QVERIFY(meanSquared > 0.5);
    QVERIFY(outer < 0.5);
    // positive deviation gathers at the centre
    stats(50.0, &meanSquared, &outer);
    QVERIFY(meanSquared < 0.25);
}

void CspBrushEngineTest::testColorJitter()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor red(Qt::red, cs);
    const KoColor blue(Qt::blue, cs);
    KisPaintInformation info(QPointF(0, 0), 1.0);
    info.setRandomSource(new KisRandomSource(7));

    // nothing on: the main color as is
    CspBrushParams params;
    params.mixingMode = CspColorMath::Mode::Standard;
    QVERIFY(!CspColorJitter(params).isActive());

    // hue per dab with CSP's default effect source (random, minimum -100%): changes both ways
    params.jitterPerDab = true;
    params.hueChange = 60.0;
    params.hueChangeDynamics = CspEffector::fromString("on=r;r=-1");
    const CspColorJitter hue(params);
    QVERIFY(hue.isActive());
    QSet<int> hues;
    bool below = false;
    bool above = false;
    for (int i = 0; i < 40; i++) {
        const int h = hue.apply(red, blue, info).toQColor().hue();
        hues << h;
        if (h > 180) below = true;          // red turned toward magenta
        if (h > 0 && h <= 60) above = true; // toward yellow
        QVERIFY(h <= 60 || h >= 300);
    }
    QVERIFY(hues.size() > 3);
    QVERIFY(below && above);

    // the sub color without an effect source: always that share of the sub color
    params.hueChange = 0.0;
    params.subColorChange = 1.0;
    QCOMPARE(CspColorJitter(params).apply(red, blue, info).toQColor(), QColor(Qt::blue));
    params.subColorChange = 0.5;
    const QColor half = CspColorJitter(params).apply(red, blue, info).toQColor();
    QVERIFY(half.red() > 100 && half.blue() > 100 && half.green() == 0);

    // per stroke: one shift for the whole stroke, another for the next stroke
    CspBrushParams stroke;
    stroke.jitterPerStroke = true;
    stroke.strokeHueChange = 90.0;
    const CspColorJitter perStroke(stroke);
    KisPaintInformation a(QPointF(0, 0), 1.0);
    a.setPerStrokeRandomSource(new KisPerStrokeRandomSource());
    const QColor first = perStroke.apply(red, blue, a).toQColor();
    a.setRandomSource(new KisRandomSource(99));
    QCOMPARE(perStroke.apply(red, blue, a).toQColor(), first);
    bool changed = false;
    for (int i = 0; i < 5 && !changed; i++) {
        KisPaintInformation b(QPointF(0, 0), 1.0);
        b.setPerStrokeRandomSource(new KisPerStrokeRandomSource());
        changed = perStroke.apply(red, blue, b).toQColor() != first;
    }
    QVERIFY(changed);
}

void CspBrushEngineTest::testNoMaskedBrush()
{
    // the Masked Brush is gone: older presets' MaskingBrush/ keys don't reach Krita's core
    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisResourcesInterfaceSP(new KisLocalStrokeResources()));
    settings->setProperty("CSP/BrushSize", 40.0);
    settings->setProperty("MaskingBrush/Enabled", true);
    settings->setProperty("MaskingBrush/Preset/paintop", "paintbrush");
    QVERIFY(!settings->hasMaskingSettings());
    QVERIFY(!settings->hasProperty("MaskingBrush/Preset/paintop"));

    settings->fromXML("<!DOCTYPE params><params><param name=\"MaskingBrush/Enabled\" type=\"bool\">true</param>"
                      "<param name=\"CSP/BrushSize\" type=\"string\">30</param></params>");
    QVERIFY(!settings->hasMaskingSettings());
    QCOMPARE(settings->paintOpSize(), 30.0);
    QVERIFY(!settings->paintIncremental());
    settings->setProperty("CSP/BrushUseWaterColor", true);
    QVERIFY(settings->paintIncremental()); // color mixing paints the layer directly
}

void CspBrushEngineTest::testDataRoundTrip()
{
    KisPropertiesConfigurationSP config(new KisPropertiesConfiguration());
    config->setProperty("CSP/BrushSize", 12.5);
    config->setProperty("CSP/x-SwapMainSub", 1);
    config->setProperty("OtherEngineKey", 3);

    CspBrushData d;
    d.read(config.data());
    QCOMPARE(d.values.size(), 2);
    QCOMPARE(d.real("BrushSize"), 12.5);

    KisPropertiesConfigurationSP out(new KisPropertiesConfiguration());
    d.write(out.data());
    QCOMPARE(out->getDouble("CSP/BrushSize"), 12.5);
    QVERIFY(!out->hasProperty("OtherEngineKey"));

    CspBrushData empty;
    empty.read(out.data());
    QVERIFY(empty == d);
}

namespace {
// a 10 x 20 gray tip: left half black, right half white, fully opaque
CspBrushParams ribbonParams()
{
    CspBrushParams params;
    params.useMaterial = true;
    params.ribbon = true;
    CspBrushParams::Tip tip;
    tip.image = QImage(10, 20, QImage::Format_ARGB32);
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 10; x++) {
            tip.image.setPixel(x, y, x < 5 ? qRgba(0, 0, 0, 255) : qRgba(255, 255, 255, 255));
        }
    }
    params.tips << tip;
    return params;
}

struct RibbonRun {
    QVector<QRect> rects;
    QVector<KisFixedPaintDeviceSP> dabs;
};

RibbonRun runRibbon(CspRibbonBuilder &ribbon, const QVector<QPointF> &points, qreal size, const KoColorSpace *cs)
{
    RibbonRun run;
    for (const QPointF &p : points) {
        CspRibbonBuilder::Sample s;
        s.pos = p;
        s.brushSize = size;
        KisFixedPaintDeviceSP dab;
        QRect rect;
        if (ribbon.addSample(s, cs, KisRandomSourceSP(), &dab, &rect)) {
            run.rects << rect;
            run.dabs << dab;
        }
    }
    return run;
}
}

void CspBrushEngineTest::testRibbonTiling()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    CspBrushParams params = ribbonParams();
    CspTipSet tips(params);
    tips.setColors(KoColor(Qt::black, cs), KoColor(Qt::white, cs), CspColorMath::Mode::Standard);
    CspRibbonBuilder ribbon(params, &tips);

    // brush size 40 = the tip's long side: the band is 20 px wide, a tile 40 px long
    QVector<QPointF> points;
    for (int i = 0; i <= 100; i++) points << QPointF(10 + 2 * i, 50);
    runRibbon(ribbon, points, 40.0, cs);

    const QVector<CspRibbonBuilder::Edge> &edges = ribbon.edges();
    QCOMPARE(edges.size(), 100); // the last sample's edge waits for a direction
    const CspRibbonBuilder::Edge &last = edges.last();
    QVERIFY(qAbs(last.v - 198.0 / 40.0) < 1e-6);
    QVERIFY(qAbs(KisAlgebra2D::norm(last.right - last.left) - 20.0) < 1e-6);
    // moving right on screen, the right-hand side is below (y down)
    QVERIFY(last.right.y() > last.left.y());
}

void CspBrushEngineTest::testRibbonNoFoldOnTightCurve()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    CspBrushParams params = ribbonParams();
    CspTipSet tips(params);
    tips.setColors(KoColor(Qt::black, cs), KoColor(Qt::white, cs), CspColorMath::Mode::Standard);
    CspRibbonBuilder ribbon(params, &tips);

    // radius 8 circle, band 30 px wide: the inner side has to pin, never run backwards
    QVector<QPointF> points;
    for (int i = 0; i <= 60; i++) {
        const qreal a = 2 * M_PI * i / 60;
        points << QPointF(100 + 8 * std::cos(a), 100 + 8 * std::sin(a));
    }
    runRibbon(ribbon, points, 60.0, cs);

    const QVector<CspRibbonBuilder::Edge> &edges = ribbon.edges();
    QVERIFY(edges.size() > 10);
    for (int i = 2; i < edges.size(); i++) {
        const QPointF travel = (points[i] - points[i - 1]) / KisAlgebra2D::norm(points[i] - points[i - 1]);
        QVERIFY(KisAlgebra2D::dotProduct(edges[i].left - edges[i - 1].left, travel) >= -1e-9);
        QVERIFY(KisAlgebra2D::dotProduct(edges[i].right - edges[i - 1].right, travel) >= -1e-9);
    }
}

void CspBrushEngineTest::testRibbonRendersTip()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    CspBrushParams params = ribbonParams();
    CspTipSet tips(params);
    tips.setColors(KoColor(Qt::red, cs), KoColor(Qt::blue, cs), CspColorMath::Mode::Standard);
    CspRibbonBuilder ribbon(params, &tips);

    // going down the screen, the image's left (black -> main, red) is on the
    // stroke's left, which is +x
    QVector<QPointF> points;
    for (int i = 0; i <= 20; i++) points << QPointF(50, 10 + 2 * i);
    const RibbonRun run = runRibbon(ribbon, points, 40.0, cs);
    QVERIFY(!run.dabs.isEmpty());

    const KisFixedPaintDeviceSP dab = run.dabs[run.dabs.size() / 2];
    const QRect rect = run.rects[run.rects.size() / 2];
    const QImage image = dab->convertToQImage(nullptr);
    auto colorAt = [&](int canvasX) {
        for (int y = 0; y < image.height(); y++) {
            const QRgb c = image.pixel(canvasX - rect.x(), y);
            if (qAlpha(c) == 255) return c;
        }
        return QRgb(0);
    };
    QCOMPARE(colorAt(55), qRgba(255, 0, 0, 255));
    QCOMPARE(colorAt(45), qRgba(0, 0, 255, 255));
}

void CspBrushEngineTest::testRibbonHasNoGaps()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    CspBrushParams params = ribbonParams();
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 10; x++) params.tips[0].image.setPixel(x, y, qRgba(0, 0, 0, 255));
    }
    CspTipSet tips(params);
    tips.setColors(KoColor(Qt::black, cs), KoColor(Qt::white, cs), CspColorMath::Mode::Standard);
    CspRibbonBuilder ribbon(params, &tips);

    // uneven sample distances, like Krita's spacing produces
    QVector<QPointF> points;
    qreal x = 10.0;
    for (int i = 0; i < 300; i++) {
        points << QPointF(x, 50.3);
        x += 2.0 + 0.37 * ((i * 7) % 5);
    }
    const RibbonRun run = runRibbon(ribbon, points, 40.0, cs);

    KisPaintDeviceSP device = new KisPaintDevice(cs);
    KisPainter gc(device);
    gc.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
    for (int i = 0; i < run.dabs.size(); i++) {
        gc.bltFixed(run.rects[i].topLeft(), run.dabs[i], run.dabs[i]->bounds());
    }

    // the band is y 40.3..60.3; check the rows fully inside it
    QString gaps;
    for (int py = 42; py <= 58; py++) {
        for (int px = 30; px < int(x) - 30; px++) {
            KoColor c(cs);
            device->pixel(px, py, &c);
            if (c.opacityU8() < 250 && gaps.size() < 200) gaps += QString("(%1,%2:%3) ").arg(px).arg(py).arg(c.opacityU8());
        }
    }
    QVERIFY2(gaps.isEmpty(), qPrintable(gaps));
}

void CspBrushEngineTest::testRibbonStaysInsideBand()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    CspBrushParams params = ribbonParams();
    params.tips[0].image.fill(qRgba(0, 0, 0, 255));
    CspTipSet tips(params);
    tips.setColors(KoColor(Qt::black, cs), KoColor(Qt::white, cs), CspColorMath::Mode::Standard);

    // a gentle arc entered as a polyline (9 px chords) sampled every 4 px, like
    // the render test: no pixel may land outside the 50 px wide band
    const QPointF center(400, 400);
    const qreal radius = 300;
    QVector<QPointF> vertices;
    for (int i = 0; i <= 60; i++) {
        const qreal a = M_PI + 0.03 * i;
        vertices << center + radius * QPointF(std::cos(a), std::sin(a));
    }
    QVector<QPointF> points;
    for (int i = 0; i + 1 < vertices.size(); i++) {
        const qreal len = KisAlgebra2D::norm(vertices[i + 1] - vertices[i]);
        for (qreal t = 0; t < len; t += 4.0) points << vertices[i] + (vertices[i + 1] - vertices[i]) * (t / len);
    }

    for (qreal size : {100.0, 250.0}) {
        CspRibbonBuilder ribbon(params, &tips);
        const RibbonRun run = runRibbon(ribbon, points, size, cs);
        QVERIFY(!run.dabs.isEmpty());
        const qreal halfWidth = 0.5 * size * 10.0 / 20.0;

        QString stray;
        for (int i = 0; i < run.dabs.size(); i++) {
            const QImage image = run.dabs[i]->convertToQImage(nullptr);
            for (int y = 0; y < image.height(); y++) {
                for (int x = 0; x < image.width(); x++) {
                    if (!qAlpha(image.pixel(x, y))) continue;
                    const QPointF p = run.rects[i].topLeft() + QPointF(x + 0.5, y + 0.5);
                    const qreal off = qAbs(KisAlgebra2D::norm(p - center) - radius);
                    if (off > halfWidth + 1.5 && stray.size() < 300) {
                        stray += QString("(%1,%2 off %3) ").arg(p.x()).arg(p.y()).arg(off - halfWidth);
                    }
                }
            }
        }
        QVERIFY2(stray.isEmpty(), qPrintable(stray));
    }
}

// Settings of a test preset: the preset XML is the .kpp PNG's "preset" text chunk
static KisPropertiesConfigurationSP loadPresetSettings(const QString &name)
{
    QImageReader reader(QString(FILES_DATA_DIR) + "/" + name, "PNG");
    QDomDocument doc;
    doc.setContent(reader.text("preset"));
    KisPropertiesConfigurationSP settings(new KisPropertiesConfiguration());
    for (QDomElement e = doc.documentElement().firstChildElement("param"); !e.isNull(); e = e.nextSiblingElement("param")) {
        settings->setProperty(e.attribute("name"), e.text());
    }
    return settings;
}

static KisPropertiesConfigurationSP textureSettings(const QImage &pattern)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    pattern.save(&buffer, "PNG");

    KisPropertiesConfigurationSP settings(new KisPropertiesConfiguration());
    settings->setProperty("Texture/Pattern/Enabled", true);
    settings->setProperty("Texture/Pattern/Pattern", QString::fromLatin1(png.toBase64()));
    settings->setProperty("Texture/Pattern/PatternMD5Sum", QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Md5).toHex()));
    settings->setProperty("Texture/Pattern/PatternFileName", "test_texture.png");
    settings->setProperty("Texture/Pattern/Name", "test_texture");
    settings->setProperty("Texture/Pattern/Scale", 1.0);
    settings->setProperty("Texture/Pattern/Brightness", 0.0);
    settings->setProperty("Texture/Pattern/Contrast", 1.0);
    settings->setProperty("Texture/Pattern/NeutralPoint", 0.5);
    settings->setProperty("Texture/Pattern/TexturingMode", int(KisTextureOptionData::MULTIPLY));
    settings->setProperty("Texture/Pattern/CutoffPolicy", 0);
    settings->setProperty("Texture/Pattern/CutoffLeft", 0);
    settings->setProperty("Texture/Pattern/CutoffRight", 255);
    return settings;
}

static QImage grayPattern(int w, int h)
{
    QImage image(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int v = (x * 53 + y * 97) % 256;
            image.setPixel(x, y, qRgba(v, v, v, 255));
        }
    }
    return image;
}

void CspBrushEngineTest::testTextureModes()
{
    for (int mode = 0; mode < CspBrushParams::TextureModeCount; mode++) {
        for (qreal a : {0.1, 0.5, 1.0}) {
            for (qreal t : {0.0, 0.5, 1.0}) {
                // no density, no effect
                QVERIFY2(qAbs(CspTextureStage::modeFunction(mode, a, t, 0.0) - a) < 1e-9, qPrintable(QString::number(mode)));
                const qreal r = CspTextureStage::modeFunction(mode, a, t, 1.0);
                QVERIFY(r >= 0.0 && r <= 1.0);
            }
        }
    }
    // white paper (t = 0) leaves Normal, Subtract and Height strokes alone
    QCOMPARE(CspTextureStage::modeFunction(CspBrushParams::TexNormal, 0.7, 0.0, 1.0), 0.7);
    QCOMPARE(CspTextureStage::modeFunction(CspBrushParams::TexSubtract, 0.7, 0.0, 1.0), 0.7);
    QCOMPARE(CspTextureStage::modeFunction(CspBrushParams::TexHeight, 0.7, 0.0, 1.0), 0.7);
    // Height keeps the solid core of a stroke longer than Subtract does
    QVERIFY(CspTextureStage::modeFunction(CspBrushParams::TexHeight, 0.9, 0.5, 1.0)
            > CspTextureStage::modeFunction(CspBrushParams::TexSubtract, 0.9, 0.5, 1.0));
}

void CspBrushEngineTest::testTextureSampling()
{
    CspBrushParams params;
    QImage texture(2, 1, QImage::Format_ARGB32);
    texture.setPixel(0, 0, qRgba(0, 0, 0, 255));       // dark: full strength
    texture.setPixel(1, 0, qRgba(255, 255, 255, 255)); // white: none
    params.textureScale = 1.0;

    CspTextureStage stage(params, texture);
    QVERIFY(stage.isEnabled());
    QCOMPARE(stage.strengthAt(0, 0), 1.0);
    QCOMPARE(stage.strengthAt(1, 0), 0.0);
    QCOMPARE(stage.strengthAt(2, 5), 1.0); // tiled
    QCOMPARE(stage.strengthAt(-1, 0), 0.0);

    params.textureInvert = true;
    QCOMPARE(CspTextureStage(params, texture).strengthAt(0, 0), 0.0);
}

void CspBrushEngineTest::testTextureLodScale()
{
    CspBrushParams params;
    params.textureScale = 1.0;
    CspTextureStage stage(params, grayPattern(5, 3));

    // a preview pixel at lod 1 samples the centre of the 2x2 canvas pixels it covers
    for (int y = -2; y < 4; y++) {
        for (int x = -2; x < 5; x++) {
            const qreal full = (stage.strengthAt(2 * x, 2 * y) + stage.strengthAt(2 * x + 1, 2 * y)
                                + stage.strengthAt(2 * x, 2 * y + 1) + stage.strengthAt(2 * x + 1, 2 * y + 1)) / 4.0;
            QVERIFY(qAbs(stage.strengthAt(x, y, 0.5) - full) < 1e-6);
        }
    }
}

void CspBrushEngineTest::testTextureOutline()
{
    const int outline = CspBrushParams::TexOutline;
    const int subtract = CspBrushParams::TexSubtract;
    // a solid core survives a texture that Subtract would halve
    QCOMPARE(CspTextureStage::modeFunction(outline, 1.0, 0.5, 1.0), 1.0);
    // and is cut where the texture is as strong as the coverage, like Subtract
    QCOMPARE(CspTextureStage::modeFunction(outline, 0.5, 1.0, 1.0), 0.0);
    // between the two, a soft ramp rather than Subtract's threshold
    const qreal mid = CspTextureStage::modeFunction(outline, 0.5, 0.5, 1.0);
    QVERIFY(mid > CspTextureStage::modeFunction(subtract, 0.5, 0.5, 1.0));
    QVERIFY(mid > 0.0 && mid < 0.5);
    // monotonic in the coverage
    qreal last = 0.0;
    for (int i = 0; i <= 20; i++) {
        const qreal r = CspTextureStage::modeFunction(outline, i / 20.0, 0.6, 0.8);
        QVERIFY(r >= last);
        last = r;
    }
}

void CspBrushEngineTest::testTextureRotation()
{
    CspBrushParams params;
    const QImage image = grayPattern(7, 5);
    const CspTextureStage straight(params, image);
    params.textureRotation = 90.0;
    const CspTextureStage turned(params, image);
    // QTransform's 90 degrees takes (x, y) to (-y, x): pixel centres stay on texel centres
    for (int y = -3; y < 6; y++) {
        for (int x = -4; x < 8; x++) {
            QVERIFY(qAbs(turned.strengthAt(x, y) - straight.strengthAt(-y - 1, x)) < 1e-6);
        }
    }
}

void CspBrushEngineTest::testTexturePattern()
{
    // the image comes from Krita's pattern: the library first, else the copy in the preset
    const QImage image = grayPattern(7, 5);
    KisPropertiesConfigurationSP settings = textureSettings(image);
    const QImage embedded = CspTextureStage::loadPattern(settings.data(), KisResourcesInterfaceSP(new KisLocalStrokeResources()));
    QCOMPARE(embedded.size(), image.size());
    QCOMPARE(embedded.pixel(3, 2), image.pixel(3, 2));

    QImage other = grayPattern(7, 5);
    other.invertPixels();
    KoPatternSP library(new KoPattern(other, "test_texture", "test_texture.png"));
    settings->setProperty("Texture/Pattern/PatternMD5Sum", library->md5Sum());
    const QImage linked = CspTextureStage::loadPattern(settings.data(), KisResourcesInterfaceSP(new KisLocalStrokeResources({library})));
    QCOMPARE(linked.pixel(3, 2), other.pixel(3, 2));

    // off: no image
    settings->setProperty("Texture/Pattern/Enabled", false);
    QVERIFY(CspTextureStage::loadPattern(settings.data(), KisResourcesInterfaceSP(new KisLocalStrokeResources())).isNull());

    // a preset with its pattern embedded, the CSP settings drive the stage
    const KisPropertiesConfigurationSP texturePen = loadPresetSettings("texture_pen.kpp");
    const QImage paper = CspTextureStage::loadPattern(texturePen.data(), KisResourcesInterfaceSP(new KisLocalStrokeResources()));
    QVERIFY(!paper.isNull());
    const CspBrushParams params = CspBrushParams::fromSettings(texturePen.data());
    QVERIFY(CspTextureStage(params, paper).isEnabled());
}

void CspBrushEngineTest::testWaterEdgeStrength()
{
    // 40 x 40 area, opaque 20 x 20 square in the middle
    const int w = 40;
    QVector<float> alpha(w * w, 0.0f);
    for (int y = 10; y < 30; y++) {
        for (int x = 10; x < 30; x++) alpha[y * w + x] = 1.0f;
    }
    const QVector<float> edge = CspStrokeBuffer::edgeStrength(alpha, w, w, 6);
    QCOMPARE(edge[20 * w + 20], 0.0f);       // centre: no edge
    QVERIFY(edge[20 * w + 10] > 0.8f);       // border
    QVERIFY(edge[20 * w + 12] < edge[20 * w + 10]);
    QCOMPARE(edge[5 * w + 5], 0.0f);         // outside the stroke
}

void CspBrushEngineTest::testColorMixerEndpoints()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor red(Qt::red, cs);
    const KoColor blue(Qt::blue, cs);
    KoColor clear(Qt::blue, cs);
    clear.setOpacity(quint8(0));
    QColor q;

    CspBrushParams params;
    params.mixingMode = CspColorMath::Mode::Standard;
    for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixRunningColor, CspBrushParams::MixSmear}) {
        params.mixingType = type;
        // CSP manual: amount 100 -> no background; amount 0 -> based on the background
        CspColorMixer(params).paintColor(blue, red, 1.0, 1.0).toQColor(&q);
        QCOMPARE(q, QColor(Qt::red));
        CspColorMixer(params).paintColor(blue, red, 0.0, 1.0).toQColor(&q);
        QCOMPARE(q, QColor(Qt::blue));
        // an empty layer brings no color: the drawing color alone, at any amount above 0
        CspColorMixer(params).paintColor(clear, red, 0.5, 1.0).toQColor(&q);
        QCOMPARE(q, QColor(Qt::red));
        // on an opaque canvas (the scratchpad, a filled layer) the paint is opaque at any density
        QCOMPARE(CspColorMixer(params).paintColor(blue, red, 0.5, 0.5).opacityU8(), quint8(255));
        // with no paint at all on an empty layer there is nothing to lay down
        QCOMPARE(CspColorMixer(params).paintColor(clear, red, 0.0, 1.0).opacityU8(), quint8(0));
    }

    // Density of paint: the alpha over an empty layer. Smear does not use it
    params.mixingType = CspBrushParams::MixBlend;
    QCOMPARE(CspColorMixer(params).paintColor(clear, red, 1.0, 0.0).opacityU8(), quint8(0));
    QCOMPARE(CspColorMixer(params).paintColor(clear, red, 1.0, 1.0).opacityU8(), quint8(255));
    const qreal half = CspColorMixer(params).paintColor(clear, red, 1.0, 0.5).opacityF();
    QVERIFY2(qAbs(half - 0.82) < 0.02, qPrintable(QString::number(half))); // CSP: 83% at Density 50
    params.mixingType = CspBrushParams::MixSmear;
    QCOMPARE(CspColorMixer(params).paintColor(clear, red, 1.0, 0.0).opacityU8(), quint8(255));

    // the stroke starts with more canvas color than it settles at
    for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixRunningColor, CspBrushParams::MixSmear}) {
        QVERIFY(CspColorMixer::startCanvasShare(0.5, type) > CspColorMixer::canvasShare(0.5, type));
        QCOMPARE(CspColorMixer::startCanvasShare(0.0, type), 1.0);
        QCOMPARE(CspColorMixer::startCanvasShare(1.0, type), 0.0);
    }
}

void CspBrushEngineTest::testColorMixerStretch()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor red(Qt::red, cs);
    const KoColor blue(Qt::blue, cs);
    const KoColor green(Qt::green, cs);
    KoColor clear(Qt::blue, cs);
    clear.setOpacity(quint8(0));

    CspBrushParams params;
    params.mixingMode = CspColorMath::Mode::Standard;
    params.mixingType = CspBrushParams::MixBlend;

    // a long Color stretch drags the start color further: started on blue, then over green
    auto blueAfter = [&](qreal stretch, int dabs, qreal step) {
        params.colorStretch = stretch;
        CspColorMixer mixer(params);
        QColor q;
        mixer.paintColor(blue, red, 0.0, 1.0).toQColor(&q);
        for (int i = 0; i < dabs; i++) mixer.paintColor(green, red, 0.0, 1.0, step).toQColor(&q);
        return q.blue();
    };
    QVERIFY(blueAfter(0.9, 10, 0.1) > blueAfter(0.5, 10, 0.1));
    QVERIFY(blueAfter(0.5, 10, 0.1) > blueAfter(0.1, 10, 0.1));
    // the same distance in more, smaller steps gives the same color: spacing does not change the mixing
    QVERIFY(qAbs(blueAfter(0.5, 10, 0.1) - blueAfter(0.5, 100, 0.01)) <= 2);
    // half of the difference goes every halfDistance()
    const qreal half = CspColorMixer::halfDistance(0.5, CspBrushParams::MixBlend);
    QVERIFY(qAbs(blueAfter(0.5, 20, half / 20) - 128) <= 2);
    // a brush that does not move keeps its color
    QCOMPARE(blueAfter(0.5, 50, 0.0), 255);

    for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixRunningColor, CspBrushParams::MixSmear}) {
        qreal last = 0.0;
        for (int stretch = 0; stretch <= 100; stretch++) {
            const qreal h = CspColorMixer::halfDistance(stretch / 100.0, type);
            QVERIFY2(h >= last && h > 0.0, qPrintable(QString("type %1, stretch %2").arg(type).arg(stretch)));
            last = h;
        }
    }
    // Running color at 100 never gives the start color up
    QVERIFY(qIsInf(CspColorMixer::halfDistance(1.0, CspBrushParams::MixRunningColor)));
    QVERIFY(!qIsInf(CspColorMixer::halfDistance(1.0, CspBrushParams::MixBlend)));

    // Amount of paint 0: over an empty layer the color picked up is dragged on unchanged, in every mode
    for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixRunningColor, CspBrushParams::MixSmear}) {
        params.mixingType = type;
        params.colorStretch = 0.1;
        CspColorMixer mixer(params);
        mixer.paintColor(blue, red, 0.0, 0.5);
        KoColor paint;
        for (int i = 0; i < 200; i++) paint = mixer.paintColor(clear, red, 0.0, 0.5, 0.1);
        QCOMPARE(paint.toQColor().blue(), 255);
        QCOMPARE(paint.toQColor().red(), 0);
        QVERIFY2(paint.opacityF() > 0.8, qPrintable(QString("type %1: %2").arg(type).arg(paint.opacityF())));
    }
}

void CspBrushEngineTest::testColorMixerErases()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor red(Qt::red, cs);
    const KoColor blue(Qt::blue, cs);
    CspBrushParams params;
    params.mixingMode = CspColorMath::Mode::Standard;

    for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixRunningColor, CspBrushParams::MixSmear}) {
        params.mixingType = type;
        // the brush color plays no part, at any amount of paint; density takes alpha down
        for (qreal amount : {0.0, 1.0}) {
            KoColor paint = CspColorMixer(params, true).paintColor(blue, red, amount, 0.5);
            QCOMPARE(paint.toQColor().blue(), 255);
            QCOMPARE(paint.toQColor().red(), 0);
            QVERIFY(qAbs(paint.opacityF() - 0.5) < 0.01);
            QCOMPARE(CspColorMixer(params, true).paintColor(blue, red, amount, 1.0).opacityU8(), quint8(0));
        }
    }

    // a half transparent canvas: the alpha is taken down from there
    KoColor half = blue;
    half.setOpacity(qreal(0.5));
    QVERIFY(qAbs(CspColorMixer(params, true).paintColor(half, red, 1.0, 0.5).opacityF() - 0.25) < 0.01);

    // Blend carries the color it started from
    params.mixingType = CspBrushParams::MixBlend;
    params.colorStretch = 0.9;
    CspColorMixer mixer(params, true);
    mixer.paintColor(red, blue, 1.0, 0.5);
    QVERIFY(mixer.paintColor(blue, blue, 1.0, 0.5, 0.1).toQColor().red() > 200);
}

void CspBrushEngineTest::testColorMixerCarriedReachesTarget()
{
    // a long color stretch moves the carried color little per dab, but over many dabs it must still
    // arrive: a step smaller than one 8-bit level must not be lost
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor red(Qt::red, cs);
    const KoColor blue(Qt::blue, cs);
    const KoColor green(Qt::green, cs);

    QStringList stalled;
    for (CspColorMath::Mode mode : {CspColorMath::Mode::Standard, CspColorMath::Mode::Perceptual}) {
        for (int type : {CspBrushParams::MixBlend, CspBrushParams::MixSmear}) {
            for (qreal stretch : {0.5, 0.9, 1.0}) {
                CspBrushParams params;
                params.mixingMode = mode;
                params.mixingType = type;
                params.colorStretch = stretch;
                CspColorMixer mixer(params);
                QColor q;
                mixer.paintColor(green, red, 0.0, 1.0);
                // 60 brush diameters in steps of 1% of the diameter: over 12 half-distances
                for (int i = 0; i < 6000; i++) mixer.paintColor(blue, red, 0.0, 1.0, 0.01).toQColor(&q);
                if (q.green() > 8 || q.blue() < 247) {
                    stalled << QString("mode %1, type %2, stretch %3: stops at %4 %5 %6")
                        .arg(int(mode)).arg(type).arg(stretch).arg(q.red()).arg(q.green()).arg(q.blue());
                }
            }
        }
    }
    for (const QString &line : stalled) qDebug().noquote() << line;
    QVERIFY2(stalled.isEmpty(), stalled.join("; ").toLatin1().constData());
}

static KisFixedPaintDeviceSP alpha8Mask(const QRect &rect, const QVector<quint8> &values)
{
    KisFixedPaintDeviceSP mask = new KisFixedPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    mask->setRect(rect);
    mask->lazyGrowBufferWithoutInitialization();
    memcpy(mask->data(), values.constData(), values.size());
    return mask;
}

void CspBrushEngineTest::testColorMixerLayDown()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(QRect(0, 0, 4, 1), KoColor(Qt::blue, cs));

    // full, half and no mask; transparent red paint pulls the alpha down
    KoColor paint(Qt::red, cs);
    paint.setOpacity(quint8(0));
    KisPainter gc(device);
    CspColorMixer::layDown(&gc, QRect(0, 0, 3, 1), alpha8Mask(QRect(0, 0, 3, 1), {255, 128, 0}), paint, 1.0);

    KoColor c(cs);
    device->pixel(0, 0, &c);
    QCOMPARE(c.opacityU8(), quint8(0));
    device->pixel(1, 0, &c);
    QVERIFY(qAbs(int(c.opacityU8()) - 127) <= 2);
    device->pixel(2, 0, &c);
    QCOMPARE(c.opacityU8(), quint8(255));

    QVERIFY(CspColorMixer::sampleCanvas(device, QRect(2, 0, 2, 1), QVector<qint16>()).opacityU8() == 255);
}

void CspBrushEngineTest::testSampleCanvasWeightedAlpha()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    KoColor faint(Qt::red, cs);
    faint.setOpacity(quint8(50));
    device->fill(QRect(0, 0, 4, 4), faint);

    // the alpha is the canvas', whatever the size of the dab
    const QVector<qint16> full(16, 255);
    QVERIFY(qAbs(int(CspColorMixer::sampleCanvas(device, QRect(0, 0, 4, 4), full).opacityU8()) - 50) <= 1);

    // and only the weighted pixels count
    QVector<qint16> half(16, 0);
    faint.setOpacity(quint8(200));
    device->fill(QRect(0, 0, 2, 4), faint);
    for (int y = 0; y < 4; y++) half[y * 4] = 128;
    QVERIFY(qAbs(int(CspColorMixer::sampleCanvas(device, QRect(0, 0, 4, 4), half).opacityU8()) - 200) <= 1);
}

void CspBrushEngineTest::testSampleCanvasSparse()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const QRect all(0, 0, 300, 300);

    KisPaintDeviceSP noise = new KisPaintDevice(cs);
    QByteArray bytes(all.width() * all.height() * 4, 0);
    QRandomGenerator rng(7);
    for (int i = 0; i < bytes.size(); i += 4) {
        for (int c = 0; c < 3; c++) bytes[i + c] = char(rng.bounded(256));
        bytes[i + 3] = char(255);
    }
    noise->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()), all);
    KisPaintDeviceSP flat = new KisPaintDevice(cs);
    flat->fill(all, KoColor(QColor(90, 140, 200), cs));

    QVector<qint16> ramp(all.width() * all.height());
    for (int i = 0; i < ramp.size(); i++) ramp[i] = qint16(1 + (i % 300 + i / 300) % 255);

    using CspColorMath::Mode;
    auto maxDiff = [](const KoColor &a, const KoColor &b) {
        int d = 0;
        for (int i = 0; i < 4; i++) d = qMax(d, qAbs(int(a.data()[i]) - int(b.data()[i])));
        return d;
    };

    for (Mode mode : {Mode::Standard, Mode::Perceptual}) {
        if (mode == Mode::Perceptual && !CspColorMath::hasSpectral()) continue;
        for (bool weighted : {false, true}) {
            const QVector<qint16> weights = weighted ? ramp : QVector<qint16>();
            const KoColor denseNoise = CspColorMixer::sampleCanvas(noise, all, weights, mode, 0);
            const KoColor sparseNoise = CspColorMixer::sampleCanvas(noise, all, weights, mode, CspColorMixer::MaxSamples);
            QVERIFY2(maxDiff(denseNoise, sparseNoise) <= 3, "noise canvas");
            const KoColor denseFlat = CspColorMixer::sampleCanvas(flat, all, weights, mode, 0);
            const KoColor sparseFlat = CspColorMixer::sampleCanvas(flat, all, weights, mode, CspColorMixer::MaxSamples);
            QVERIFY2(maxDiff(denseFlat, sparseFlat) <= 1, "flat canvas");

            // 64 x 64 pixels and below: untouched
            const QRect small(10, 10, 64, 64);
            QVector<qint16> smallWeights;
            if (weighted) {
                smallWeights.resize(small.width() * small.height());
                for (int i = 0; i < smallWeights.size(); i++) smallWeights[i] = ramp[i];
            }
            const KoColor a = CspColorMixer::sampleCanvas(noise, small, smallWeights, mode, 0);
            const KoColor b = CspColorMixer::sampleCanvas(noise, small, smallWeights, mode);
            QCOMPARE(maxDiff(a, b), 0);
        }
    }

    // the mask overload gives the same as the weights it stands for
    QByteArray mask(all.width() * all.height(), 0);
    QVector<qint16> maskWeights(mask.size());
    for (int i = 0; i < mask.size(); i++) {
        mask[i] = char(i % 251);
        maskWeights[i] = quint8(mask[i]);
    }
    QCOMPARE(maxDiff(CspColorMixer::sampleCanvas(noise, all, reinterpret_cast<const quint8 *>(mask.constData())),
                     CspColorMixer::sampleCanvas(noise, all, maskWeights)), 0);
}

static QColor pixelAt(KisPaintDeviceSP device, int x, int y)
{
    KoColor c(device->colorSpace());
    device->pixel(x, y, &c);
    return c.toQColor();
}

void CspBrushEngineTest::testColorMixerPerceptual()
{
    if (!CspColorMath::hasSpectral()) QSKIP("built without libmypaint");
    using CspColorMath::Mode;
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    // the canvas average is the spectral mix of the pixels
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(QRect(0, 0, 1, 1), KoColor(Qt::blue, cs));
    device->fill(QRect(1, 0, 1, 1), KoColor(Qt::yellow, cs));
    QColor sample = CspColorMixer::sampleCanvas(device, QRect(0, 0, 2, 1), QVector<qint16>(), Mode::Perceptual).toQColor();
    const CspColorMath::Rgb expected = CspColorMath::mix(CspColorMath::Rgb{0, 0, 1}, CspColorMath::Rgb{1, 1, 0}, 0.5, Mode::Perceptual);
    QVERIFY2(qAbs(sample.greenF() - expected[1]) < 0.01,
             qPrintable(QString("%1 %2 %3 %4 vs %5 %6 %7").arg(sample.redF()).arg(sample.greenF()).arg(sample.blueF())
                        .arg(sample.alphaF()).arg(expected[0]).arg(expected[1]).arg(expected[2])));
    QVERIFY(qAbs(sample.blueF() - expected[2]) < 0.01);

    // lay-down: each pixel mixes with its own canvas color
    device->fill(QRect(0, 0, 2, 1), KoColor(Qt::yellow, cs));
    {
        KisPainter gc(device);
        CspColorMixer::layDown(&gc, QRect(0, 0, 2, 1), alpha8Mask(QRect(0, 0, 2, 1), {128, 0}), KoColor(Qt::blue, cs), 1.0,
                               Mode::Perceptual);
    }
    const QColor mixed = pixelAt(device, 0, 0);
    QVERIFY(mixed.greenF() > mixed.redF() + 0.3 && mixed.greenF() > mixed.blueF());
    QCOMPARE(pixelAt(device, 1, 0), QColor(Qt::yellow));

    // Standard over the same canvas gives gray
    device->fill(QRect(0, 0, 2, 1), KoColor(Qt::yellow, cs));
    {
        KisPainter gc(device);
        CspColorMixer::layDown(&gc, QRect(0, 0, 2, 1), alpha8Mask(QRect(0, 0, 2, 1), {128, 0}), KoColor(Qt::blue, cs), 1.0,
                               Mode::Standard);
    }
    const QColor gray = pixelAt(device, 0, 0);
    QVERIFY(qAbs(gray.redF() - gray.blueF()) < 0.02);

    // mirrored: the copy at x = 3 mixes with the red under it, with the mirrored mask
    device->fill(QRect(0, 0, 2, 1), KoColor(Qt::yellow, cs));
    device->fill(QRect(2, 0, 2, 1), KoColor(Qt::red, cs));
    {
        KisPainter gc(device);
        gc.setMirrorInformation(QPointF(2.0, 0.5), true, false);
        CspColorMixer::layDown(&gc, QRect(0, 0, 2, 1), alpha8Mask(QRect(0, 0, 2, 1), {128, 0}), KoColor(Qt::blue, cs), 1.0,
                               Mode::Perceptual);
    }
    QCOMPARE(pixelAt(device, 1, 0), QColor(Qt::yellow));
    QCOMPARE(pixelAt(device, 2, 0), QColor(Qt::red));
    const QColor mirrored = pixelAt(device, 3, 0);
    QVERIFY(mirrored.blueF() > 0.2 && mirrored.redF() > 0.2 && mirrored.greenF() < 0.3); // purple, not green
    QVERIFY(pixelAt(device, 0, 0).greenF() > 0.3);
}

void CspBrushEngineTest::testWaterEdgeWithMixing()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP layer = new KisPaintDevice(cs);
    layer->fill(QRect(0, 0, 24, 24), KoColor(Qt::white, cs));

    CspBrushParams params;
    params.colorMixing = true;
    params.waterEdge = true;
    params.waterEdgeRadius = 2.0;
    params.waterEdgeAlphaPower = 1.0; // full Opacity effect: only the rim shows the paint
    params.waterEdgeValuePower = 0.0;

    KisPainter gc(layer);
    CspMixedStrokeBuffer buffer(params, &gc);
    const QRect rect(4, 4, 14, 14);
    QVector<quint8> full(rect.width() * rect.height(), 255);
    KisFixedPaintDeviceSP mask = alpha8Mask(rect, full);
    buffer.addCoverage(rect, mask, 1.0);
    CspColorMixer::layDown(buffer.painter(), rect, alpha8Mask(rect, full), KoColor(Qt::red, cs), 1.0);
    buffer.flush(rect);

    // the mixed paint is in the working copy, the layer gets the processed stroke
    QCOMPARE(pixelAt(buffer.device(), 11, 11), QColor(Qt::red));
    QCOMPARE(pixelAt(layer, 11, 11), QColor(Qt::white)); // inside: the original
    const QColor rim = pixelAt(layer, 4, 11);
    QVERIFY(rim.red() == 255 && rim.green() < 128);       // rim: the paint
    QCOMPARE(pixelAt(layer, 1, 1), QColor(Qt::white));   // outside: untouched

    // flushing again doesn't compound
    buffer.flush(rect);
    QCOMPARE(pixelAt(layer, 4, 11), rim);
}

void CspBrushEngineTest::testColorMixerRespectsSelection()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(QRect(0, 0, 4, 1), KoColor(Qt::blue, cs));

    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 2, 1));

    for (CspColorMath::Mode mode : {CspColorMath::Mode::Standard, CspColorMath::Mode::Perceptual}) {
        device->fill(QRect(0, 0, 4, 1), KoColor(Qt::blue, cs));
        KisPainter gc(device, selection);
        CspColorMixer::layDown(&gc, QRect(0, 0, 4, 1), alpha8Mask(QRect(0, 0, 4, 1), {255, 255, 255, 255}),
                               KoColor(Qt::red, cs), 1.0, mode);

        QCOMPARE(pixelAt(device, 0, 0), QColor(Qt::red));
        for (int x = 2; x < 4; x++) QCOMPARE(pixelAt(device, x, 0), QColor(Qt::blue));
    }
}

// Colors painted by a gray two-color tip (left half black = main, right half white = sub), main red, sub blue
static QPair<int, int> paintTwoColorTip(bool ribbon, bool mixToSub)
{
    QImage tip(24, 24, QImage::Format_ARGB32);
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) tip.setPixel(x, y, x < 12 ? qRgba(0, 0, 0, 255) : qRgba(255, 255, 255, 255));
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    tip.save(&buffer, "PNG");

    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisResourcesInterfaceSP(new KisLocalStrokeResources()));
    settings->setProperty("paintop", "cspbrush");
    settings->setProperty("CSP/BrushSize", 24);
    settings->setProperty("CSP/BrushUsePatternImage", 1);
    settings->setProperty("CSP/Tips/Count", 1);
    settings->setProperty("CSP/Tips/0/Name", "two color");
    settings->setProperty("CSP/Tips/0/Png", QString::fromLatin1(png.toBase64()));
    settings->setProperty("CSP/BrushSizeEffector", "on=");
    settings->setProperty("CSP/BrushColorMixingMode", 0);
    settings->setProperty("CSP/BrushRibbon", ribbon ? 1 : 0);
    if (mixToSub) {
        // Mix: always all of the sub color (no effect source)
        settings->setProperty("CSP/BrushChangePatternColor", 1);
        settings->setProperty("CSP/BrushSubColor", 100);
        settings->setProperty("CSP/BrushSubColorEffector", "on=");
    }

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    KisPainter gc(device);
    gc.setPaintColor(KoColor(Qt::red, cs));
    gc.setBackgroundColor(KoColor(Qt::blue, cs));
    gc.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
    {
        KisCspBrushPaintOp cspOp(settings, &gc, nullptr, nullptr);
        KisPaintOp &op = cspOp;
        KisDistanceInformation dist;
        KisPaintInformation p1(QPointF(40, 40), 1.0);
        KisPaintInformation p2(QPointF(140, 40), 1.0);
        {
            KisPaintInformation::DistanceInformationRegistrar r = p1.registerDistanceInformation(&dist);
            op.paintAt(p1, &dist);
        }
        op.paintLine(p1, p2, &dist);
    }

    int reds = 0;
    int blues = 0;
    const QImage image = device->convertToQImage(nullptr, QRect(0, 0, 200, 80));
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            const QColor c = QColor::fromRgba(image.pixel(x, y));
            if (c.alpha() < 200) continue;
            if (c.red() > 200 && c.blue() < 60) reds++;
            if (c.blue() > 200 && c.red() < 60) blues++;
        }
    }
    return qMakePair(reds, blues);
}

void CspBrushEngineTest::testTwoColorTipMix()
{
    for (bool ribbon : {false, true}) {
        // without jitter: the black half paints the main color, the white half the sub color
        const QPair<int, int> plain = paintTwoColorTip(ribbon, false);
        QVERIFY2(plain.first > 50 && plain.second > 50, qPrintable(QString("ribbon %1: %2 red, %3 blue")
                                                               .arg(ribbon).arg(plain.first).arg(plain.second)));
        // Mix all the way to the sub color: the main part turns to the sub color too
        const QPair<int, int> mixed = paintTwoColorTip(ribbon, true);
        QVERIFY2(mixed.first == 0 && mixed.second > plain.second, qPrintable(QString("ribbon %1: %2 red, %3 blue")
                                                                         .arg(ribbon).arg(mixed.first).arg(mixed.second)));
    }
}

void CspBrushEngineTest::testSprayDirection()
{
    CspBrushParams p;
    p.sprayAngle = 10.0;
    p.angle = 30.0;
    const qreal drawing = 0.5;
    const qreal polar = 1.2;
    const qreal a = kisDegreesToRadians(10.0);
    auto rotation = [&](int bits, qreal randomSample = 0.5) {
        p.sprayDirection = bits;
        return KisCspBrushPaintOp::sprayParticleRotation(p, drawing, polar, randomSample);
    };

    // one direction each
    QCOMPARE(rotation(0), a);
    QCOMPARE(rotation(CspBrushParams::SprayLineDirection), a - drawing);
    QCOMPARE(rotation(CspBrushParams::SprayWhole), a + kisDegreesToRadians(30.0));
    QCOMPARE(rotation(CspBrushParams::SprayTowardCenter), a - polar);
    // an old preset with several: line direction wins, not their sum
    QCOMPARE(rotation(CspBrushParams::SprayLineDirection | CspBrushParams::SprayTowardCenter), a - drawing);

    // random adds to any of them; a sample of 0.5 adds nothing
    p.sprayRandom = 1.0;
    for (int bits : {0, int(CspBrushParams::SprayLineDirection), int(CspBrushParams::SprayWhole),
                     int(CspBrushParams::SprayTowardCenter)}) {
        const qreal base = rotation(bits);
        QCOMPARE(rotation(bits | CspBrushParams::SprayRandom, 0.5), base);
        QVERIFY(qAbs(rotation(bits | CspBrushParams::SprayRandom, 1.0) - (base + M_PI)) < 1e-9);
        QVERIFY(qAbs(rotation(bits | CspBrushParams::SprayRandom, 0.0) - (base - M_PI)) < 1e-9);
    }
}

void CspBrushEngineTest::testTipImport()
{
    // a Krita mask (opaque gray, white = no paint): black where it paints, transparent where it doesn't
    QImage mask(2, 1, QImage::Format_Grayscale8);
    mask.setPixel(0, 0, qRgb(0, 0, 0));
    mask.setPixel(1, 0, qRgb(255, 255, 255));
    const QImage tip = CspTipImport::maskToTip(mask);
    QCOMPARE(tip.pixel(0, 0), qRgba(0, 0, 0, 255));
    QCOMPARE(qAlpha(tip.pixel(1, 0)), 0);

    // an animated brush: every frame becomes a tip, in order
    KisBrushSP pipe(new KisImagePipeBrush(QString(FILES_DATA_DIR) + "/three_frames.gih"));
    QVERIFY(pipe->load(KisGlobalResourcesInterface::instance()));
    const QVector<CspTipImport::Tip> frames = CspTipImport::fromBrush(pipe);
    QCOMPARE(frames.size(), 3);
    for (int i = 0; i < 3; i++) {
        const QImage &f = frames[i].image;
        QCOMPARE(f.format(), QImage::Format_ARGB32);
        QCOMPARE(qAlpha(f.pixel(0, 0)), 255);                 // the square paints
        QCOMPARE(qAlpha(f.pixel(7, 7)), 0);                   // the background doesn't
        QCOMPARE(qAlpha(f.pixel(2 + 2 * i, 0)), 0);           // the square grows frame by frame
        QCOMPARE(qAlpha(f.pixel(1 + 2 * i, 0)), 255);
    }

    // an image tip with transparency: as it is (a two-color CSP tip keeps its white part)
    QImage twoColor(2, 2, QImage::Format_ARGB32);
    twoColor.fill(Qt::transparent);
    twoColor.setPixel(0, 0, qRgba(0, 0, 0, 255));
    twoColor.setPixel(1, 0, qRgba(255, 255, 255, 255));
    const QVector<CspTipImport::Tip> image = CspTipImport::fromBrush(KisBrushSP(new KisGbrBrush(twoColor, "two")));
    QCOMPARE(image.size(), 1);
    QCOMPARE(image[0].image.pixel(1, 0), qRgba(255, 255, 255, 255));

    // an opaque two-color tip written by sut2kpp: Krita reads it as a mask, the marker keeps it as it is
    QImage opaque(2, 1, QImage::Format_ARGB32);
    opaque.setPixel(0, 0, qRgba(0, 0, 0, 255));
    opaque.setPixel(1, 0, qRgba(255, 255, 255, 255));
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "PNG");
        writer.setText("csp_tip", "1");
        QVERIFY(writer.write(opaque));
    }
    QVERIFY(CspTipImport::isCspTipPng(png));
    KisBrushSP asMask(new KisGbrBrush(QImage(opaque).convertToFormat(QImage::Format_Grayscale8), "opaque"));
    QCOMPARE(qAlpha(CspTipImport::fromBrush(asMask)[0].image.pixel(1, 0)), 0); // without the file: a mask
    QCOMPARE(CspTipImport::fromBrush(asMask, png)[0].image.pixel(1, 0), qRgba(255, 255, 255, 255));

    // the marker as sut2kpp writes it (iTXt)
    QFile converted(QString(FILES_DATA_DIR) + "/csp_tip.png");
    QVERIFY(converted.open(QIODevice::ReadOnly));
    const QByteArray convertedPng = converted.readAll();
    QVERIFY(CspTipImport::isCspTipPng(convertedPng));
    QCOMPARE(CspTipImport::fromBrush(asMask, convertedPng)[0].image.pixel(3, 0), qRgba(255, 255, 255, 255));
}

void CspBrushEngineTest::testTextureInvertsForEraser()
{
    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisResourcesInterfaceSP(new KisLocalStrokeResources()));
    const KisPropertiesConfigurationSP texture = textureSettings(grayPattern(7, 5));
    for (const QString &key : texture->getPropertiesKeys()) settings->setProperty(key, texture->getProperty(key));

    QSharedPointer<KoLocalStrokeCanvasResources> canvas(new KoLocalStrokeCanvasResources());
    canvas->storeResource(KoCanvasResource::CurrentEffectiveCompositeOp, QString(COMPOSITE_ERASE));
    settings->setCanvasResourcesInterface(canvas);

    // off: the eraser changes nothing, and the canvas' composite op isn't needed
    QVERIFY(!CspTextureStage::invertsForEraser(settings.data()));
    QVERIFY(!settings->requiredCanvasResources().contains(KoCanvasResource::CurrentEffectiveCompositeOp));

    settings->setProperty("Texture/Pattern/AutoInvertOnErase", true);
    QVERIFY(settings->requiredCanvasResources().contains(KoCanvasResource::CurrentEffectiveCompositeOp));
    QVERIFY(CspTextureStage::invertsForEraser(settings.data()));
    canvas->storeResource(KoCanvasResource::CurrentEffectiveCompositeOp, QString(COMPOSITE_OVER));
    QVERIFY(!CspTextureStage::invertsForEraser(settings.data())); // painting, not erasing

    // what the paintop does with it: the texture's density reversed
    QImage dark(1, 1, QImage::Format_ARGB32);
    dark.setPixel(0, 0, qRgba(0, 0, 0, 255));
    CspBrushParams params;
    QCOMPARE(CspTextureStage(params, dark).strengthAt(0, 0), 1.0);
    params.textureInvert = true;
    QCOMPARE(CspTextureStage(params, dark).strengthAt(0, 0), 0.0);
}

SIMPLE_TEST_MAIN(CspBrushEngineTest)
