/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QPainter>
#include <QTest>

#include <kistest.h>
#include <kundo2command.h>

#include <KoColorBackground.h>

#include "VCurveOpacity.h"
#include "VEditActions.h"
#include "VStrokeShape.h"
#include "VStrokeTestUtils.h"

using namespace VStrokeTestUtils;

/**
 * Apply Current Brush (Edit Shapes' Brush page): a brush stroke drawn again
 * with another preset keeps what was recorded and edited.
 */
class VLinePresetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testKeepsRecordedValues();
    void testUsePresetSize();
    void testUndoRedo();
    void testSkipsOtherShapes();
};

namespace {

const qreal PixelsPerPoint = 300.0 / 72.0;

VStrokeShape *strokeShape(KisPaintOpPresetSP preset)
{
    VStrokeData settings;
    settings.preset = preset;
    settings.presetName = preset->name();
    settings.brushSize = 20;
    settings.opacity = 0.8;
    settings.seed = 11;
    settings.perStrokeSeed = 22;
    VStrokeShape *shape = VStrokeShape::fromImagePixels(testStroke(), PixelsPerPoint, settings);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(20, 40, 160))));
    // edits made after drawing: a thicker middle and a faded end
    VCurveData d = shape->curves();
    d.radii[d.pointCount() / 2] *= 1.5;
    QVector<qreal> opacity(d.pointCount(), 1.0);
    opacity.last() = 0.3;
    d.attributes.insert(VCurveOpacity::Attribute, opacity);
    shape->setCurves(d);
    return shape;
}

/// a second brush: the pixel brush at another size and name
KisPaintOpPresetSP otherPreset(qreal size)
{
    KisPaintOpPresetSP preset = pixelBrushPreset()->clone().dynamicCast<KisPaintOpPreset>();
    preset->setName("vstroke_test_other");
    preset->settings()->setPaintOpSize(size);
    return preset;
}

int renderedAlpha(VStrokeShape *shape)
{
    const VStrokeRenderKey key{shape->renderTransform(), 0};
    const VStrokeRenderCache::Entry entry =
        VStrokeRenderCache::render(key, shape->renderSettings(), shape->imagePixelPrimitives());
    int sum = 0;
    for (int y = 0; y < entry.image.height(); y++) {
        for (int x = 0; x < entry.image.width(); x++) sum += qAlpha(entry.image.pixel(x, y));
    }
    return sum;
}

} // namespace

void VLinePresetTest::initTestCase()
{
    ensurePaintOps();
}

void VLinePresetTest::testKeepsRecordedValues()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    const VCurveData curves = shape->curves();
    const VStrokeData before = shape->data();
    const QColor color = shape->color();
    const quint64 revision = shape->revision();

    KisPaintOpPresetSP csp = cspBrushPreset();
    QVERIFY(csp);
    int count = 0;
    QScopedPointer<KUndo2Command> command(VEditActions::brushCommand({shape.data()}, csp, csp, true, &count));
    QVERIFY(command);
    QCOMPARE(count, 1);
    command->redo();

    const VStrokeData &after = shape->data();
    QCOMPARE(after.preset->paintOp().id(), QString("cspbrush"));
    QCOMPARE(after.presetName, csp->name());
    QVERIFY(after.presetDefinition);
    QVERIFY(after.presetDefinition != before.presetDefinition);
    // the stroke's size goes into the new brush
    QCOMPARE(after.brushSize, 20.0);
    QCOMPARE(after.preset->settings()->paintOpSize(), 20.0);
    // what was recorded and edited stays
    QVERIFY(shape->curves() == curves);
    QCOMPARE(shape->color(), color);
    QCOMPARE(after.opacity, before.opacity);
    QCOMPARE(after.seed, before.seed);
    QCOMPARE(after.perStrokeSeed, before.perStrokeSeed);
    QCOMPARE(after.compositeOp, before.compositeOp);
    QCOMPARE(after.startAngle, before.startAngle);
    // a new look, and it renders
    QVERIFY(shape->revision() != revision);
    QVERIFY(renderedAlpha(shape.data()) > 0);
    // the docker's preset is left as it was
    QVERIFY(csp->settings()->paintOpSize() != 20.0 || after.preset != csp);
}

void VLinePresetTest::testUsePresetSize()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    const VCurveData curves = shape->curves();
    KisPaintOpPresetSP other = otherPreset(40);
    QScopedPointer<KUndo2Command> command(VEditActions::brushCommand({shape.data()}, other, KisPaintOpPresetSP(),
                                                                      false));
    QVERIFY(command);
    command->redo();
    QCOMPARE(shape->data().brushSize, 40.0);
    QCOMPARE(shape->data().presetName, other->name());
    // twice the brush, twice every radius: the taper and the edits stay in proportion
    for (int i = 0; i < curves.pointCount(); i++) {
        QVERIFY2(qAbs(shape->curves().radii[i] - 2 * curves.radii[i]) < 1e-9, qPrintable(QString::number(i)));
    }
    QVERIFY(shape->curves().attributes.value(VCurveOpacity::Attribute) == curves.attributes.value(VCurveOpacity::Attribute));
}

void VLinePresetTest::testUndoRedo()
{
    QScopedPointer<VStrokeShape> shape(strokeShape(pixelBrushPreset()));
    const VCurveData curves = shape->curves();
    const VStrokeData before = shape->data();
    QScopedPointer<KUndo2Command> command(VEditActions::brushCommand({shape.data()}, otherPreset(30), KisPaintOpPresetSP(),
                                                                      false));
    command->redo();
    const VStrokeData after = shape->data();
    const VCurveData curvesAfter = shape->curves();
    QVERIFY(!(curvesAfter == curves));

    command->undo();
    QCOMPARE(shape->data().preset, before.preset);
    QCOMPARE(shape->data().presetName, before.presetName);
    QCOMPARE(shape->data().brushSize, before.brushSize);
    QVERIFY(shape->curves() == curves);

    command->redo();
    QCOMPARE(shape->data().presetDefinition, after.presetDefinition);
    QCOMPARE(shape->data().brushSize, 30.0);
    QVERIFY(shape->curves() == curvesAfter);
}

void VLinePresetTest::testSkipsOtherShapes()
{
    int count = -1;
    QScopedPointer<KUndo2Command> command(VEditActions::brushCommand({}, otherPreset(30), KisPaintOpPresetSP(), true, &count));
    QVERIFY(!command);
    QCOMPARE(count, 0);
}

KISTEST_MAIN(VLinePresetTest)

#include "VLinePresetTest.moc"
