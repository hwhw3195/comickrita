/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include <cmath>
#include <limits>

#include <kistest.h>
#include <kis_slider_spin_box.h>

#include "VCurveData.h"
#include "VCurveOpacity.h"
#include "VEditActions.h"
#include "VEditTool.h"
#include "VGeomCurveShape.h"

/**
 * The values the Vector Edit and Edit Shapes tools write: the Amount of
 * Opacity/Density Increase and Decrease, and the smallest radius a shape
 * keeps (VCurveData::MinRadius).
 */
class VEditValuesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testOpacityAmount_data();
    void testOpacityAmount();
    void testSetIgnoresAmount();
    void testShapesKeepAMinimumRadius();
    void testThinStopsAtTheMinimum();
    void testThicknessAmount();
    void testSliderArrowKeyStep();
    void testSizeShortcutSteps();
};

namespace {

VCurveData line(qreal radius)
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < 5; i++) d.addPoint(QPointF(i * 10.0, 0.0), radius);
    return d;
}

qreal opacityAt(const VCurveData &d, int i)
{
    return d.attribute(VCurveOpacity::Attribute, i, 1.0);
}

} // namespace

void VEditValuesTest::testOpacityAmount_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<qreal>("amount");
    QTest::addColumn<qreal>("start");
    QTest::addColumn<qreal>("expected");

    // Decrease moves toward 0, Increase toward 1, by the Amount of the way
    QTest::newRow("decrease, default 10%") << int(VEditSettings::OpacityTool::Decrease) << 0.1 << 1.0 << 0.9;
    QTest::newRow("decrease, 30%") << int(VEditSettings::OpacityTool::Decrease) << 0.3 << 1.0 << 0.7;
    QTest::newRow("decrease, 100%") << int(VEditSettings::OpacityTool::Decrease) << 1.0 << 1.0 << 0.0;
    QTest::newRow("decrease, 0%") << int(VEditSettings::OpacityTool::Decrease) << 0.0 << 1.0 << 1.0;
    QTest::newRow("increase, 50%") << int(VEditSettings::OpacityTool::Increase) << 0.5 << 0.2 << 0.6;
}

void VEditValuesTest::testOpacityAmount()
{
    QFETCH(int, tool);
    QFETCH(qreal, amount);
    QFETCH(qreal, start);
    QFETCH(qreal, expected);

    VCurveData d = line(2.0);
    if (start < 1.0) d.attributes.insert(VCurveOpacity::Attribute, QVector<qreal>(d.pointCount(), start));
    VEditSettings settings;
    settings.changeAmount = amount;
    settings.affectOpacity = true;
    settings.affectDensity = false;
    // only point 2 under the brush, at full weight and strength
    QVector<qreal> weights(d.pointCount(), 0.0);
    weights[2] = 1.0;
    VEditActions::changeOpacity(d, nullptr, weights, 1.0, VEditSettings::OpacityTool(tool), settings);

    QVERIFY2(qAbs(opacityAt(d, 2) - expected) < 1e-9, qPrintable(QString::number(opacityAt(d, 2))));
    QCOMPARE(opacityAt(d, 1), start);
    QCOMPARE(opacityAt(d, 3), start);

    // the strength scales it: half the strength, half the step
    VCurveData half = line(2.0);
    if (start < 1.0) half.attributes.insert(VCurveOpacity::Attribute, QVector<qreal>(half.pointCount(), start));
    VEditActions::changeOpacity(half, nullptr, weights, 0.5, VEditSettings::OpacityTool(tool), settings);
    const qreal halfExpected = start + (expected - start) / 2;
    QVERIFY2(qAbs(opacityAt(half, 2) - halfExpected) < 1e-9, qPrintable(QString::number(opacityAt(half, 2))));
}

void VEditValuesTest::testSetIgnoresAmount()
{
    VCurveData d = line(2.0);
    VEditSettings settings;
    settings.changeAmount = 0.9;
    settings.setOpacity = 0.25;
    QVector<qreal> weights(d.pointCount(), 1.0);
    VEditActions::changeOpacity(d, nullptr, weights, 1.0, VEditSettings::OpacityTool::Set, settings);
    for (int i = 0; i < d.pointCount(); i++) QCOMPARE(opacityAt(d, i), 0.25);
}

void VEditValuesTest::testShapesKeepAMinimumRadius()
{
    VCurveData d = line(2.0);
    d.radii[0] = 0.0;
    d.radii[1] = -1.0;
    d.radii[2] = std::numeric_limits<qreal>::quiet_NaN();
    d.radii[3] = VCurveData::MinRadius / 10;

    VGeomCurveShape shape;
    shape.setCurves(d);
    const VCurveData &kept = shape.curves();
    for (int i = 0; i < 4; i++) {
        QVERIFY2(kept.radii[i] == VCurveData::MinRadius, qPrintable(QString("point %1: %2").arg(i).arg(kept.radii[i])));
    }
    // radii above the minimum are untouched
    QCOMPARE(kept.radii[4], 2.0);
}

