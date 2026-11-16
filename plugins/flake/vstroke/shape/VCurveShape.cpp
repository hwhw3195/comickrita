/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveShape.h"

#include <QDomElement>

#include <KoXmlWriter.h>

VCurveShape::VCurveShape()
    : m_curves(new VCurveData)
{
}

VCurveShape::VCurveShape(const VCurveShape &rhs)
    : KoShape(rhs),
      SvgShape(rhs),
      m_curves(rhs.m_curves),
      m_outline(rhs.m_outline),
      m_outlineRadiusScale(rhs.m_outlineRadiusScale)
{
}

VCurveShape::~VCurveShape()
{
}

const VCurveData &VCurveShape::curves() const
{
    return *m_curves;
}

VCurveData &VCurveShape::curvesForWrite()
{
    return *m_curves;
}

void VCurveShape::setCurves(const VCurveData &curves)
{
    *m_curves = curves;
    // every tool, transform, paste and load writes through here
    m_curves->clampRadii();
    curvesChanged();
}

void VCurveShape::setCurvesAndUpdate(const VCurveData &curves)
{
    const QRectF before = boundingRect();
    setCurves(curves);
    updateAbsolute(before);
    update();
}

void VCurveShape::curvesChanged()
{
    invalidateOutline();
    const QSizeF oldSize = size();
    KoShape::setSize(m_curves->controlBounds().size());
    // KoShape::setSize() tells the shape managers only when the size changes.
    // Curves moved within the same size (a Perspective or Edit Shapes move)
    // still move the bounds, and the managers find the shapes to paint in a
    // tile by the bounds they last indexed: the shape would vanish from the
    // tiles at its new place
    if (size() == oldSize) notifyChanged();
}

void VCurveShape::normalizeOrigin()
{
    const QPointF origin = m_curves->controlBounds().topLeft();
    if (origin.isNull()) return;
    m_curves->transform(QTransform::fromTranslate(-origin.x(), -origin.y()));
    setTransformation(QTransform::fromTranslate(origin.x(), origin.y()) * transformation());
    curvesChanged();
}

void VCurveShape::setSize(const QSizeF &newSize)
{
    const QSizeF oldSize = size();
    const qreal sx = oldSize.width() > 0 ? newSize.width() / oldSize.width() : 1.0;
    const qreal sy = oldSize.height() > 0 ? newSize.height() / oldSize.height() : 1.0;

    m_curves->transform(QTransform::fromScale(sx, sy));
    invalidateOutline();
    KoShape::setSize(newSize);
}

QPainterPath VCurveShape::outline() const
{
    const qreal radiusScale = outlineRadiusScale();
    if (radiusScale <= 0) return QPainterPath();

    if (!qFuzzyCompare(radiusScale, m_outlineRadiusScale)) {
        m_outline = buildOutline(radiusScale);
        m_outlineRadiusScale = radiusScale;
    }
    return m_outline;
}

QPainterPath VCurveShape::buildOutline(qreal radiusScale) const
{
    return lineOutline(radiusScale);
}

QPainterPath VCurveShape::lineOutline(qreal radiusScale) const
{
    return VCurveOutline::line(*m_curves, strokeStyle(), radiusScale, outlineMinRadius(), outlineTolerance());
}

QRectF VCurveShape::outlineRect() const
{
    return outline().boundingRect();
}

void VCurveShape::invalidateOutline()
{
    m_outlineRadiusScale = -1.0;
}

VCurveStrokeStyle VCurveShape::strokeStyle() const
{
    return VCurveStrokeStyle();
}

qreal VCurveShape::outlineRadiusScale() const
{
    return 1.0;
}

qreal VCurveShape::outlineMinRadius() const
{
    return 0.0;
}

qreal VCurveShape::outlineTolerance() const
{
    return 0.1;
}

void VCurveShape::writeDomElement(KoXmlWriter &writer, const QDomElement &e)
{
    const QByteArray tag = e.tagName().toUtf8();
    writer.startElement(tag.constData());

    const QDomNamedNodeMap attributes = e.attributes();
    for (int i = 0; i < attributes.count(); i++) {
        const QDomAttr attr = attributes.item(i).toAttr();
        const QByteArray name = attr.name().toUtf8();
        writer.addAttribute(name.constData(), attr.value());
    }
    for (QDomNode child = e.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.isElement()) {
            writeDomElement(writer, child.toElement());
        } else if (child.isText() || child.isCDATASection()) {
            // the writer does not indent inside text
            writer.addTextNode(child.nodeValue());
        }
    }
    writer.endElement();
}

QString VCurveShape::pathData(const QPainterPath &path, const QTransform &t)
{
    QString d;
    for (int i = 0; i < path.elementCount(); i++) {
        const QPainterPath::Element e = path.elementAt(i);
        const QPointF pt = t.map(QPointF(e.x, e.y));
        if (e.isMoveTo()) {
            if (i > 0) d += "Z ";
            d += "M";
        } else {
            d += "L";
        }
        d += QString::number(pt.x(), 'g', 8) + "," + QString::number(pt.y(), 'g', 8) + " ";
    }
    if (!d.isEmpty()) d += "Z";
    return d;
}
