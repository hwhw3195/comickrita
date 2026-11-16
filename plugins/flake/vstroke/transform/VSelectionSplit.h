/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSELECTION_SPLIT_H
#define VSELECTION_SPLIT_H

#include <QTransform>

#include <memory>

#include <kis_types.h>

#include "VCurveData.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KoShapeControllerBase;
class KUndo2Command;
class VCurveShape;

/**
 * Cuts the curves of a freehand layer at the edge of a selection, for Copy,
 * Cut, Del and Free Transform. A selection is a pixel mask, so a line is cut
 * where the mask says, checked every image pixel along it (VCurveTrim::keepInside),
 * and at more than half selected: a soft selection cuts hard. The cut goes
 * through the center line, so the width of the line can reach past the edge.
 *
 * Other shapes aren't cut: they count as selected when it fully covers them.
 */
namespace VSelectionSplit
{
/// the parts of \p shape where \p selection is set; \p documentToPixels is the image's resolution
KRITAVSTROKE_EXPORT VCurveData within(const VCurveShape *shape, KisSelectionSP selection, const QTransform &documentToPixels);

/// the command removing what \p selection holds from freehand \p layer (one to call in the GUI thread, it may run in any); null when that is nothing
KRITAVSTROKE_EXPORT KUndo2Command *cutCommand(KisShapeLayer *layer, KoShapeControllerBase *controller, KisSelectionSP selection);

/**
 * The command moving what \p selection holds on \p layer by \p imageTransform;
 * what is outside stays. The layer's own transformation doesn't change. Null
 * when \p layer isn't a freehand layer.
 *
 * The commands of one transform stroke share \p session, so the cut happens
 * once, however many commands the preview makes and undoes.
 */
KRITAVSTROKE_EXPORT KUndo2Command *moveCommand(KisShapeLayer *layer, KisSelectionSP selection, const QTransform &imageTransform,
                                               bool scaleBrushSizes, std::shared_ptr<void> &session);

/// what \p transform does to a stroke's width: the scale of areas, of its affine part
KRITAVSTROKE_EXPORT qreal brushScale(const QTransform &transform);
}

#endif // VSELECTION_SPLIT_H
