/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_STROKE_TEST_H
#define CSP_BRUSH_STROKE_TEST_H

#include <QObject>

/// Whole strokes on an image, through the freehand strategy and the canvas resources
class CspBrushStrokeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testBlendWithoutMixing_data();
    void testBlendWithoutMixing();
    void testEraseWithMixing_data();
    void testEraseWithMixing();
    void testEraseCarriesColor();
    void testStandardKeepsFaintAlpha_data();
    void testStandardKeepsFaintAlpha();
    void testBlendWithMixing_data();
    void testBlendWithMixing();
    void testNormalMixingStillPaints();
    void testBlendWithMixingAlphaLock();
    void testBlendWithMixingSelection();
    void testRibbonIgnoresMixing();
    void testMatchesCspReference_data();
    void testMatchesCspReference();
};

#endif // CSP_BRUSH_STROKE_TEST_H
