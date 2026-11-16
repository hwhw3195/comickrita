/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PAINTER_TEST_H
#define VSTROKE_PAINTER_TEST_H

#include <QObject>

/**
 * Vector strokes re-render from their recorded primitives, so painting the
 * same primitives with the same seeds must give the same pixels, byte for
 * byte. Covers the pixel brush (scatter) and cspbrush (spray).
 */
class VStrokePainterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    void testPrimitiveXmlRoundTrip();

    void testDeterminism_data();
    void testDeterminism();

    void testSizeMultiplier_data();
    void testSizeMultiplier();

    void testDensity_data();
    void testDensity();

    void testCurvesReplayAsDrawn_data();
    void testCurvesReplayAsDrawn();
    void testGapBetweenPiecesStaysEmpty_data();
    void testGapBetweenPiecesStaysEmpty();
    void testEditedRadiusScalesDabs();
    void testLaterPieceKeepsEarlierInk();
    void testTipsInsideTheInkContinue();
};

#endif // VSTROKE_PAINTER_TEST_H
