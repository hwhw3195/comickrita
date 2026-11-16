/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushStrokeTest.h"

#include <QDir>
#include <QDomDocument>
#include <QImageReader>
#include <QTest>

#include <kistest.h>

#include <KisGlobalResourcesInterface.h>
#include <KisViewManager.h>
#include <KoCanvasResourceProvider.h>
#include <KoCanvasResourcesIds.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_simple_paintop_factory.h>
#include <kis_undo_stores.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>
#include <KisAsynchronousStrokeUpdateHelper.h>

#include "CspBrushParams.h"
#include "KisCspBrushPaintOp.h"
#include "KisCspBrushSettings.h"
#include "KisCspBrushSettingsWidget.h"

namespace {
const QRect canvasRect(0, 0, 120, 60);
const QColor gray(128, 128, 128);

KisPaintOpPresetSP loadPreset(const QString &file)
{
    QImageReader reader(QString(FILES_DATA_DIR) + "/" + file, "PNG");
    QDomDocument doc;
    if (!doc.setContent(reader.text("preset"))) return KisPaintOpPresetSP();

    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    const QDomElement root = doc.documentElement();
    for (QDomElement p = root.firstChildElement("param"); !p.isNull(); p = p.nextSiblingElement("param")) {
        settings->setProperty(p.attribute("name"), p.text());
    }
    settings->setProperty("paintop", "cspbrush");
    settings->setProperty("CSP/BrushSize", 20.0);
    settings->setProperty("CSP/BrushSizeUnit", 0);
    settings->setProperty("CSP/Opacity", 100);

    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setSettings(settings);
    preset->setName(file);
    return preset;
}

KisPaintOpPresetSP mixingPreset(int colorMode, bool edge, qreal density = 100)
{
    KisPaintOpPresetSP preset = loadPreset("mix_marker.kpp");
    KisPaintOpSettingsSP s = preset->settings();
    s->setProperty("CSP/BrushUseWaterColor", 1);
    s->setProperty("CSP/BrushWaterColor", int(CspBrushParams::MixBlend));
    s->setProperty("CSP/BrushColorMixingMode", colorMode);
    s->setProperty("CSP/BrushMixColor", 100);
    s->setProperty("CSP/BrushMixAlpha", density);
    s->setProperty("CSP/BrushUseWaterEdge", edge ? 1 : 0);
    s->setProperty("CSP/BrushWaterEdgeRadius", 2.0);
    s->setProperty("CSP/BrushWaterEdgeRadiusUnit", 0);
    s->setProperty("CSP/BrushWaterEdgeAlphaPower", 0);
    s->setProperty("CSP/BrushWaterEdgeValuePower", 0);
    return preset;
}

struct Stroke {
    KisPaintOpPresetSP preset;
    QString compositeOp {COMPOSITE_OVER};
    bool eraser {false};
    QColor brush {gray};
    bool alphaLock {false};
    KisSelectionSP selection;
    QPointF from {10, 30};
    QPointF to {110, 30};
};

/// Paints the stroke on a layer holding @p canvas (a copy), the way the freehand tool does
KisPaintDeviceSP paint(const Stroke &stroke, KisPaintDeviceSP canvas, const QSize &imageSize = canvasRect.size())
{
    const KoColorSpace *cs = canvas->colorSpace();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), imageSize.width(), imageSize.height(), cs, "stroke");
    KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8, new KisPaintDevice(*canvas));
    layer->setAlphaLocked(stroke.alphaLock);
    image->addNode(layer);

    stroke.preset->settings()->setPaintOpCompositeOp(stroke.compositeOp);

    KoCanvasResourceProvider manager;
    KisViewManager::initializeResourceManager(&manager);
    QVariant v;
    v.setValue(KoColor(stroke.brush, cs));
    manager.setResource(KoCanvasResource::ForegroundColor, v);
    v.setValue(KoColor(Qt::white, cs));
    manager.setResource(KoCanvasResource::BackgroundColor, v);
    v.setValue(KisNodeSP(layer));
    manager.setResource(KoCanvasResource::CurrentKritaNode, v);
    v.setValue(stroke.preset);
    manager.setResource(KoCanvasResource::CurrentPaintOpPreset, v);
    manager.setResource(KoCanvasResource::EraserMode, stroke.eraser);
    manager.setResource(KoCanvasResource::Opacity, 1.0);
    manager.setResource(KoCanvasResource::HdrExposure, 0.0);

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &manager);
    if (stroke.selection) resources->setSelectionOverride(stroke.selection);

    KisStrokeId id = image->startStroke(
        new FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(), kundo2_noi18n("stroke")));
    image->addJob(id, new FreehandStrokeStrategy::Data(0, KisPaintInformation(stroke.from, 1.0),
                                                       KisPaintInformation(stroke.to, 1.0)));
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();
    return layer->paintDevice();
}

