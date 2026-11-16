/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspEffector.h"

#include <QStringList>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include <kis_paint_information.h>
#include <kis_random_source.h>

namespace {
const char *const inputKeys[CspEffector::InputCount] = {"p", "t", "v", "r", "f", "d", "tm"};

int inputFromKey(const QString &key)
{
    for (int i = 0; i < CspEffector::InputCount; i++) {
        if (key == QLatin1String(inputKeys[i])) return i;
    }
    return -1;
}

bool isIdentity(const QList<KisCubicCurvePoint> &points)
{
    return points.isEmpty() || KisCubicCurve(points).isIdentity();
}
}

int CspEffector::defaultLength(Input input)
{
    switch (input) {
    case Fade: return 1000;
    case Distance: return 1000;
    case Time: return 3000;
    default: return 0;
    }
}

qreal CspEffector::velocityFromKritaSpeed(qreal kritaSpeed, qreal fast, qreal knee)
{
    // Krita's speed is min(1, px per ms / 30) (KisPaintingInformationBuilder)
    const qreal v = qMax(0.0, kritaSpeed) * 30.0;
    const qreal fastness = qMin(1.0, std::log1p(v / knee) / std::log1p(fast / knee));
    return 1.0 - fastness;
}

CspEffector CspEffector::fromString(const QString &text)
{
    CspEffector e;
    const QStringList parts = text.split(';', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int eq = part.indexOf('=');
        if (eq < 0) continue;
        const QString key = part.left(eq).trimmed();
        const QString value = part.mid(eq + 1).trimmed();

        if (key == "on") {
            for (const QString &name : value.split(',', Qt::SkipEmptyParts)) {
                const int input = inputFromKey(name.trimmed());
                if (input >= 0) e.m_enabled[input] = true;
            }
            continue;
        }
        if (key == "tmax") {
            e.m_tiltMax = value.toInt();
            continue;
        }
        // lengths and periodic flags: "<input>l", "<input>p"
        const QChar suffix = key.isEmpty() ? QChar() : key.back();
        const int lengthInput = inputFromKey(key.chopped(qMin(1, int(key.size()))));
        if ((suffix == 'l' || suffix == 'p') && lengthInput >= 0 && hasLength(Input(lengthInput))) {
            if (suffix == 'l') e.setLength(Input(lengthInput), value.toInt());
            else e.m_periodic[lengthInput] = value.toInt() != 0;
            continue;
        }

        const int input = inputFromKey(key);
        if (input < 0) continue;
        const int at = value.indexOf('@');
        e.m_min[input] = (at < 0 ? value : value.left(at)).toDouble();
        if (at >= 0) {
            QList<KisCubicCurvePoint> points;
            for (const QString &pt : value.mid(at + 1).split(' ', Qt::SkipEmptyParts)) {
                const QStringList xy = pt.split(',');
                if (xy.size() >= 2) {
                    points << KisCubicCurvePoint(xy[0].toDouble(), xy[1].toDouble(), xy.size() > 2 && xy[2] == "c");
                }
            }
            e.setCurve(Input(input), points);
        }
    }
    return e;
}

QString CspEffector::toString() const
{
    QStringList on;
    QStringList parts;
    for (int i = 0; i < InputCount; i++) {
        if (m_enabled[i]) on << inputKeys[i];
    }
    parts << "on=" + on.join(',');
    for (int i = 0; i < InputCount; i++) {
        QString s = QString("%1=%2").arg(inputKeys[i]).arg(m_min[i]);
        if (!m_curves[i].isEmpty()) {
            QStringList pts;
            for (const KisCubicCurvePoint &p : m_curves[i]) {
                pts << QString("%1,%2%3").arg(p.x()).arg(p.y()).arg(p.isSetAsCorner() ? ",c" : "");
            }
            s += "@" + pts.join(' ');
        }
        parts << s;
        if (hasLength(Input(i))) {
            if (m_length[i] != defaultLength(Input(i))) parts << QString("%1l=%2").arg(inputKeys[i]).arg(m_length[i]);
            if (m_periodic[i]) parts << QString("%1p=1").arg(inputKeys[i]);
        }
    }
    if (m_tiltMax != 100) parts << QString("tmax=%1").arg(m_tiltMax);
    return parts.join(';');
}

bool CspEffector::isActive() const
{
    return std::any_of(m_enabled.begin(), m_enabled.end(), [](bool b) { return b; });
}

void CspEffector::setCurve(Input input, const QList<KisCubicCurvePoint> &points)
{
    QList<KisCubicCurvePoint> sorted = points;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const KisCubicCurvePoint &a, const KisCubicCurvePoint &b) { return a.x() < b.x(); });
    m_curves[input] = sorted;
    m_transfer[input] = isIdentity(sorted) ? QVector<qreal>() : KisCubicCurve(sorted).floatTransfer(256);
}

void CspEffector::setVelocityCalibration(qreal fast, qreal knee)
{
    m_velocityFast = fast;
    m_velocityKnee = knee;
}

// fade, distance and time as libpaintop/sensors/KisDynamicSensorFade/Distance/Time.cpp
qreal CspEffector::input(Input input, const KisPaintInformation &info) const
{
    switch (input) {
    case Pressure:
        return info.pressure();
    case Tilt:
        // tiltElevation: 1 = upright, 0 = flat
        return 1.0 - KisPaintInformation::tiltElevation(info, 60.0, 60.0, true);
    case Velocity:
        return velocityFromKritaSpeed(info.drawingSpeed(), m_velocityFast, m_velocityKnee);
    case Random:
        return !info.isHoveringMode() && info.randomSource() ? info.randomSource()->generateNormalized() : 0.5;
    default:
        break;
    }
    if (info.isHoveringMode()) return 1.0;
    const qreal length = m_length[input];
    qreal position = 0.0;
    if (input == Fade) position = info.currentDabSeqNo();
    else if (input == Distance) position = info.totalStrokeLength();
    else position = info.currentTime();
    return (m_periodic[input] ? std::fmod(position, length) : qMin(position, length)) / length;
}

qreal CspEffector::value(const KisPaintInformation &info) const
{
    qreal result = 1.0;
    for (int i = 0; i < InputCount; i++) {
        if (!m_enabled[i]) continue;
        const qreal x = qBound(0.0, input(Input(i), info), 1.0);
        const qreal y = m_transfer[i].isEmpty() ? x : KisCubicCurve::interpolateLinear(x, m_transfer[i]);
        result *= m_min[i] + (1.0 - m_min[i]) * qBound(0.0, y, 1.0);
    }
    return result;
}
