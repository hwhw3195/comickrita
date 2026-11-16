/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeCurves.h"

#include <kis_algebra_2d.h>

const QString VStrokeCurves::DensityScale = QStringLiteral("density-scale");
const QString VStrokeCurves::DensityOffset = QStringLiteral("density-offset");

namespace {

/// a sample value saved per point, with the default of KisPaintInformation
struct Attribute {
    const char *name;
    qreal defaultValue;
    qreal (*get)(const KisPaintInformation &);
    void (*set)(KisPaintInformation &, qreal);
};

const Attribute Attributes[] = {
    {"pressure", 1.0,
     [](const KisPaintInformation &pi) { return pi.pressure(); },
     [](KisPaintInformation &pi, qreal v) { pi.setPressure(v); }},
    {"x-tilt", 0.0,
     [](const KisPaintInformation &pi) { return pi.xTilt(); },
     [](KisPaintInformation &, qreal) {}},
    {"y-tilt", 0.0,
     [](const KisPaintInformation &pi) { return pi.yTilt(); },
     [](KisPaintInformation &, qreal) {}},
    {"rotation", 0.0,
     [](const KisPaintInformation &pi) { return pi.rotation(); },
     [](KisPaintInformation &, qreal) {}},
    {"tangential-pressure", 0.0,
     [](const KisPaintInformation &pi) { return pi.tangentialPressure(); },
     [](KisPaintInformation &, qreal) {}},
    {"perspective", 1.0,
     [](const KisPaintInformation &pi) { return pi.perspective(); },
     [](KisPaintInformation &, qreal) {}},
    {"time", 0.0,
     [](const KisPaintInformation &pi) { return pi.currentTime(); },
     [](KisPaintInformation &pi, qreal v) { pi.setCurrentTime(v); }},
    {"speed", 0.0,
     [](const KisPaintInformation &pi) { return pi.drawingSpeed(); },
     [](KisPaintInformation &, qreal) {}},
    {"canvas-rotation", 0.0,
     [](const KisPaintInformation &pi) { return pi.canvasRotation(); },
     [](KisPaintInformation &pi, qreal v) { pi.setCanvasRotation(v); }},
    {"canvas-mirrored-h", 0.0,
     [](const KisPaintInformation &pi) { return qreal(pi.canvasMirroredH()); },
     [](KisPaintInformation &pi, qreal v) { pi.setCanvasMirroredH(v != 0); }},
    {"canvas-mirrored-v", 0.0,
     [](const KisPaintInformation &pi) { return qreal(pi.canvasMirroredV()); },
     [](KisPaintInformation &pi, qreal v) { pi.setCanvasMirroredV(v != 0); }},
    {"tilt-direction-offset", 0.0,
     [](const KisPaintInformation &pi) { return pi.tiltDirectionOffset(); },
     [](KisPaintInformation &pi, qreal v) { pi.setTiltDirectionOffset(v); }},
};

/// replays the same as \p b: every value the stroke keeps is equal
bool samePoint(const KisPaintInformation &a, const KisPaintInformation &b)
{
    if (a.pos() != b.pos()) return false;
    for (const Attribute &attribute : Attributes) {
        if (attribute.get(a) != attribute.get(b)) return false;
    }
    return true;
}

int addSample(VCurveData &d, const KisPaintInformation &pi, qreal brushSize)
{
    const int index = d.addPoint(pi.pos(), VStrokeCurves::baseRadius(brushSize, pi.pressure()));
    for (const Attribute &attribute : Attributes) {
        d.setAttribute(attribute.name, index, attribute.get(pi), attribute.defaultValue);
    }
    return index;
}

}

qreal VStrokeCurves::baseRadius(qreal brushSize, qreal pressure)
{
    return 0.5 * brushSize * pressure;
}

