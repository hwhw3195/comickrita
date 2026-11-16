/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSELECTION_COVER_H
#define VSELECTION_COVER_H

#include <QList>
#include <QTransform>

#include <kis_types.h>

#include "kritavstroke_export.h"

class KisShapeLayer;
class KoShape;

/// which shapes a raster selection covers (Clear, transforms limited to a selection)
namespace VSelectionCover
{
/// the shapes of a stock \p layer that Clear removes: all of them, or with
/// an active \p selection the ones it fully covers
KRITAVSTROKE_EXPORT QList<KoShape*> shapesToClear(KisShapeLayer *layer, KisSelectionSP selection);
/// whether \p selection holds every pixel \p shape paints
KRITAVSTROKE_EXPORT bool coveredBySelection(KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels);
}

#endif // VSELECTION_COVER_H
