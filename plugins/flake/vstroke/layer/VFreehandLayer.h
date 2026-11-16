/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VFREEHAND_LAYER_H
#define VFREEHAND_LAYER_H

#include <QList>

#include <kis_types.h>

#include "kritavstroke_export.h"

class KoShape;
class KisShapeLayer;
class KUndo2Command;
class KoShapeControllerBase;

/**
 * A freehand vector layer is a stock vector layer with the "freehandvector"
 * node property set; .kra files save it. Every stock vector layer feature
 * works on it. On it, the vector brush draws, and stock paths added by the
 * shape tools, paste or drag and drop become freehand curves.
 */
namespace VFreehandLayer
{
extern KRITAVSTROKE_EXPORT const char *const Property;

KRITAVSTROKE_EXPORT bool isFreehand(const KisNode *node);
inline bool isFreehand(KisNodeSP node) { return isFreehand(node.data()); }
KRITAVSTROKE_EXPORT void setFreehand(KisNode *node, bool freehand = true);

/// whether \p shapes hold freehand curves (brush strokes or converted paths), in groups too
KRITAVSTROKE_EXPORT bool containsCurves(const QList<KoShape*> &shapes);

/**
 * Cuts \p layer, a copy not in the image, down to \p selection: its curves
 * keep the parts inside it, and other shapes stay if their bounds reach it.
 * False when \p layer isn't a freehand layer.
 */
KRITAVSTROKE_EXPORT bool keepWithinSelection(KisShapeLayer *layer, KisSelectionSP selection);

/**
 * Sets the KisShapeLayerHooks once: shapes added to a freehand layer
 * convert, and pasted freehand curves get a new freehand layer unless the
 * active layer is one.
 */
KRITAVSTROKE_EXPORT void installHooks();

/**
 * The command turning \p layer into a freehand layer: the flag, and its
 * stock paths replaced by freehand curves, through \p controller (the
 * document's). Null when \p layer is one already.
 */
KRITAVSTROKE_EXPORT KUndo2Command *convertCommand(KisShapeLayer *layer, KoShapeControllerBase *controller);
}

#endif // VFREEHAND_LAYER_H
