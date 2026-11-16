/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_CONVERTER_H
#define VCURVE_CONVERTER_H

#include <QList>

#include "kritavstroke_export.h"

class KoShape;
class KoPathShape;
class VGeomCurveShape;

/**
 * Turns stock paths into freehand vector curves (VGeomCurveShape) for
 * freehand vector layers: the shape tools, paste and "Convert to Freehand
 * Vector Layer" all go through here.
 */
namespace VCurveConverter
{
/**
 * Whether \p shape converts: a path (boxes and ellipses are paths too)
 * with no fill or a solid fill, and no line or a solid line without dashes
 * or arrowheads. Text, images, groups and other paths stay as they are.
 */
KRITAVSTROKE_EXPORT bool canConvert(const KoShape *shape);

/**
 * A curve with the same sub-paths, points, handles, transformation, fill
 * and line as \p path; every radius is half the line width. Null when the
 * path does not convert.
 */
KRITAVSTROKE_EXPORT VGeomCurveShape *fromPath(const KoPathShape *path);

/**
 * Replaces the shapes that convert in \p shapes with their curves and
 * deletes them; the others stay. Returns whether any changed.
 */
KRITAVSTROKE_EXPORT bool convertInPlace(QList<KoShape*> &shapes);
}

#endif // VCURVE_CONVERTER_H
