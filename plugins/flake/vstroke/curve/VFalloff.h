/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VFALLOFF_H
#define VFALLOFF_H

#include <QtGlobal>

#include "kritavstroke_export.h"

/// falloffs for edits that reach around a point (point edit, Liquify of curves)
namespace VFalloff
{
/// the shape of the falloff, as Blender's proportional editing offers
enum class Profile {
    Smooth,
    Sphere,
    Root,
    InverseSquare,
    Sharp,
    Linear,
    Constant
};

/// the falloff weight at \p distance from a selected point: 1 at 0, 0 from \p radius on
KRITAVSTROKE_EXPORT qreal falloff(Profile profile, qreal distance, qreal radius);
}

#endif // VFALLOFF_H
