/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_EFFECTOR_H
#define CSP_EFFECTOR_H

#include <QList>
#include <QString>
#include <QVector>
#include <array>

#include <kis_cubic_curve.h>

#include "kritacspbrushpaintop_export.h"

class KisPaintInformation;

/**
 * One CSP "effect source" popup: which inputs modulate a setting, the
 * minimum each input can bring it down to and the input curves. Next to
 * CSP's pressure, tilt, velocity and random it has Krita's fade, distance
 * and time inputs.
 *
 * value = PRODUCT over enabled inputs of lerp(min, 1, curve(x))
 *
 * The result is not clamped: a negative random minimum makes it negative
 * sometimes, as in CSP; the paintop clamps where it matters.
 *
 * Serialized form (stored as CSP/<Setting>Effector, written by sut2kpp --engine csp):
 *   "on=p,f;p=0.3@0,0 0.5,0.6,c 1,1;f=0@0,0 1,1;fl=200;fp=1;r=-0.5"
 * Inputs: p pressure, t tilt, v velocity, r random, f fade, d distance,
 * tm time. After '@' the curve points, ",c" marks a corner; a missing curve
 * is linear. Minimums are fractions (0..1, random -1..1). fl/dl/tml are the
 * fade (dabs), distance (px) and time (ms) lengths, fp/dp/tmp=1 make them
 * periodic.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspEffector
{
public:
    enum Input { Pressure = 0, Tilt, Velocity, Random, Fade, Distance, Time, InputCount };

    CspEffector() = default;
    static CspEffector fromString(const QString &text);
    QString toString() const;

    static bool hasCurve(Input input) { return input != Random; }
    static bool hasLength(Input input) { return input >= Fade; }
    static int defaultLength(Input input);

    bool isEnabled(Input input) const { return m_enabled[input]; }
    bool isActive() const;
    qreal minimum(Input input) const { return m_min[input]; }
    /// Empty: linear
    const QList<KisCubicCurvePoint> &curve(Input input) const { return m_curves[input]; }
    int length(Input input) const { return m_length[input]; }
    bool isPeriodic(Input input) const { return m_periodic[input]; }

    void setEnabled(Input input, bool value) { m_enabled[input] = value; }
    void setMinimum(Input input, qreal value) { m_min[input] = value; }
    void setCurve(Input input, const QList<KisCubicCurvePoint> &points);
    void setLength(Input input, int value) { m_length[input] = qMax(1, value); }
    void setPeriodic(Input input, bool value) { m_periodic[input] = value; }

    /// Velocity input calibration in px/ms (x-VelocityFast, x-VelocityKnee)
    void setVelocityCalibration(qreal fast, qreal knee);

    /// CSP's velocity axis (1 = slow, 0 = fast) for Krita's speed (min(1, px per ms / 30))
    static qreal velocityFromKritaSpeed(qreal kritaSpeed, qreal fast = 2.0, qreal knee = 0.3);

    /// Input x (0..1, CSP's axes: tilt 0 = upright, velocity 1 = slow) for a dab
    qreal input(Input input, const KisPaintInformation &info) const;

    /// The multiplier for the setting's slider value; 1 with no input on
    qreal value(const KisPaintInformation &info) const;

private:
    std::array<bool, InputCount> m_enabled {};
    std::array<qreal, InputCount> m_min {};
    std::array<QList<KisCubicCurvePoint>, InputCount> m_curves;
    std::array<QVector<qreal>, InputCount> m_transfer; ///< empty: linear
    std::array<int, InputCount> m_length {{0, 0, 0, 0, 1000, 1000, 3000}};
    std::array<bool, InputCount> m_periodic {};
    int m_tiltMax {100};
    qreal m_velocityFast {2.0};
    qreal m_velocityKnee {0.3};
};

#endif // CSP_EFFECTOR_H
