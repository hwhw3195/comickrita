/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PRIMITIVE_H
#define VSTROKE_PRIMITIVE_H

#include <QPointF>
#include <QTransform>
#include <QVector>

#include <brushengine/kis_paint_information.h>

#include "kritavstroke_export.h"

class QDomDocument;
class QDomElement;

/**
 * One painting call of a freehand stroke, exactly as KisToolFreehandHelper
 * sent it to FreehandStrokeStrategy (POINT, LINE or CURVE). A vector stroke
 * stores these instead of a resampled polyline, so replaying them through
 * VStrokePainter reproduces the live stroke.
 *
 * Positions are in image pixels.
 */
struct KRITAVSTROKE_EXPORT VStrokePrimitive
{
    enum Type {
        Point,
        Line,
        Curve
    };

    Type type = Point;
    KisPaintInformation pi1;
    KisPaintInformation pi2;     ///< Line and Curve only
    QPointF control1;            ///< Curve only
    QPointF control2;            ///< Curve only
    /// not saved: the stroke starts over here, the first primitive of a sub-path away from the one before
    bool restartsStroke = false;

    static VStrokePrimitive point(const KisPaintInformation &pi);
    static VStrokePrimitive line(const KisPaintInformation &pi1, const KisPaintInformation &pi2);
    static VStrokePrimitive curve(const KisPaintInformation &pi1, const QPointF &control1,
                                  const QPointF &control2, const KisPaintInformation &pi2);

    /// all positions of the primitive, including control points
    QVector<QPointF> positions() const;

    /// a copy with all positions mapped by \p t; pressure and the rest stay
    VStrokePrimitive mapped(const QTransform &t) const;
    static QVector<VStrokePrimitive> mapped(const QVector<VStrokePrimitive> &primitives, const QTransform &t);

    void toXML(QDomDocument &doc, QDomElement &e) const;
    static VStrokePrimitive fromXML(const QDomElement &e);
};

#endif // VSTROKE_PRIMITIVE_H
