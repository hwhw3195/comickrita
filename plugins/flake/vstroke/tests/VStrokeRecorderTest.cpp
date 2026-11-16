/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeRecorderTest.h"

#include <QTest>

#include <kistest.h>

#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <kis_paint_device.h>
#include <kis_stroke_strategy.h>
#include <strokes/freehand_stroke.h>

#include "VStrokeRecorderFacade.h"
#include "VStrokeShape.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

namespace {

VStrokeRenderSettings settingsFor(KisPaintOpPresetSP preset)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    VStrokeRenderSettings s;
    s.preset = preset;
    s.paintColor = KoColor(QColor(20, 40, 160), cs);
    s.backgroundColor = KoColor(Qt::white, cs);
    s.opacity = 0.7;
    s.seed = 99;
    s.perStrokeSeed = 1999;
    s.startDistance = VStrokePainter::startDistance(preset, testStroke().first().pi1.pos(), 0.0);
    return s;
}

/// feeds the primitives the way KisToolFreehandHelper sends them
void record(VStrokeRecorderFacade *facade, const QVector<VStrokePrimitive> &primitives)
{
    KisStrokeId id = facade->startStroke(new KisStrokeStrategy(QLatin1String("FREEHAND_STROKE")));
    QVERIFY(facade->isRunning());

    Q_FOREACH (const VStrokePrimitive &p, primitives) {
        switch (p.type) {
        case VStrokePrimitive::Point:
            facade->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1));
            break;
        case VStrokePrimitive::Line:
            facade->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1, p.pi2));
            break;
        case VStrokePrimitive::Curve:
            facade->addJob(id, new FreehandStrokeStrategy::Data(0, p.pi1, p.control1, p.control2, p.pi2));
            break;
        }
    }
    facade->endStroke(id);
}

QImage toImage(KisPaintDeviceSP device)
{
    return device->convertToQImage(nullptr, QRect(0, 0, 420, 240));
}

}

void VStrokeRecorderTest::initTestCase()
{
    ensurePaintOps();
}

void VStrokeRecorderTest::testLiveStrokeMatchesReRender_data()
{
    QTest::addColumn<QString>("paintop");
    QTest::newRow("pixel-scatter") << "paintbrush";
    QTest::newRow("csp-spray") << "cspbrush";
}

void VStrokeRecorderTest::testLiveStrokeMatchesReRender()
{
    QFETCH(QString, paintop);
    KisPaintOpPresetSP preset = paintop == "cspbrush" ? cspBrushPreset() : pixelBrushPreset();
    QVERIFY(preset);
    const VStrokeRenderSettings settings = settingsFor(preset);

    VStrokeRecorderFacade facade;
    int dirtyCalls = 0;
    bool finished = false;
    facade.dirtyCallback = [&] (const QVector<QRect> &) { dirtyCalls++; };
    facade.finishedCallback = [&] () { finished = true; };
    facade.prepare(settings);

    record(&facade, testStroke());
    QVERIFY(finished);
    QVERIFY(!facade.isRunning());
    QVERIFY(dirtyCalls > 0);
    QCOMPARE(facade.primitives().size(), testStroke().size());

    // the live stroke equals an offline render of the recorded primitives
    KisPaintDeviceSP offline = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    QVERIFY(VStrokePainter::render(offline, settings, facade.primitives()));
    const QImage live = toImage(facade.mergedDevice());
    QVERIFY(live == toImage(offline));

    // and a shape made from it re-renders the same, through its local coordinates
    VStrokeData data;
    data.preset = preset;
    data.opacity = settings.opacity;
    data.seed = settings.seed;
    data.perStrokeSeed = settings.perStrokeSeed;
    data.brushSize = 20;
    QScopedPointer<VStrokeShape> shape(
        VStrokeShape::fromImagePixels(facade.primitives(), 300.0 / 72.0, data));
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));

    KisPaintDeviceSP reRendered = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    QVERIFY(VStrokePainter::render(reRendered, shape->renderSettings(), shape->imagePixelPrimitives()));
    QVERIFY(toImage(reRendered) == live);
}

void VStrokeRecorderTest::testCancel()
{
    VStrokeRecorderFacade facade;
    bool finished = false;
    facade.finishedCallback = [&] () { finished = true; };
    facade.prepare(settingsFor(pixelBrushPreset()));

    KisStrokeId id = facade.startStroke(new KisStrokeStrategy(QLatin1String("FREEHAND_STROKE")));
    facade.addJob(id, new FreehandStrokeStrategy::Data(0, testStroke().first().pi1));
    QVERIFY(facade.cancelStroke(id));

    QVERIFY(!finished);
    QVERIFY(!facade.isRunning());
    QVERIFY(facade.primitives().isEmpty());
    QVERIFY(facade.device()->extent().isEmpty());
}

KISTEST_MAIN(VStrokeRecorderTest)