KisPaintDeviceSP filled(const QColor &color)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(canvasRect, KoColor(color, cs));
    return device;
}

QColor pixelAt(KisPaintDeviceSP device, int x, int y)
{
    KoColor c(device->colorSpace());
    device->pixel(x, y, &c);
    return c.toQColor();
}

QString show(const QColor &c)
{
    return QString("%1,%2,%3 a%4").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

bool near(const QColor &c, int value, int tolerance)
{
    return qAbs(c.red() - value) <= tolerance && qAbs(c.green() - value) <= tolerance
        && qAbs(c.blue() - value) <= tolerance;
}

const int multiplied = 64; // gray 128 on gray 128
const int screened = 192;
}

void CspBrushStrokeTest::initTestCase()
{
    // the code being built, not a plugin the loader may have found among the installed ones
    KisPaintOpRegistry *r = KisPaintOpRegistry::instance();
    if (r->value("cspbrush")) r->remove("cspbrush");
    r->add(new KisSimplePaintOpFactory<KisCspBrushPaintOp, KisCspBrushSettings, KisCspBrushSettingsWidget>(
        "cspbrush", "CSP Brush", KisPaintOpFactory::categoryStable(), "krita-paintbrush.png", QString(), QStringList(), 6));
}

void CspBrushStrokeTest::testBlendWithoutMixing_data()
{
    QTest::addColumn<bool>("edge");
    QTest::newRow("plain") << false;
    QTest::newRow("edge") << true;
}

void CspBrushStrokeTest::testBlendWithoutMixing()
{
    QFETCH(bool, edge);
    auto preset = [&] {
        KisPaintOpPresetSP p = mixingPreset(0, edge);
        p->settings()->setProperty("CSP/BrushUseWaterColor", 0);
        return p;
    };

    Stroke multiply;
    multiply.preset = preset();
    multiply.compositeOp = COMPOSITE_MULT;
    QColor c = pixelAt(paint(multiply, filled(gray)), 60, 30);
    QVERIFY2(near(c, multiplied, 4), qPrintable(show(c)));

    Stroke erase;
    erase.preset = preset();
    erase.eraser = true;
    c = pixelAt(paint(erase, filled(gray)), 60, 30);
    QVERIFY2(c.alpha() <= 5, qPrintable(show(c)));
}

void CspBrushStrokeTest::testEraseWithMixing_data()
{
    QTest::addColumn<int>("colorMode");
    QTest::addColumn<bool>("edge");
    QTest::newRow("standard") << 0 << false;
    QTest::newRow("perceptual") << 1 << false;
    QTest::newRow("standard edge") << 0 << true;
    QTest::newRow("perceptual edge") << 1 << true;
}

void CspBrushStrokeTest::testEraseWithMixing()
{
    QFETCH(int, colorMode);
    QFETCH(bool, edge);

    Stroke stroke;
    stroke.preset = mixingPreset(colorMode, edge);
    stroke.eraser = true;
    KisPaintDeviceSP result = paint(stroke, filled(gray));

    QColor c = pixelAt(result, 60, 30);
    QVERIFY2(c.alpha() <= 5, qPrintable(show(c)));
    c = pixelAt(result, 60, 2); // away from the stroke
    QVERIFY2(c.alpha() == 255 && near(c, gray.red(), 1), qPrintable(show(c)));
}

void CspBrushStrokeTest::testEraseCarriesColor()
{
    // a red half, then a blue one: a low density erases slowly, and the brush carries red into the blue
    KisPaintDeviceSP canvas = filled(Qt::blue);
    canvas->fill(QRect(0, 0, 60, 60), KoColor(Qt::red, canvas->colorSpace()));

    Stroke stroke;
    stroke.preset = mixingPreset(0, false, 10);
    stroke.preset->settings()->setProperty("CSP/BrushMixColorExtension", 90);
    stroke.eraser = true;
    KisPaintDeviceSP result = paint(stroke, canvas);

    const QColor c = pixelAt(result, 75, 30);
    QVERIFY2(c.alpha() < 250 && c.alpha() > 0, qPrintable(show(c)));
    QVERIFY2(c.red() > 40, qPrintable(show(c)));
}

