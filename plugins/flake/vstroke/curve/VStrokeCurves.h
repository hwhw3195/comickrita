/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_CURVES_H
#define VSTROKE_CURVES_H

#include <QTransform>
#include <QVector>

#include "VCurveData.h"
#include "VStrokePrimitive.h"
#include "kritavstroke_export.h"

/**
 * Brush strokes as VCurveData, and back to the primitives the brush engine
 * replays.
 *
 * Each sample becomes a point; its KisPaintInformation values are point
 * attributes (left out while every point has the default). Line and curve
 * primitives become segments, point primitives the Dab flag. A primitive
 * that does not start where the previous one ended starts a new sub-path.
 * toPrimitives() gives back the same primitives, in the same order, with the
 * same values, so a stroke replays exactly as it was drawn.
 * Sub-paths apart from each other, as a trim leaves them, are the exception:
 * the first primitive of each is marked restartsStroke, so they don't paint
 * across the gap. A sub-path starting inside the ink where the one before
 * ends (within its radius, at least a pixel) goes on with the stroke: the
 * freehand helper starts those a hair away at pen jitter, in one paintop.
 *
 * The radius of a point is in image pixels and starts at baseRadius(). At
 * replay, a radius edited away from it scales the dabs there through
 * KisPaintInformation::sizeMultiplier().
 *
 * The attributes DensityScale (default 1) and DensityOffset (default 0)
 * remap the density of the dabs there (their opacity and flow) through
 * KisPaintInformation::setDensity(); they are not recorded, only edited.
 */
namespace VStrokeCurves
{
extern KRITAVSTROKE_EXPORT const QString DensityScale;
extern KRITAVSTROKE_EXPORT const QString DensityOffset;

/// the radius of a sample of a stroke drawn with \p brushSize (a diameter in image pixels)
KRITAVSTROKE_EXPORT qreal baseRadius(qreal brushSize, qreal pressure);

KRITAVSTROKE_EXPORT VCurveData fromPrimitives(const QVector<VStrokePrimitive> &primitives, qreal brushSize);

/// positions and handles mapped by \p t
KRITAVSTROKE_EXPORT QVector<VStrokePrimitive> toPrimitives(const VCurveData &curves, qreal brushSize,
                                                           const QTransform &t = QTransform());

/// the paint information of point \p index, positioned by \p t
KRITAVSTROKE_EXPORT KisPaintInformation paintInfo(const VCurveData &curves, int index, qreal brushSize,
                                                  const QTransform &t = QTransform());
}

#endif // VSTROKE_CURVES_H
