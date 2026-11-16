/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VGEOM_CURVE_SHAPE_H
#define VGEOM_CURVE_SHAPE_H

#include <KoShapeFactoryBase.h>

#include "VCurveShape.h"
#include "kritavstroke_export.h"

#define VGeomCurveShapeId "VGeomCurveShape"

/**
 * A geometric line on a freehand vector layer: a box, ellipse, polyline or
 * path converted from a stock path (VCurveConverter), with a radius per
 * point.
 *
 * The line is the outline from position and radius, filled with the color of
 * the shape's KoShapeStroke, whose join, cap and miter limit it follows. The
 * fill is the shape's background over VCurveOutline::fill(), so a shape cut
 * open keeps its fill. Positions and radii are in shape-local units: the
 * line width scales with the shape's transformation, as a stock path's
 * does. Changing the stroke width scales every radius.
 *
 * Saved as an SVG <path> whose "d" is the centerline with the stock style,
 * so other readers (and stock Krita) show the shape with a constant width;
 * the curves are a child element they ignore.
 */
class KRITAVSTROKE_EXPORT VGeomCurveShape : public VCurveShape
{
public:
    VGeomCurveShape();
    ~VGeomCurveShape() override;

    KoShape *cloneShape() const override;

    void paint(QPainter &painter) const override;
    /// the line is painted by paint()
    void paintStroke(QPainter &painter) const override;

    /// the line has no dashes or markers
    Features strokeFeatures() const override;

    QRectF boundingRect() const override;

    bool saveSvg(SvgSavingContext &context) override;
    bool loadSvg(const QDomElement &element, SvgLoadingContext &context) override;

    Qt::FillRule fillRule() const;
    void setFillRule(Qt::FillRule fillRule);

    /// the color of the line: the color of the KoShapeStroke
    QColor lineColor() const;

    /// SVG path data of the centerline, mapped by \p t
    QString centerlinePathData(const QTransform &t) const;

protected:
    void shapeChanged(ChangeType type, KoShape *shape = 0) override;

    QPainterPath buildOutline(qreal radiusScale) const override;
    VCurveStrokeStyle strokeStyle() const override;

private:
    VGeomCurveShape(const VGeomCurveShape &rhs);

    /// takes the join, cap and width of the stroke; a new width scales the radii
    void syncStroke();
    /// the line faded by its per-point opacity (VCurveOpacity)
    void paintLineWithOpacity(QPainter &painter, const QColor &color) const;

private:
    Qt::FillRule m_fillRule = Qt::WindingFill;
    VCurveStrokeStyle m_style;
    qreal m_lineWidth = 0.0;    ///< the stroke width the radii were last scaled to
};

class KRITAVSTROKE_EXPORT VGeomCurveShapeFactory : public KoShapeFactoryBase
{
public:
    VGeomCurveShapeFactory();

    KoShape *createDefaultShape(KoDocumentResourceManager *documentResources = 0) const override;
    bool supports(const QDomElement &element, KoShapeLoadingContext &context) const override;
};

#endif // VGEOM_CURVE_SHAPE_H