void CspBrushStrokeTest::testStandardKeepsFaintAlpha_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<bool>("eraser");
    QTest::addColumn<qreal>("density");
    QTest::newRow("erase blend 50") << int(CspBrushParams::MixBlend) << true << 50.0;
    QTest::newRow("erase blend 0") << int(CspBrushParams::MixBlend) << true << 0.0;
    QTest::newRow("erase smear 50") << int(CspBrushParams::MixSmear) << true << 50.0;
    QTest::newRow("erase running 50") << int(CspBrushParams::MixRunningColor) << true << 50.0;
    QTest::newRow("paint blend 0") << int(CspBrushParams::MixBlend) << false << 0.0;
    QTest::newRow("paint smear 0") << int(CspBrushParams::MixSmear) << false << 0.0;
    QTest::newRow("paint running 0") << int(CspBrushParams::MixRunningColor) << false << 0.0;
}

void CspBrushStrokeTest::testStandardKeepsFaintAlpha()
{
    QFETCH(int, type);
    QFETCH(bool, eraser);
    QFETCH(qreal, density);

    // the two color modes mix colors differently, but agree on alpha
    KisPaintDeviceSP canvas = filled(gray);
    KoColor faint(gray, canvas->colorSpace());
    faint.setOpacity(quint8(50));
    canvas->fill(canvasRect, faint);

    int alpha[2];
    for (int colorMode : {0, 1}) {
        Stroke stroke;
        stroke.preset = mixingPreset(colorMode, false, density);
        stroke.preset->settings()->setProperty("CSP/BrushWaterColor", type);
        stroke.preset->settings()->setProperty("CSP/BrushBlur", 0);
        stroke.preset->settings()->setProperty("CSP/BrushBlurLinkSize", 0);
        stroke.eraser = eraser;
        alpha[colorMode] = pixelAt(paint(stroke, new KisPaintDevice(*canvas)), 60, 30).alpha();
    }
    QVERIFY2(qAbs(alpha[0] - alpha[1]) <= 2, qPrintable(QString("standard %1, perceptual %2").arg(alpha[0]).arg(alpha[1])));
    // Smear starts from the opaque brush color, and has not lost it all yet
    if (eraser || type != CspBrushParams::MixSmear) QVERIFY2(alpha[0] <= 52, qPrintable(QString("alpha %1 on a canvas of 50").arg(alpha[0])));
}

void CspBrushStrokeTest::testBlendWithMixing_data()
{
    QTest::addColumn<QString>("op");
    QTest::addColumn<int>("expected");
    QTest::addColumn<int>("colorMode");
    QTest::addColumn<bool>("edge");
    QTest::newRow("multiply standard") << QString(COMPOSITE_MULT) << multiplied << 0 << false;
    QTest::newRow("multiply perceptual") << QString(COMPOSITE_MULT) << multiplied << 1 << false;
    QTest::newRow("multiply edge") << QString(COMPOSITE_MULT) << multiplied << 0 << true;
    QTest::newRow("screen standard") << QString(COMPOSITE_SCREEN) << screened << 0 << false;
}

void CspBrushStrokeTest::testBlendWithMixing()
{
    QFETCH(QString, op);
    QFETCH(int, expected);
    QFETCH(int, colorMode);
    QFETCH(bool, edge);

    Stroke stroke;
    stroke.preset = mixingPreset(colorMode, edge);
    stroke.compositeOp = op;
    KisPaintDeviceSP result = paint(stroke, filled(gray));

    // dabs overlap all along the stroke: one result everywhere means they don't compound
    for (int x = 40; x <= 80; x += 10) {
        const QColor c = pixelAt(result, x, 30);
        QVERIFY2(near(c, expected, 5), qPrintable(QString("%1: %2").arg(x).arg(show(c))));
        QCOMPARE(c.alpha(), 255);
    }
    QCOMPARE(pixelAt(result, 60, 2), gray);
}

void CspBrushStrokeTest::testNormalMixingStillPaints()
{
    Stroke stroke;
    stroke.preset = mixingPreset(0, false);
    stroke.brush = Qt::red;
    const QColor c = pixelAt(paint(stroke, filled(gray)), 60, 30);
    QVERIFY2(c.red() > 240 && c.green() < 15, qPrintable(show(c)));
}

