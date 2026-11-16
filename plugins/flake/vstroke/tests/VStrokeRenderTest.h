/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_RENDER_TEST_H
#define VSTROKE_RENDER_TEST_H

#include <QObject>

/**
 * The brush render cache of VStrokeShape, painted the way
 * KisShapeLayerCanvas paints shapes.
 */
class VStrokeRenderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    void testCacheIsBlittedOneToOne();
    void testCloneUsesSharedRendering();
    void testMoveRerenders();
    void testMovedCurvesReindexed();
    void testEraseStrokeErases();
    void testBlendModeOverStrokeBelow_data();
    void testBlendModeOverStrokeBelow();
    void testBlendModeOverEmptyIsNormal();
    void testKritaBlendMode_data();
    void testKritaBlendMode();
    void testKritaBlendModeKeepsClipAndOpacity();
    void testUnknownBlendModeIsNormal();
    void testMissingPresetPaintsOutline();
    void testPerPointOpacity();
    void testFastLiquifyWarpsUntilRendered();
    void testMovedPointShowsLastRendering();
    void testScaledStrokeKeepsBrushWidth();
};

#endif // VSTROKE_RENDER_TEST_H
