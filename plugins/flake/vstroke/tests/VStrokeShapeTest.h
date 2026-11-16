/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_SHAPE_TEST_H
#define VSTROKE_SHAPE_TEST_H

#include <QObject>

class VStrokeShapeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // must run before the factory is registered
    void testStockReaderFallback();
    void testCompositeOpRoundTrip();
    void testOldEraseAttributeLoads_data();
    void testOldEraseAttributeLoads();

    void testSvgRoundTrip();
    void testLoadVersion2();
    void testPlainPathStillLoadsAsPath();
    void testCloneSharesData();
    void testTransformKeepsImagePixels();
};

#endif // VSTROKE_SHAPE_TEST_H