void CspBrushStrokeTest::testBlendWithMixingAlphaLock()
{
    QColor half = gray;
    half.setAlpha(128);

    Stroke stroke;
    stroke.preset = mixingPreset(0, false);
    stroke.compositeOp = COMPOSITE_MULT;
    stroke.alphaLock = true;
    const QColor c = pixelAt(paint(stroke, filled(half)), 60, 30);
    QVERIFY2(c.alpha() == 128 && c.red() < 118, qPrintable(show(c)));
}

void CspBrushStrokeTest::testBlendWithMixingSelection()
{
    KisSelectionSP selection = new KisSelection();
    selection->pixelSelection()->select(QRect(0, 0, 60, 60));

    Stroke stroke;
    stroke.preset = mixingPreset(0, false);
    stroke.compositeOp = COMPOSITE_MULT;
    stroke.selection = selection;
    KisPaintDeviceSP result = paint(stroke, filled(gray));

    const QColor inside = pixelAt(result, 40, 30);
    QVERIFY2(near(inside, multiplied, 5), qPrintable(show(inside)));
    QCOMPARE(pixelAt(result, 80, 30), gray);
}

void CspBrushStrokeTest::testRibbonIgnoresMixing()
{
    KisPaintOpPresetSP preset = loadPreset("ribbon_wave.kpp");
    preset->settings()->setProperty("CSP/BrushUseWaterColor", 1);
    QVERIFY(!preset->settings()->paintIncremental());

    Stroke stroke;
    stroke.preset = preset;
    stroke.compositeOp = COMPOSITE_MULT;
    KisPaintDeviceSP result = paint(stroke, filled(gray));

    // through the temporary layer, overlapping quads don't go below one multiplication
    int darkest = 255;
    for (int y = 0; y < 60; y++) {
        for (int x = 20; x < 100; x++) darkest = qMin(darkest, pixelAt(result, x, y).red());
    }
    QVERIFY2(darkest < 110, qPrintable(QString::number(darkest))); // it did paint
    QVERIFY2(darkest >= multiplied - 4, qPrintable(QString::number(darkest)));
}