void VEditValuesTest::testThinStopsAtTheMinimum()
{
    // Thin at full strength, many times over, as dragging the brush over and over does
    VGeomCurveShape shape;
    shape.setCurves(line(2.0));
    for (int pass = 0; pass < 2000; pass++) {
        VCurveData d = shape.curves();
        for (qreal &r : d.radii) r *= 0.9;
        shape.setCurves(d);
    }
    for (qreal r : shape.curves().radii) {
        QVERIFY2(r >= VCurveData::MinRadius, qPrintable(QString::number(r)));
        QVERIFY2(r < 2 * VCurveData::MinRadius, qPrintable(QString::number(r)));
    }
    // and it still thickens back
    VCurveData d = shape.curves();
    for (qreal &r : d.radii) r *= 1.1;
    shape.setCurves(d);
    QVERIFY(shape.curves().radii[0] > VCurveData::MinRadius);
}

void VEditValuesTest::testThicknessAmount()
{
    // weights: full, half, none; strength 1
    const QVector<qreal> weights = {1.0, 0.5, 0.0, 0.0, 0.0};
    VCurveData d = line(2.0);
    VEditActions::changeThickness(d, weights, 1.0, true, 0.3);
    QCOMPARE(d.radii[0], 2.6);
    QCOMPARE(d.radii[1], 2.3);
    QCOMPARE(d.radii[2], 2.0);

    d = line(2.0);
    VEditActions::changeThickness(d, weights, 0.5, false, 0.4);
    QCOMPARE(d.radii[0], 1.6);
    QCOMPARE(d.radii[1], 1.8);

    // Thin at 100% and full weight: down to the minimum, not below or negative
    VGeomCurveShape shape;
    d = line(2.0);
    VEditActions::changeThickness(d, weights, 1.0, false, 1.0);
    QCOMPARE(d.radii[0], 0.0);
    shape.setCurves(d);
    QCOMPARE(shape.curves().radii[0], VCurveData::MinRadius);
    QCOMPARE(shape.curves().radii[1], 1.0);
}

void VEditValuesTest::testSliderArrowKeyStep()
{
    // the arrow keys and the wheel step a slider of 0 to 1 by 0.01, not by Qt's 1
    QWidget parent;
    KisDoubleSliderSpinBox *strength = VEditActions::doubleSlider(&parent, 0.01, 1, 0.5, 2, QString(), QString());
    QCOMPARE(strength->singleStep(), 0.01);
    strength->stepBy(1);
    QCOMPARE(strength->value(), 0.51);
    strength->stepBy(-2);
    QCOMPARE(strength->value(), 0.49);

    KisDoubleSliderSpinBox *transition = VEditActions::doubleSlider(&parent, 0, 0.5, 0.25, 2, QString(), QString());
    QCOMPARE(transition->singleStep(), 0.01);

    // wider ranges keep whole steps
    KisDoubleSliderSpinBox *size = VEditActions::doubleSlider(&parent, 0, 2000, 30, 1, QString(), QString());
    QCOMPARE(size->singleStep(), 1.0);
    size->stepBy(1);
    QCOMPARE(size->value(), 31.0);
    // and a range from 0 holds small values
    size->setValue(0.3);
    QCOMPARE(size->value(), 0.3);
    size->setValue(0);
    QCOMPARE(size->value(), 0.0);
}

void VEditValuesTest::testSizeShortcutSteps()
{
    // from 1 pt up as brush sizes step; below, by tenths down to 0
    auto close = [] (qreal a, qreal b) { return qAbs(a - b) < 1e-9; };
    QCOMPARE(VEditTool::steppedBrushSize(2, false), 1.0);
    QCOMPARE(VEditTool::steppedBrushSize(1, true), 2.0);
    QVERIFY(VEditTool::steppedBrushSize(30, true) > 30);
    QVERIFY(VEditTool::steppedBrushSize(30, false) < 30);
    QVERIFY(close(VEditTool::steppedBrushSize(1, false), 0.9));
    QVERIFY(close(VEditTool::steppedBrushSize(0.9, true), 1.0));
    QVERIFY(close(VEditTool::steppedBrushSize(0.95, true), 1.0));
    QVERIFY(close(VEditTool::steppedBrushSize(0.95, false), 0.9));
    QVERIFY(close(VEditTool::steppedBrushSize(0.3, false), 0.2));
    QVERIFY(close(VEditTool::steppedBrushSize(0.3, true), 0.4));
    QVERIFY(close(VEditTool::steppedBrushSize(0.1, false), 0.0));
    QCOMPARE(VEditTool::steppedBrushSize(0, false), 0.0);
    QVERIFY(close(VEditTool::steppedBrushSize(0, true), 0.1));
    // every step down from 2 reaches 0, every step up from 0 passes 1
    qreal size = 2;
    for (int i = 0; i < 11; i++) size = VEditTool::steppedBrushSize(size, false);
    QCOMPARE(size, 0.0);
    for (int i = 0; i < 11; i++) size = VEditTool::steppedBrushSize(size, true);
    QCOMPARE(size, 2.0);
}

KISTEST_MAIN(VEditValuesTest)

#include "VEditValuesTest.moc"
