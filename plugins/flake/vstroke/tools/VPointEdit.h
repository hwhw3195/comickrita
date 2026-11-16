/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VPOINT_EDIT_H
#define VPOINT_EDIT_H

#include <QList>
#include <QRectF>
#include <QPainterPath>
#include <QTransform>
#include <QVector>

#include <kundo2command.h>

#include "VCurveData.h"
#include "VFalloff.h"
#include "kritavstroke_export.h"

class VCurveShape;

/**
 * Editing the points of freehand curves, as the point edit tool does: the
 * points move as they are, nothing is resampled or smoothed. Points near
 * the selected ones follow with a falloff, like Blender's proportional
 * editing.
 */
namespace VPointEdit
{
using Profile = VFalloff::Profile;
using VFalloff::falloff;

/// how far each point and each bezier handle follows the selected points
struct KRITAVSTROKE_EXPORT Weights {
    QVector<qreal> points;
    /// per point, like the handle arrays; empty without curve segments
    QVector<qreal> handlesOut;
    QVector<qreal> handlesIn;

    bool movesAny() const;
};

/**
 * 1 for the selected points and their handles. Other points and handles
 * get the falloff weight of their own distance to the nearest selected
 * point, so a bezier segment whose ends move by different amounts bends
 * with the falloff instead of twisting. Distances are measured between
 * positions mapped by \p toDocument. A radius of 0 moves the selected
 * points only.
 */
KRITAVSTROKE_EXPORT Weights weights(const VCurveData &curves, const QVector<bool> &selected,
                                    const QTransform &toDocument, qreal radius,
                                    Profile profile = Profile::Smooth);

/// moves positions and handles by weight * \p delta (in the units of the positions)
KRITAVSTROKE_EXPORT void move(VCurveData &curves, const Weights &weights, const QPointF &delta);

/// scales each radius by 1 + weight * (\p factor - 1)
KRITAVSTROKE_EXPORT void scaleRadii(VCurveData &curves, const Weights &weights, qreal factor);

/// the point nearest to \p position within \p maxDistance, or -1; positions are mapped by \p toDocument
KRITAVSTROKE_EXPORT int nearestPoint(const VCurveData &curves, const QTransform &toDocument,
                                     const QPointF &position, qreal maxDistance);

/// the bezier handles of a point, as Edit Shapes shows them for a single selected point
enum class HandleSide { In, Out };
/// whether point \p index has handle \p side: the segment it belongs to is a bezier
KRITAVSTROKE_EXPORT bool hasHandle(const VCurveData &curves, int index, HandleSide side);

/**
 * How far apart the points around \p index are through \p toView: the mean
 * distance to the point before and the point after it in its curve, or to
 * the one neighbour at an end; 0 for a point alone.
 */
KRITAVSTROKE_EXPORT qreal pointSpacing(const VCurveData &curves, int index, const QTransform &toView);
/// the position of handle \p side of point \p index
KRITAVSTROKE_EXPORT QPointF handle(const VCurveData &curves, int index, HandleSide side);
/**
 * Moves handle \p side of point \p index to \p position. On a smooth
 * point the other handle turns to stay on the same line through the point,
 * keeping its length; on a symmetric point it mirrors this one.
 */
KRITAVSTROKE_EXPORT void moveHandle(VCurveData &curves, int index, HandleSide side, const QPointF &position);

/// the point types of the Edit Shapes context menu, as those of KoPathPointTypeCommand
enum class PointType { Corner, Smooth, Symmetric, Curve, Line };
/**
 * Corner: the handles move on their own. Smooth and Symmetric: the handles
 * stay on one line through the point (and equally long), and are set so;
 * line segments at the point become beziers first. Curve: line segments at
 * the point become beziers that still trace the line. Line: the segments
 * at the point become lines.
 */
KRITAVSTROKE_EXPORT void setPointType(VCurveData &curves, int index, PointType type);
/// the type point \p index has: Corner, Smooth or Symmetric
KRITAVSTROKE_EXPORT PointType pointType(const VCurveData &curves, int index);

/// the points inside \p rect, positions mapped by \p toDocument
KRITAVSTROKE_EXPORT QVector<int> pointsIn(const VCurveData &curves, const QTransform &toDocument, const QRectF &rect);
/// the points inside \p area, positions mapped by \p toDocument
KRITAVSTROKE_EXPORT QVector<int> pointsIn(const VCurveData &curves, const QTransform &toDocument, const QPainterPath &area);

/**
 * Splits \p curves by the \p selected points, for copy and cut: \p picked
 * gets the stretches between selected points (every segment whose two ends
 * are selected, and sub-paths of one selected point), \p rest the other
 * segments. A stretch's end point goes to both sides, so the two cover the
 * line exactly. Ends made by the split are flat, other ends keep their caps.
 * Sub-paths entirely on one side stay as they are. Either may be null.
 */
KRITAVSTROKE_EXPORT void splitSelected(const VCurveData &curves, const QVector<bool> &selected,
                                       VCurveData *picked, VCurveData *rest);
}

/**
 * Changes the curves of shapes from \p before to \p after. Copies of
 * VCurveData share their arrays until written, so keeping both is cheap.
 */
class KRITAVSTROKE_EXPORT VCurveEditCommand : public KUndo2Command
{
public:
    VCurveEditCommand(const QList<VCurveShape*> &shapes, const QList<VCurveData> &before,
                      const QList<VCurveData> &after, const KUndo2MagicString &text, KUndo2Command *parent = nullptr);

    void redo() override;
    void undo() override;

private:
    QList<VCurveShape*> m_shapes;
    QList<VCurveData> m_before;
    QList<VCurveData> m_after;
};

#endif // VPOINT_EDIT_H
