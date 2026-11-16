/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VGeomCurveShape.h"

#include <QDomDocument>
#include <QPainter>

#include <KoShapeBackground.h>
#include <KoShapeStroke.h>
#include <KoXmlNS.h>
#include <KoXmlWriter.h>
#include <SvgLoadingContext.h>
#include <SvgSavingContext.h>
#include <SvgStyleWriter.h>
#include <SvgUtil.h>
#include <klocalizedstring.h>

#include "VCurveOpacity.h"

namespace {

QString number(qreal value)
{
    return QString::number(value, 'g', 12);
}

QString point(const QPointF &pt)
{
    return number(pt.x()) + "," + number(pt.y());
}

}

VGeomCurveShape::VGeomCurveShape()
{
    setShapeId(VGeomCurveShapeId);
}

VGeomCurveShape::VGeomCurveShape(const VGeomCurveShape &rhs)
    : VCurveShape(rhs),
      m_fillRule(rhs.m_fillRule),
      m_style(rhs.m_style),
      m_lineWidth(rhs.m_lineWidth)
{
}

VGeomCurveShape::~VGeomCurveShape()
{
}

KoShape *VGeomCurveShape::cloneShape() const
{
    return new VGeomCurveShape(*this);
}

QColor VGeomCurveShape::lineColor() const
{
    if (QSharedPointer<KoShapeStroke> s = qSharedPointerDynamicCast<KoShapeStroke>(stroke())) {
        // "no line" is an invalid color, which still has an alpha and would paint black
        if (s->color().isValid()) return s->color();
    }
    return Qt::transparent;
}

void VGeomCurveShape::paint(QPainter &painter) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (background()) {
        background()->paint(painter, VCurveOutline::fill(curves(), m_fillRule));
    }
    const QColor color = lineColor();
    if (color.alpha() > 0 && m_lineWidth > 0) {
        if (VCurveOpacity::isVarying(curves())) {
            paintLineWithOpacity(painter, color);
        } else {
            painter.fillPath(lineOutline(1.0), color);
        }
    }
    painter.restore();
}

void VGeomCurveShape::paintLineWithOpacity(QPainter &painter, const QColor &color) const
{
    // the line in device pixels, faded point by point, then drawn 1:1
    const QPainterPath line = lineOutline(1.0);
    const QTransform toDevice = painter.transform();
    QRect rect = toDevice.mapRect(line.boundingRect()).toAlignedRect().adjusted(-1, -1, 1, 1);
    if (painter.hasClipping()) {
        rect &= toDevice.mapRect(painter.clipBoundingRect()).toAlignedRect().adjusted(-1, -1, 1, 1);
    }
    if (painter.device()) {
        rect &= QRect(0, 0, painter.device()->width(), painter.device()->height());
    }
    if (rect.isEmpty()) return;

    QImage image(rect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setTransform(toDevice * QTransform::fromTranslate(-rect.x(), -rect.y()));
        p.fillPath(line, color);
    }
    VCurveOpacity::applyMask(image, rect.topLeft(), curves(), toDevice);

    painter.save();
    painter.resetTransform();
    painter.drawImage(rect.topLeft(), image);
    painter.restore();
}

void VGeomCurveShape::paintStroke(QPainter &painter) const
{
    Q_UNUSED(painter);
}

KoStrokeFeatures::Features VGeomCurveShape::strokeFeatures() const
{
    return Width | CapAndJoin;
}

QPainterPath VGeomCurveShape::buildOutline(qreal radiusScale) const
{
    // what a click selects: the line, and the fill when there is one
    QPainterPath line = m_lineWidth > 0 ? lineOutline(radiusScale) : QPainterPath();
    // with no line and no fill it can still be selected, and a file's parser drops shapes without an outline
    if (!background()) return line.isEmpty() ? lineOutline(radiusScale) : line;
    QPainterPath area = VCurveOutline::fill(curves(), m_fillRule);
    return line.isEmpty() ? area : line.united(area);
}

QRectF VGeomCurveShape::boundingRect() const
{
    return absoluteTransformation().mapRect(outlineRect());
}

VCurveStrokeStyle VGeomCurveShape::strokeStyle() const
{
    return m_style;
}

Qt::FillRule VGeomCurveShape::fillRule() const
{
    return m_fillRule;
}

void VGeomCurveShape::setFillRule(Qt::FillRule fillRule)
{
    m_fillRule = fillRule;
    invalidateOutline();
}

