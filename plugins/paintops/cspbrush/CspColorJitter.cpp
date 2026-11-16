/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspColorJitter.h"

#include <QColor>
#include <QtMath>
#include <cmath>

#include <KisPerStrokeRandomSource.h>
#include <kis_paint_information.h>

namespace {
qreal strokeRandom(const KisPaintInformation &info, const QString &key)
{
    return info.perStrokeRandomSource() ? info.perStrokeRandomSource()->generateNormalized(key) : 0.5;
}
}

CspColorJitter::CspColorJitter(const CspBrushParams &params)
    : m_params(params)
{
}

CspColorJitter::Shift CspColorJitter::shift(const KisPaintInformation &info) const
{
    Shift s;
    if (m_params.jitterPerDab) {
        s.hue += m_params.hueChange * m_params.hueChangeDynamics.value(info);
        s.saturation += m_params.saturationChange * m_params.saturationChangeDynamics.value(info);
        s.value += m_params.valueChange * m_params.valueChangeDynamics.value(info);
        s.subColor += m_params.subColorChange * m_params.subColorChangeDynamics.value(info);
    }
    if (m_params.jitterPerStroke) {
        s.hue += m_params.strokeHueChange * (2.0 * strokeRandom(info, "csp_hue") - 1.0);
        s.saturation += m_params.strokeSaturationChange * (2.0 * strokeRandom(info, "csp_saturation") - 1.0);
        s.value += m_params.strokeValueChange * (2.0 * strokeRandom(info, "csp_value") - 1.0);
        s.subColor += m_params.strokeSubColorChange * strokeRandom(info, "csp_subcolor");
    }
    s.subColor = qBound(0.0, s.subColor, 1.0);
    return s;
}

KoColor CspColorJitter::apply(const KoColor &main, const KoColor &sub, const KisPaintInformation &info) const
{
    return apply(main, sub, shift(info), m_params.mixingMode);
}

KoColor CspColorJitter::apply(const KoColor &main, const KoColor &sub, const Shift &shift, CspColorMath::Mode mode)
{
    KoColor color = main;
    if (!qFuzzyIsNull(shift.hue) || !qFuzzyIsNull(shift.saturation) || !qFuzzyIsNull(shift.value)) {
        QColor c = main.toQColor();
        qreal h, s, v, a;
        c.getHsvF(&h, &s, &v, &a);
        if (h < 0) h = 0; // achromatic
        h = std::fmod(h + shift.hue / 360.0 + 1.0, 1.0);
        c.setHsvF(h, qBound(0.0, s + shift.saturation, 1.0), qBound(0.0, v + shift.value, 1.0), a);
        color.fromQColor(c);
    }
    if (shift.subColor > 0.0) {
        color = CspColorMath::mix(color, sub.convertedTo(color.colorSpace()), float(shift.subColor), mode);
    }
    return color;
}
