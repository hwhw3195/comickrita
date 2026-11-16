/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_OPACITY_H
#define VCURVE_OPACITY_H

#include <QImage>
#include <QPoint>
#include <QTransform>

#include "VCurveData.h"
#include "kritavstroke_export.h"

/**
 * The per-point opacity of a curve (the attribute "opacity", 1 when
 * missing), applied to its rendered pixels: each pixel takes the opacity
 * of the nearest place on the centerline, mixed along the segment there.
 * Both brush strokes (after the brush rendered them) and drawn shapes
 * (their line) use it.
 */
namespace VCurveOpacity
{
extern KRITAVSTROKE_EXPORT const QString Attribute;

/// whether any point of \p curves is less than fully opaque
KRITAVSTROKE_EXPORT bool isVarying(const VCurveData &curves);

/**
 * Multiplies the pixels of \p image, whose top left is at \p offset in
 * the coordinates \p toImage maps the curves to, by the opacity of the
 * nearest place on the centerline. ARGB32 and premultiplied ARGB32 only;
 * other formats are converted to premultiplied ARGB32 first.
 */
KRITAVSTROKE_EXPORT void applyMask(QImage &image, const QPoint &offset, const VCurveData &curves,
                                   const QTransform &toImage);
}

#endif // VCURVE_OPACITY_H