void VGeomCurveShape::shapeChanged(ChangeType type, KoShape *shape)
{
    VCurveShape::shapeChanged(type, shape);
    if (shape) return;

    if (type == StrokeChanged) {
        syncStroke();
    } else if (type == BackgroundChanged) {
        invalidateOutline();
    }
}

void VGeomCurveShape::syncStroke()
{
    const QSharedPointer<KoShapeStroke> s = qSharedPointerDynamicCast<KoShapeStroke>(stroke());
    if (s) {
        m_style.join = s->joinStyle();
        m_style.cap = s->capStyle();
        m_style.miterLimit = s->miterLimit();
    }

    const qreal width = s && s->isVisible() ? s->lineWidth() : 0.0;
    if (m_lineWidth > 0 && width > 0 && width != m_lineWidth) {
        VCurveData &d = curvesForWrite();
        for (qreal &r : d.radii) r *= width / m_lineWidth;
        d.clampRadii();
    }
    // a hidden stroke hides the line; shown again it keeps the radii
    m_lineWidth = width;
    invalidateOutline();
}

QString VGeomCurveShape::centerlinePathData(const QTransform &t) const
{
    const VCurveData &d = curves();
    QString path;
    for (int c = 0; c < d.curveCount(); c++) {
        const int begin = d.curveBegin(c);
        const int end = d.curveEnd(c);
        path += "M" + point(t.map(d.positions[begin])) + " ";
        const int last = d.isCyclic(c) ? end : end - 1;
        for (int i = begin; i < last; i++) {
            const int next = i + 1 < end ? i + 1 : begin;
            if (d.segment(i) == VCurveData::CurveSegment) {
                path += "C" + point(t.map(d.handlesOut[i])) + " " + point(t.map(d.handlesIn[next])) + " " +
                        point(t.map(d.positions[next])) + " ";
            } else {
                path += "L" + point(t.map(d.positions[next])) + " ";
            }
        }
        if (d.isCyclic(c)) path += "Z ";
    }
    return path.trimmed();
}

bool VGeomCurveShape::saveSvg(SvgSavingContext &context)
{
    KoXmlWriter &writer = context.shapeWriter();
    const QTransform userSpace = context.userSpaceTransform();

    writer.startElement("path");
    writer.addAttribute("id", context.getID(this));
    SvgUtil::writeTransformAttributeLazy("transform", transformation(), writer);
    SvgStyleWriter::saveSvgStyle(this, context);
    if (m_fillRule == Qt::OddEvenFill) {
        writer.addAttribute("fill-rule", "evenodd");
    }
    writer.addAttribute("d", centerlinePathData(userSpace));
    writer.addAttribute("krita:type", "vcurve");
    writer.addAttribute("krita:vcurve-version", 1);
    SvgStyleWriter::saveMetadata(this, context);

    QDomDocument doc;
    QDomElement data = doc.createElement(VCurveData::XmlElement);
    curves().toXML(doc, data, userSpace);
    writeDomElement(writer, data);

    writer.endElement();
    return true;
}

bool VGeomCurveShape::loadSvg(const QDomElement &element, SvgLoadingContext &context)
{
    Q_UNUSED(context);
    if (element.tagName() != "path" || element.attribute("krita:type") != "vcurve") {
        return false;
    }

    VCurveData curves;
    if (!VCurveData::fromXML(element.firstChildElement(VCurveData::XmlElement), &curves)) {
        return false;
    }
    m_fillRule = element.attribute("fill-rule") == "evenodd" ? Qt::OddEvenFill : Qt::WindingFill;
    // the radii are as saved: the stroke the parser applies next must not rescale them
    m_lineWidth = 0.0;
    setCurves(curves);
    return true;
}

VGeomCurveShapeFactory::VGeomCurveShapeFactory()
    : KoShapeFactoryBase(VGeomCurveShapeId, i18n("Freehand Vector Curve"))
{
    setXmlElementNames(KoXmlNS::svg, QStringList("path"));
    setHidden(true); // made from stock paths on freehand vector layers
}

KoShape *VGeomCurveShapeFactory::createDefaultShape(KoDocumentResourceManager *documentResources) const
{
    Q_UNUSED(documentResources);
    return new VGeomCurveShape();
}

bool VGeomCurveShapeFactory::supports(const QDomElement &element, KoShapeLoadingContext &context) const
{
    Q_UNUSED(context);
    return element.tagName() == "path" && element.attribute("krita:type") == "vcurve";
}
