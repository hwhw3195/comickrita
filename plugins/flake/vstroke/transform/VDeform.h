/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VDEFORM_H
#define VDEFORM_H

#include <QTransform>

#include <functional>
#include <memory>

#include <KisShapeLayerHooks.h>

#include "VCurveData.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KoPathShape;
class KUndo2Command;

/**
 * The transform tool's deforming modes (Perspective, Warp, Cage, Mesh) on a
 * freehand vector layer: the mode sends every point of the image somewhere, and the points
 * and handles of the curves go there themselves, so the preview is the
 * result. Stock paths on the layer move the same way; text and images stay.
 *
 * A VMoveSession per layer and stroke does the moving; this gives it the
 * targets: the curves mapped from where they were before the stroke.
 *
 * Every point keeps its place in its curve, so a brush stroke's earlier
 * rendering can stand in, bent to the new curves, until it renders again.
 * A handle keeps its direction relative to the space around its point (it
 * goes by the map's derivative there), so a smooth point stays smooth, and
 * a line the map bends noticeably becomes a bezier along the bend.
 *
 * With a selection, or Cage's polygon, only what is inside moves: curves
 * are cut at its edge (as Free Transform cuts them at a selection), and the
 * pieces inside move as a new shape. Paths aren't cut: they move when all
 * of them is inside.
 */
namespace VDeform
{
using PointMap = std::function<QPointF(const QPointF &)>;
/// maps all of \p points into \p mapped, as a PointMap would one by one (KisShapeLayerHooks::DeformMap::mapAll)
using BatchMap = std::function<void(const QVector<QPointF> &points, QVector<QPointF> &mapped)>;

/**
 * Every point mapped() needs mapped for one shape's curves, in image pixels:
 * each point and four probes around it (the derivative), and for each segment
 * its middle and five points along it (the bend test and the handle fit). It
 * depends only on the curves and \p toImage, so a stroke builds it once per
 * part and maps it whole each pass.
 */
struct KRITAVSTROKE_EXPORT Samples {
    QVector<QPointF> points;    ///< image pixels
    QVector<int> segmentStart;  ///< per curve point: the index of its segment's middle in \p points, or -1
    QTransform fromImage;
    qreal step = 0;             ///< the derivative's step, in the curves' units
    qreal tolerance = 0;        ///< the bend tolerance, in the curves' units
    bool isValid() const { return step > 0; }
};
KRITAVSTROKE_EXPORT Samples samples(const VCurveData &curves, const QTransform &toImage);

/// \p curves moved, from their \p samples mapped by \p map (see mapped() below)
KRITAVSTROKE_EXPORT VCurveData mapped(const VCurveData &curves, const Samples &samples, const BatchMap &map, bool scaleRadii);

/// a BatchMap calling \p map point by point
KRITAVSTROKE_EXPORT BatchMap batchOf(const PointMap &map);

/**
 * \p curves, in units that \p toImage takes to image pixels, moved by
 * \p map, which works in image pixels. With \p scaleRadii, the radii scale
 * by what the map does to areas at each point.
 */
KRITAVSTROKE_EXPORT VCurveData mapped(const VCurveData &curves, const QTransform &toImage, const PointMap &map, bool scaleRadii);

/// moves the points of \p path as mapped() moves curves; it then normalizes, so its transformation may change
KRITAVSTROKE_EXPORT void mapPath(KoPathShape *path, const QTransform &documentToImage, const PointMap &map);

/// the KisShapeLayerHooks::DeformCommand for a freehand layer; null for other layers
KRITAVSTROKE_EXPORT KUndo2Command *command(KisShapeLayer *layer, const KisShapeLayerHooks::DeformMap &map, std::shared_ptr<void> &session);

/// the KisShapeLayerHooks::DeformDone: ends the layer's VMoveSession (any thread)
KRITAVSTROKE_EXPORT void done(KisShapeLayer *layer, std::shared_ptr<void> &session, bool keep);

/// sets the deform hooks of KisShapeLayerHooks for freehand layers
KRITAVSTROKE_EXPORT void installHooks();
}

#endif // VDEFORM_H
