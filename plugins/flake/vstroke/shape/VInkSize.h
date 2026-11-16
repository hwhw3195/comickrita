/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VINK_SIZE_H
#define VINK_SIZE_H

#include <QHash>
#include <QString>
#include <QVector>

#include <functional>

#include "VCurveData.h"
#include "kritavstroke_export.h"

class KisPaintInformation;
class KisPropertiesConfiguration;
class VCurveShape;

/**
 * How wide a brush stroke really paints, for Trim.
 *
 * The radius a stroke stores is ½ × brush size × pressure (see
 * VStrokeCurves::baseRadius()), but the brush scales its dabs by its own
 * size response: a pressure curve with a minimum (a CSP brush's
 * "BrushSizeEffector", a pixel brush's Size option), tilt or rotation. The
 * ink radius of a point is ½ × brush size × that response × the point's
 * size multiplier (a radius edited away from the stored one).
 *
 * Only pressure, tilt and rotation count: velocity, random, fade, distance
 * and time are taken as 1. Brush engines other than the CSP brush
 * ("cspbrush") and the pixel brush ("paintbrush") keep the stored radius.
 */
namespace VInkSize
{
/// a curve shape's radii in document units per stored unit: brush strokes keep theirs in image pixels
KRITAVSTROKE_EXPORT qreal radiusScale(const VCurveShape *shape);

/// the brush's size response at a dab (1 = the brush size)
using Response = std::function<qreal(const KisPaintInformation &)>;

/// the size response of the paintop \p paintopId with \p settings; empty for other engines
KRITAVSTROKE_EXPORT Response response(const QString &paintopId, const KisPropertiesConfiguration *settings);

/// the ink radius of each point of \p curves drawn with \p brushSize (image pixels), by \p response
KRITAVSTROKE_EXPORT QVector<qreal> radii(const VCurveData &curves, qreal brushSize, const Response &response);

/**
 * The ink radii of shapes in document units, each brush's response read
 * once.
 */
class KRITAVSTROKE_EXPORT Cache
{
public:
    /// the ink radius of each point of sub-path \p subPath of \p shape; its stored radii for shapes without a brush
    QVector<qreal> documentRadii(const VCurveShape *shape, int subPath);
    /// the same for \p curves, a sub-path of \p shape in its own coordinates
    QVector<qreal> documentRadii(const VCurveShape *shape, const VCurveData &curves);

private:
    QHash<const void *, Response> m_responses;
};
}

#endif // VINK_SIZE_H
