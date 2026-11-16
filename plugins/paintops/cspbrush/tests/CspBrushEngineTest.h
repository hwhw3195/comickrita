/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_ENGINE_TEST_H
#define CSP_BRUSH_ENGINE_TEST_H

#include <QObject>

class CspBrushEngineTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testEffectorRoundTrip();
    void testEffectorValue();
    void testEffectorFade();
    void testVelocityMapping();
    void testRepeatMethods_data();
    void testRepeatMethods();
    void testTipImport();
    void testColorMixing();
    void testSpectralMixing();
    void testParamsFromData();
    void testVelocityCalibrationClamped();
    void testLineDirectionRotation();
    void testDiameterClamped();
    void testSprayDistribution();
    void testSprayDirection();
    void testColorJitter();
    void testTwoColorTipMix();
    void testNoMaskedBrush();
    void testDataRoundTrip();
    void testRibbonTiling();
    void testRibbonNoFoldOnTightCurve();
    void testRibbonRendersTip();
    void testRibbonHasNoGaps();
    void testRibbonStaysInsideBand();
    void testTextureModes();
    void testTextureSampling();
    void testTextureLodScale();
    void testTextureOutline();
    void testTextureRotation();
    void testTexturePattern();
    void testTextureInvertsForEraser();
    void testWaterEdgeStrength();
    void testColorMixerEndpoints();
    void testColorMixerErases();
    void testColorMixerStretch();
    void testColorMixerCarriedReachesTarget();
    void testColorMixerLayDown();
    void testSampleCanvasWeightedAlpha();
    void testSampleCanvasSparse();
    void testColorMixerPerceptual();
    void testWaterEdgeWithMixing();
    void testColorMixerRespectsSelection();
};

#endif // CSP_BRUSH_ENGINE_TEST_H