namespace {
// The user's Clip Studio Paint strokes (vault: "2026-11-15 CSP mixing reference 2 (user).png"): a yellow
// 20 px stroke, no pressure, started on an opaque blue band and drawn out over an empty layer.
struct ReferencePoint {
    int offset;  ///< px from the band's edge along the stroke
    int share;   ///< how far the color is from yellow (0) towards blue (100)
    int opacity; ///< of the paint, in percent; -1: not measured
};
struct ReferenceStroke {
    const char *name; ///< mode, Color stretch / Amount of paint; Density of paint is 50
    int type;
    int stretch;
    int amount;
    int startToEdge; ///< px from the first dab's centre to the band's edge
    QVector<ReferencePoint> points;
};
const QColor referenceYellow(255, 222, 57);
const QColor referenceBlue(57, 152, 255);

// Amount of paint 0: blue on blue cannot be measured in the band; after it the color is the band's to the end
const QVector<ReferenceStroke> referenceStrokes = {
    {"blend 0/50", CspBrushParams::MixBlend, 0, 50, 84, {{-82, 54, -1}, {-64, 47, -1}, {-46, 46, -1}, {-28, 46, -1}, {6, 10, 82}, {24, 2, 82}, {42, 0, 82}, {60, 0, 82}, {78, 0, 82}, {96, 0, 82}, {114, 0, 83}}},
    {"blend 10/50", CspBrushParams::MixBlend, 10, 50, 82, {{-80, 57, -1}, {-62, 49, -1}, {-44, 47, -1}, {-26, 46, -1}, {6, 21, 84}, {24, 8, 82}, {42, 3, 83}, {60, 1, 82}, {78, 1, 82}, {96, 0, 82}, {114, 0, 83}}},
    {"blend 50/50", CspBrushParams::MixBlend, 50, 50, 26, {{-24, 62, -1}, {6, 44, 92}, {24, 32, 87}, {42, 24, 85}, {60, 18, 84}, {78, 13, 84}, {96, 9, 83}, {114, 7, 82}}},
    {"blend 95/50", CspBrushParams::MixBlend, 95, 50, 82, {{-80, 66, -1}, {-62, 61, -1}, {-44, 58, -1}, {-26, 55, -1}, {6, 46, 95}, {24, 39, 91}, {42, 33, 89}, {60, 27, 86}, {78, 23, 86}, {96, 19, 85}, {114, 16, 84}, {132, 12, 85}}},
    {"blend 100/50", CspBrushParams::MixBlend, 100, 50, 81, {{-79, 60, -1}, {-61, 59, -1}, {-43, 57, -1}, {-25, 54, -1}, {6, 46, 95}, {24, 39, 92}, {42, 33, 89}, {60, 28, 88}, {78, 24, 86}, {96, 20, 85}, {114, 17, 84}, {132, 14, 84}}},
    {"running 0/50", CspBrushParams::MixRunningColor, 0, 50, 79, {{-77, 8, -1}, {-59, 6, -1}, {-41, 6, -1}, {-23, 6, -1}, {6, 0, -1}, {24, 0, -1}, {42, 0, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}, {114, 0, -1}, {132, 0, -1}}},
    {"running 10/50", CspBrushParams::MixRunningColor, 10, 50, 77, {{-75, 10, -1}, {-57, 6, -1}, {-39, 6, -1}, {-21, 6, -1}, {6, 1, -1}, {24, 0, -1}, {42, 0, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}, {114, 0, -1}, {132, 0, -1}}},
    {"running 50/50", CspBrushParams::MixRunningColor, 50, 50, 20, {{-18, 15, -1}, {6, 2, -1}, {24, 0, -1}, {42, 0, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}}},
    {"running 95/50", CspBrushParams::MixRunningColor, 95, 50, 71, {{-69, 27, -1}, {-51, 21, -1}, {-33, 16, -1}, {-15, 13, -1}, {6, 10, -1}, {24, 8, -1}, {42, 7, -1}, {60, 6, -1}, {78, 5, -1}, {96, 4, -1}, {114, 3, -1}, {132, 2, -1}}},
    {"running 100/50", CspBrushParams::MixRunningColor, 100, 50, 72, {{-70, 30, -1}, {-52, 29, -1}, {-34, 30, -1}, {-16, 29, -1}, {6, 29, -1}, {24, 29, -1}, {42, 29, -1}, {60, 29, -1}, {78, 29, -1}, {96, 29, -1}, {114, 29, -1}, {132, 29, -1}}},
    {"smear 0/50", CspBrushParams::MixSmear, 0, 50, 71, {{-69, 56, -1}, {-51, 50, -1}, {-33, 50, -1}, {-15, 48, -1}, {6, 12, -1}, {24, 3, -1}, {42, 1, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}, {114, 0, -1}, {132, 0, -1}}},
    {"smear 10/50", CspBrushParams::MixSmear, 10, 50, 70, {{-68, 63, -1}, {-50, 54, -1}, {-32, 51, -1}, {-14, 49, -1}, {6, 26, -1}, {24, 13, -1}, {42, 7, -1}, {60, 4, -1}, {78, 2, -1}, {96, 1, -1}, {114, 1, -1}, {132, 0, -1}, {150, 0, -1}}},
    {"smear 50/50", CspBrushParams::MixSmear, 50, 50, 15, {{-13, 67, -1}, {6, 54, -1}, {24, 44, -1}, {42, 36, -1}, {60, 29, -1}, {78, 24, -1}, {96, 19, -1}, {114, 16, -1}}},
    {"smear 95/50", CspBrushParams::MixSmear, 95, 50, 68, {{-66, 70, -1}, {-48, 66, -1}, {-30, 63, -1}, {-12, 60, -1}, {6, 54, -1}, {24, 48, -1}, {42, 43, -1}, {60, 38, -1}, {78, 34, -1}, {96, 30, -1}, {114, 27, -1}, {132, 24, -1}}},
    {"smear 100/50", CspBrushParams::MixSmear, 100, 50, 61, {{-59, 70, -1}, {-41, 66, -1}, {-23, 64, -1}, {6, 55, -1}, {24, 49, -1}, {42, 44, -1}, {60, 39, -1}, {78, 35, -1}, {96, 32, -1}, {114, 28, -1}, {132, 25, -1}, {150, 22, -1}}},
    {"blend 50/0", CspBrushParams::MixBlend, 50, 0, 26, {{20, 100, 87}, {60, 100, 84}, {100, 100, 83}}},
    {"blend 50/100", CspBrushParams::MixBlend, 50, 100, 26, {{-24, 0, -1}, {6, 0, 92}, {24, 0, 88}, {42, 0, 85}, {60, 0, 84}, {78, 0, 84}, {96, 0, 83}, {114, 0, 83}}},
    {"running 50/0", CspBrushParams::MixRunningColor, 50, 0, 20, {{20, 100, -1}, {60, 100, -1}, {100, 100, -1}}},
    {"running 50/100", CspBrushParams::MixRunningColor, 50, 100, 23, {{-21, 0, -1}, {6, 0, -1}, {24, 0, -1}, {42, 0, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}}},
    {"smear 50/0", CspBrushParams::MixSmear, 50, 0, 15, {{20, 100, -1}, {60, 100, -1}, {100, 100, -1}}},
    {"smear 50/100", CspBrushParams::MixSmear, 50, 100, 18, {{-16, 0, -1}, {6, 0, -1}, {24, 0, -1}, {42, 0, -1}, {60, 0, -1}, {78, 0, -1}, {96, 0, -1}, {114, 0, -1}}},
};
}

