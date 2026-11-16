/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_COLOR_JITTER_H
#define CSP_COLOR_JITTER_H

#include <KoColor.h>

#include "CspBrushParams.h"

class KisPaintInformation;

/**
 * CSP's color jitter: hue, saturation, value and the share of the sub
 * color, changed per dab ("Change brush tip color") and/or per stroke.
 *
 * Per dab, each amount is scaled by its effect source: CSP's default is
 * Random with a -100% minimum, which gives a factor in -1..1, so the
 * randomness lives in the effector. Per stroke, one draw per stroke
 * (-1..1 for hue, saturation and value, 0..1 for the sub color). The two
 * add up.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspColorJitter
{
public:
    explicit CspColorJitter(const CspBrushParams &params);

    bool isActive() const { return m_params.jitterPerDab || m_params.jitterPerStroke; }

    struct Shift {
        qreal hue {0.0};        ///< degrees
        qreal saturation {0.0}; ///< -1..1, added
        qreal value {0.0};      ///< -1..1, added
        qreal subColor {0.0};   ///< share of the sub color, 0..1
    };
    Shift shift(const KisPaintInformation &info) const;

    /// The main color of one dab: @p main shifted, then mixed with @p sub (in the brush's mixing mode)
    KoColor apply(const KoColor &main, const KoColor &sub, const KisPaintInformation &info) const;
    static KoColor apply(const KoColor &main, const KoColor &sub, const Shift &shift, CspColorMath::Mode mode);

private:
    CspBrushParams m_params;
};

#endif // CSP_COLOR_JITTER_H
