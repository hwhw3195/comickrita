/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveConverter.h"

#include <QBrush>

#include <kis_pointer_utils.h>

#include <KoColorBackground.h>
#include <KoPathPoint.h>
#include <KoPathShape.h>
#include <KoShapeStroke.h>

#include "VGeomCurveShape.h"

namespace {

QPointF control1(const KoPathPoint *p)
{
    return p->activeControlPoint2() ? p->controlPoint2() : p->point();
}

QPointF control2(const KoPathPoint *p)
{
    return p->activeControlPoint1() ? p->controlPoint1() : p->point();
}

bool isCurve(const KoPathPoint *from, const KoPathPoint *to)
{
    return from->activeControlPoint2() || to->activeControlPoint1();
}

}

bool VCurveConverter::canConvert(const KoShape *shape)
{
    const KoPathShape *path = dynamic_cast<const KoPathShape*>(shape);
    if (!path || path->hasMarkers() || path->pointCount() == 0) return false;

    if (path->background() && !qSharedPointerDynamicCast<KoColorBackground>(path->background())) {
        return false;
    }

    if (KoShapeStrokeModelSP model = path->stroke()) {
        const QSharedPointer<KoShapeStroke> stroke = qSharedPointerDynamicCast<KoShapeStroke>(model);
        if (!stroke) return false;
        if (stroke->isVisible() && stroke->lineStyle() != Qt::SolidLine) return false;
        if (stroke->lineBrush().gradient()) return false;
    }
    return true;
}

VGeomCurveShape *VCurveConverter::fromPath(const KoPathShape *path)
{
    if (!canConvert(path)) return nullptr;

    const QSharedPointer<KoShapeStroke> stroke = qSharedPointerDynamicCast<KoShapeStroke>(path->stroke());
    const qreal radius = stroke && stroke->isVisible() ? 0.5 * stroke->lineWidth() : 0.0;

    VCurveData d;
    for (int s = 0; s < path->subpathCount(); s++) {
        const int count = path->subpathPointCount(s);
        if (count <= 0) continue;
        const bool closed = path->isClosedSubpath(s);
        d.beginCurve(closed);

        for (int i = 0; i < count; i++) {
            d.addPoint(path->pointByIndex(KoPathPointIndex(s, i))->point(), radius);
        }
        const int begin = d.curveBegin(d.curveCount() - 1);
        const int segments = closed ? count : count - 1;
        for (int i = 0; i < segments; i++) {
            const KoPathPoint *from = path->pointByIndex(KoPathPointIndex(s, i));
            const KoPathPoint *to = path->pointByIndex(KoPathPointIndex(s, (i + 1) % count));
            if (isCurve(from, to)) {
                d.setCurveSegment(begin + i, control1(from), control2(to));
            }
        }
    }
    if (d.isEmpty()) return nullptr;

    VGeomCurveShape *shape = new VGeomCurveShape();
    shape->setCurves(d);
    shape->setFillRule(path->fillRule());
    shape->setTransformation(path->transformation());
    shape->setBackground(path->background());
    if (stroke) {
        // a copy: the shapes must not share a stroke the user edits on one of them
        shape->setStroke(toQShared(new KoShapeStroke(*stroke)));
    }
    shape->setZIndex(path->zIndex());
    shape->setName(path->name());
    shape->setVisible(path->isVisible());
    shape->setTransparency(path->transparency());
    shape->setKeepAspectRatio(path->keepAspectRatio());
    shape->setGeometryProtected(path->isGeometryProtected());
    shape->setContentProtected(path->isContentProtected());
    shape->setSelectable(path->isSelectable());
    return shape;
}

bool VCurveConverter::convertInPlace(QList<KoShape*> &shapes)
{
    bool changed = false;
    for (KoShape *&shape : shapes) {
        if (VGeomCurveShape *curve = fromPath(dynamic_cast<KoPathShape*>(shape))) {
            delete shape;
            shape = curve;
            changed = true;
        }
    }
    return changed;
}
