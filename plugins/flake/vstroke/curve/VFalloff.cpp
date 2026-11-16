/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VFalloff.h"

#include <cmath>

#include <QtMath>

qreal VFalloff::falloff(Profile profile, qreal distance, qreal radius)
{
    if (radius <= 0 || distance >= radius) return 0.0;
    // t: 1 at the selected point, 0 at the radius (Blender's definitions)
    const qreal t = 1.0 - distance / radius;
    switch (profile) {
    case Profile::Smooth:
        return t * t * (3.0 - 2.0 * t);
    case Profile::Sphere:
        return std::sqrt(qMax(0.0, t * (2.0 - t)));
    case Profile::Root:
        return std::sqrt(t);
    case Profile::InverseSquare:
        return t * (2.0 - t);
    case Profile::Sharp:
        return t * t;
    case Profile::Linear:
        return t;
    case Profile::Constant:
        return 1.0;
    }
    return t;
}
