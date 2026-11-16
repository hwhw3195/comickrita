/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokePrimitive.h"

#include <QDomDocument>
#include <QDomElement>

namespace {
const char *typeName(VStrokePrimitive::Type type)
{
    switch (type) {
    case VStrokePrimitive::Line: return "line";
    case VStrokePrimitive::Curve: return "curve";
    case VStrokePrimitive::Point: break;
    }
    return "point";
}

void writePaintInfo(QDomDocument &doc, QDomElement &parent, const QString &tag, const KisPaintInformation &pi)
{
    QDomElement e = doc.createElement(tag);
    pi.toXML(doc, e);
    // not saved by KisPaintInformation, but sensors may read them
    e.setAttribute("canvasRotation", QString::number(pi.canvasRotation(), 'g', 15));
    e.setAttribute("canvasMirroredH", pi.canvasMirroredH() ? 1 : 0);
    e.setAttribute("canvasMirroredV", pi.canvasMirroredV() ? 1 : 0);
    e.setAttribute("tiltDirectionOffset", QString::number(pi.tiltDirectionOffset(), 'g', 15));
    parent.appendChild(e);
}

KisPaintInformation readPaintInfo(const QDomElement &e)
{
    KisPaintInformation pi = KisPaintInformation::fromXML(e);
    pi.setCanvasRotation(e.attribute("canvasRotation", "0").toDouble());
    pi.setCanvasMirroredH(e.attribute("canvasMirroredH", "0").toInt());
    pi.setCanvasMirroredV(e.attribute("canvasMirroredV", "0").toInt());
    pi.setTiltDirectionOffset(e.attribute("tiltDirectionOffset", "0").toDouble());
    return pi;
}

void writePoint(QDomElement &e, const QString &prefix, const QPointF &pt)
{
    e.setAttribute(prefix + "X", QString::number(pt.x(), 'g', 17));
    e.setAttribute(prefix + "Y", QString::number(pt.y(), 'g', 17));
}

QPointF readPoint(const QDomElement &e, const QString &prefix)
{
    return QPointF(e.attribute(prefix + "X").toDouble(), e.attribute(prefix + "Y").toDouble());
}
}

VStrokePrimitive VStrokePrimitive::point(const KisPaintInformation &pi)
{
    VStrokePrimitive p;
    p.type = Point;
    p.pi1 = pi;
    return p;
}

VStrokePrimitive VStrokePrimitive::line(const KisPaintInformation &pi1, const KisPaintInformation &pi2)
{
    VStrokePrimitive p;
    p.type = Line;
    p.pi1 = pi1;
    p.pi2 = pi2;
    return p;
}

VStrokePrimitive VStrokePrimitive::curve(const KisPaintInformation &pi1, const QPointF &control1,
                                         const QPointF &control2, const KisPaintInformation &pi2)
{
    VStrokePrimitive p;
    p.type = Curve;
    p.pi1 = pi1;
    p.pi2 = pi2;
    p.control1 = control1;
    p.control2 = control2;
    return p;
}

QVector<QPointF> VStrokePrimitive::positions() const
{
    switch (type) {
    case Line: return {pi1.pos(), pi2.pos()};
    case Curve: return {pi1.pos(), control1, control2, pi2.pos()};
    case Point: break;
    }
    return {pi1.pos()};
}

VStrokePrimitive VStrokePrimitive::mapped(const QTransform &t) const
{
    VStrokePrimitive p = *this;
    p.pi1.setPos(t.map(pi1.pos()));
    if (type != Point) {
        p.pi2.setPos(t.map(pi2.pos()));
    }
    if (type == Curve) {
        p.control1 = t.map(control1);
        p.control2 = t.map(control2);
    }
    return p;
}

QVector<VStrokePrimitive> VStrokePrimitive::mapped(const QVector<VStrokePrimitive> &primitives, const QTransform &t)
{
    QVector<VStrokePrimitive> result;
    result.reserve(primitives.size());
    Q_FOREACH (const VStrokePrimitive &p, primitives) {
        result << p.mapped(t);
    }
    return result;
}

void VStrokePrimitive::toXML(QDomDocument &doc, QDomElement &e) const
{
    e.setAttribute("type", typeName(type));
    writePaintInfo(doc, e, "pi1", pi1);
    if (type != Point) {
        writePaintInfo(doc, e, "pi2", pi2);
    }
    if (type == Curve) {
        writePoint(e, "c1", control1);
        writePoint(e, "c2", control2);
    }
}

VStrokePrimitive VStrokePrimitive::fromXML(const QDomElement &e)
{
    VStrokePrimitive p;
    const QString type = e.attribute("type");
    p.type = type == "line" ? Line : type == "curve" ? Curve : Point;
    p.pi1 = readPaintInfo(e.firstChildElement("pi1"));
    if (p.type != Point) {
        p.pi2 = readPaintInfo(e.firstChildElement("pi2"));
    }
    if (p.type == Curve) {
        p.control1 = readPoint(e, "c1");
        p.control2 = readPoint(e, "c2");
    }
    return p;
}