VCurveData VStrokeCurves::fromPrimitives(const QVector<VStrokePrimitive> &primitives, qreal brushSize)
{
    VCurveData d;
    // the sample the last primitive ended at (the last point), if any
    bool haveLast = false;
    KisPaintInformation last;

    for (const VStrokePrimitive &p : primitives) {
        const bool continues = haveLast && samePoint(p.pi1, last);

        if (p.type == VStrokePrimitive::Point) {
            if (!continues || d.hasFlag(d.pointCount() - 1, VCurveData::Dab)) {
                d.beginCurve();
                addSample(d, p.pi1, brushSize);
            }
            d.setFlag(d.pointCount() - 1, VCurveData::Dab);
            last = p.pi1;
            haveLast = true;
            continue;
        }

        if (!continues) {
            d.beginCurve();
            addSample(d, p.pi1, brushSize);
        }
        const int from = d.pointCount() - 1;
        addSample(d, p.pi2, brushSize);
        if (p.type == VStrokePrimitive::Curve) {
            d.setCurveSegment(from, p.control1, p.control2);
        }
        last = p.pi2;
        haveLast = true;
    }
    return d;
}

KisPaintInformation VStrokeCurves::paintInfo(const VCurveData &d, int i, qreal brushSize, const QTransform &t)
{
    auto value = [&](const Attribute &a) {
        return d.attribute(a.name, i, a.defaultValue);
    };

    KisPaintInformation pi(t.map(d.positions[i]),
                           value(Attributes[0]),   // pressure
                           value(Attributes[1]),   // x tilt
                           value(Attributes[2]),   // y tilt
                           value(Attributes[3]),   // rotation
                           value(Attributes[4]),   // tangential pressure
                           value(Attributes[5]),   // perspective
                           value(Attributes[6]),   // time
                           value(Attributes[7]));  // speed
    for (int k = 8; k < int(sizeof(Attributes) / sizeof(Attributes[0])); k++) {
        Attributes[k].set(pi, value(Attributes[k]));
    }

    // a radius not edited (or a brush size read back from a file with
    // rounding) leaves the multiplier at exactly 1
    const qreal base = baseRadius(brushSize, pi.pressure());
    if (base > 0 && qAbs(d.radii[i] / base - 1.0) > 1e-9) {
        pi.setSizeMultiplier(d.radii[i] / base);
    }
    const qreal densityScale = d.attribute(DensityScale, i, 1.0);
    const qreal densityOffset = d.attribute(DensityOffset, i, 0.0);
    if (densityScale != 1.0 || densityOffset != 0.0) {
        pi.setDensity(densityScale, densityOffset);
    }
    return pi;
}

namespace {

/// whether the sub-path starting at \p begin starts inside the ink where the one before ends
bool continuesStroke(const VCurveData &d, int begin, const QTransform &t)
{
    if (d.positions[begin] == d.positions[begin - 1]) return true;
    // in image pixels, where the radii are; a trim's gap is wider than the ink
    const qreal gap = KisAlgebra2D::norm(t.map(d.positions[begin]) - t.map(d.positions[begin - 1]));
    return gap <= qMax(1.0, d.radii[begin - 1]);
}

}

QVector<VStrokePrimitive> VStrokeCurves::toPrimitives(const VCurveData &d, qreal brushSize, const QTransform &t)
{
    QVector<VStrokePrimitive> primitives;
    primitives.reserve(d.pointCount() + d.flags.count(VCurveData::Dab));

    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int end = d.curveEnd(c);
        KisPaintInformation previous;
        // repeated dabs in one place are sub-paths too, but the stroke goes
        // on, and so do the sub-paths the freehand helper starts a hair away
        // (pen jitter at a lift steps back to an earlier sample): inside the
        // ink there, the live stroke was one paintop
        bool restart = c > 0 && begin < end && !continuesStroke(d, begin, t);
        const int primitivesBefore = primitives.size();
        for (int i = begin; i < end; i++) {
            const KisPaintInformation pi = paintInfo(d, i, brushSize, t);
            if (i > begin) {
                if (d.segment(i - 1) == VCurveData::CurveSegment) {
                    primitives << VStrokePrimitive::curve(previous, t.map(d.handlesOut[i - 1]),
                                                          t.map(d.handlesIn[i]), pi);
                } else {
                    primitives << VStrokePrimitive::line(previous, pi);
                }
            }
            if (d.hasFlag(i, VCurveData::Dab)) {
                primitives << VStrokePrimitive::point(pi);
            }
            if (restart && primitives.size() > primitivesBefore) {
                primitives[primitivesBefore].restartsStroke = true;
                restart = false;
            }
            previous = pi;
        }
    }
    return primitives;
}
