/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PARITY_TEST_H
#define VSTROKE_PARITY_TEST_H

#include <QObject>

/**
 * A stroke on a vector layer must look like the same stroke on a raster
 * layer. Paints the same primitives through Krita's freehand stroke on a
 * paint layer (indirect painting, opacity merge) and as a vector stroke
 * blitted like KisShapeLayerCanvas does, on an 8-bit sRGB image.
 *
 * FreehandStrokeStrategy cannot be seeded, so the brushes here use no
 * randomness; VStrokePainterTest covers random brushes.
 */
class VStrokeParityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    void testRasterVectorParity_data();
    void testRasterVectorParity();
};

#endif // VSTROKE_PARITY_TEST_H