void CspBrushStrokeTest::testMatchesCspReference_data()
{
    QTest::addColumn<int>("index");
    for (int i = 0; i < referenceStrokes.size(); i++) QTest::newRow(referenceStrokes[i].name) << i;
}

void CspBrushStrokeTest::testMatchesCspReference()
{
    QFETCH(int, index);
    const ReferenceStroke &ref = referenceStrokes[index];
    const QSize size(300, 60);
    const int edge = 110; // the band is x < edge
    const int tolerance = 6; // percentage points

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP canvas = new KisPaintDevice(cs);
    canvas->fill(QRect(0, 0, edge, size.height()), KoColor(referenceBlue, cs));

    Stroke stroke;
    stroke.preset = mixingPreset(0, false, 50);
    KisPaintOpSettingsSP s = stroke.preset->settings();
    s->setProperty("CSP/BrushWaterColor", ref.type);
    s->setProperty("CSP/BrushMixColor", ref.amount);
    s->setProperty("CSP/BrushMixColorExtension", ref.stretch);
    s->setProperty("CSP/BrushFlow", 100);
    s->setProperty("CSP/BrushUseIn", 0);
    s->setProperty("CSP/BrushUseOut", 0);
    stroke.brush = referenceYellow;
    stroke.from = QPointF(edge - ref.startToEdge, 30);
    stroke.to = QPointF(285, 30);
    KisPaintDeviceSP result = paint(stroke, canvas, size);

    const QString dir = QDir::tempPath() + "/cspbrush_renders/reference";
    QDir().mkpath(dir);
    result->convertToQImage(nullptr, QRect(QPoint(), size)).save(dir + QString("/%1.png").arg(index, 2, 10, QChar('0')));

    // the color between yellow and blue, and the paint's alpha, at the middle of the stroke
    const double d[3] = {double(referenceBlue.red() - referenceYellow.red()), double(referenceBlue.green() - referenceYellow.green()),
                         double(referenceBlue.blue() - referenceYellow.blue())};
    QStringList got;
    QStringList wrong;
    for (const ReferencePoint &point : ref.points) {
        double rgb[3] = {0, 0, 0};
        double alpha = 0;
        for (int y = 28; y <= 32; y++) {
            const QColor c = pixelAt(result, edge + point.offset, y);
            rgb[0] += c.red() * c.alphaF();
            rgb[1] += c.green() * c.alphaF();
            rgb[2] += c.blue() * c.alphaF();
            alpha += c.alphaF();
        }
        int share = 0;
        if (alpha > 0.01) {
            const double v[3] = {rgb[0] / alpha - referenceYellow.red(), rgb[1] / alpha - referenceYellow.green(),
                                 rgb[2] / alpha - referenceYellow.blue()};
            share = qRound(100.0 * (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
        const int opacity = qRound(100.0 * alpha / 5.0);
        got << QString("%1:%2/%3").arg(point.offset).arg(share).arg(opacity);
        if (qAbs(share - point.share) > tolerance && (opacity > 0 || point.share > 0)) {
            wrong << QString("at %1: %2% blue, CSP %3%").arg(point.offset).arg(share).arg(point.share);
        }
        if (point.opacity >= 0 && qAbs(opacity - point.opacity) > tolerance) {
            wrong << QString("at %1: opacity %2, CSP %3").arg(point.offset).arg(opacity).arg(point.opacity);
        }
        // Running color and Smear leave the band opaque
        if (point.offset > 12 && ref.type != CspBrushParams::MixBlend && opacity < 90) {
            wrong << QString("at %1: opacity %2, CSP 100").arg(point.offset).arg(opacity);
        }
    }
    qDebug().noquote() << ref.name << got.join(' ');
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
}

KISTEST_MAIN(CspBrushStrokeTest)
