/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_SHAPE_H
#define VCURVE_SHAPE_H

#include <QPainterPath>
#include <QSharedDataPointer>

#include <KoShape.h>
#include <KoStrokeFeatures.h>
#include <SvgShape.h>

#include "VCurveData.h"
#include "VCurveOutline.h"
#include "kritavstroke_export.h"

class KoXmlWriter;
class QDomElement;

/**
 * An object on a freehand vector layer: curves stored as per-point arrays
 * with a radius (VCurveData), shared copy-on-write between the shape and
 * its clones. The positions are in shape-local coordinates.
 *
 * The outline (what hit tests, selection and other SVG readers use) is the
 * line built from position and radius. Subclasses say how radii map to
 * local units and how corners and ends look.
 */
class KRITAVSTROKE_EXPORT VCurveShape : public KoShape, public SvgShape, public KoStrokeFeatures
{
public:
    ~VCurveShape() override;

    const VCurveData &curves() const;

    /// replaces the curves (in local coordinates) and updates the size
    void setCurves(const VCurveData &curves);

    /**
     * Replaces the curves and repaints where the shape was and where it is.
     * Use this instead of update(), setCurves(), update(): an update() may
     * start the layer's repaint at once, and a repaint started before the
     * change would paint the old curves where nothing repaints them again.
     */
    void setCurvesAndUpdate(const VCurveData &curves);

    /// scales positions and handles; radii keep their value, as stock paths keep their line width
    void setSize(const QSizeF &size) override;

    QPainterPath outline() const override;
    QRectF outlineRect() const override;

    /// writes \p e and its children with \p writer (KoXmlWriter needs tag names to outlive endElement())
    static void writeDomElement(KoXmlWriter &writer, const QDomElement &e);

    /// SVG path data of a polygonal \p path mapped by \p t
    static QString pathData(const QPainterPath &path, const QTransform &t);

protected:
    VCurveShape();
    VCurveShape(const VCurveShape &rhs);

    /// detaches the curves from clones; call curvesChanged() after editing
    VCurveData &curvesForWrite();

    /// recomputes the size and drops the outline after the curves changed
    virtual void curvesChanged();

    /**
     * Moves the local origin to the top left of the curves and compensates
     * the transformation, so the shape stays where it is.
     */
    void normalizeOrigin();

    /// what outline() returns, rebuilt when the curves or the radius scale change; the line by default
    virtual QPainterPath buildOutline(qreal radiusScale) const;
    /// the line from position and radius
    QPainterPath lineOutline(qreal radiusScale) const;

    virtual VCurveStrokeStyle strokeStyle() const;
    /// radius units per local unit
    virtual qreal outlineRadiusScale() const;
    /// in local units
    virtual qreal outlineMinRadius() const;
    /// bezier flattening tolerance, in local units
    virtual qreal outlineTolerance() const;

    void invalidateOutline();

private:
    QSharedDataPointer<VCurveData> m_curves;

    mutable QPainterPath m_outline;
    mutable qreal m_outlineRadiusScale = -1.0;
};

#endif // VCURVE_SHAPE_H
