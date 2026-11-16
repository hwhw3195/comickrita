/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeShape.h"
#include "VTrace.h"

#include <QHash>
#include <QDomDocument>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QtMath>

#include <algorithm>

#include <KisGlobalResourcesInterface.h>
#include <KisResourceTypes.h>
#include <KoColorBackground.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOp.h>
#include <KoShapeLayer.h>
#include <KoShapeStroke.h>
#include <KoXmlWriter.h>
#include <SvgLoadingContext.h>
#include <SvgSavingContext.h>
#include <SvgStyleWriter.h>
#include <SvgUtil.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_algebra_2d.h>
#include <kis_pointer_utils.h>

#include "VCurveOpacity.h"
#include "VStrokeCurves.h"
#include "VStrokeRenderCache.h"

namespace {

QAtomicInt s_liveShapes(0);
QAtomicInt s_outlinePaints(0);
QAtomicInt s_staleShapePaints(0);

quint64 nextRevision()
{
    static QAtomicInteger<quint64> revision(0);
    return ++revision;
}

/// tolerant: the canvas and the shape compute the resolution differently
bool isIntegerTranslation(const QTransform &t)
{
    const qreal eps = 1e-6;
    return qAbs(t.m11() - 1.0) < eps && qAbs(t.m22() - 1.0) < eps &&
           qAbs(t.m12()) < eps && qAbs(t.m21()) < eps &&
           qAbs(t.m13()) < eps && qAbs(t.m23()) < eps && qAbs(t.m33() - 1.0) < eps &&
           qAbs(t.dx() - qRound(t.dx())) < 1e-3 && qAbs(t.dy() - qRound(t.dy())) < 1e-3;
}
/**
 * The warped stand-in (VStrokeShape::paintWarped()): one sub-path as a
 * polyline in image pixels, beziers sampled at fixed parameters, so two
 * poses of the same curves give polylines point for point. A line that is
 * a bezier in \p other (the other pose) gets the same steps.
 */
QVector<QPointF> sampledCurve(const VCurveData &curves, int curve, const QTransform &toImage, const VCurveData &other)
{
    const int bezierSteps = 8;
    QVector<QPointF> result;
    for (int i = curves.curveBegin(curve); i < curves.curveEnd(curve); i++) {
        result << toImage.map(curves.positions[i]);
        const int next = curves.nextIndex(i);
        if (next < 0) continue;
        const bool bezier = curves.segment(i) == VCurveData::CurveSegment;
        if (!bezier && other.segment(i) != VCurveData::CurveSegment) continue;
        const QPointF p0 = curves.positions[i];
        const QPointF p1 = curves.positions[next];
        const QPointF c1 = bezier ? curves.handlesOut[i] : p0 + (p1 - p0) / 3;
        const QPointF c2 = bezier ? curves.handlesIn[next] : p1 + (p0 - p1) / 3;
        for (int k = 1; k < bezierSteps; k++) {
            const qreal t = qreal(k) / bezierSteps;
            const qreal u = 1.0 - t;
            result << toImage.map(u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p1);
        }
    }
    return result;
}

QPointF unitVector(const QPointF &direction, const QPointF &fallback)
{
    const qreal length = KisAlgebra2D::norm(direction);
    return length < 1e-9 ? fallback : direction / length;
}

QPointF normalOf(const QPointF &tangent)
{
    return QPointF(-tangent.y(), tangent.x());
}

/// the cross-sections of one pose: a left and a right end per section
struct Sections {
    QVector<QPointF> left;
    QVector<QPointF> right;
};

/**
 * Cross-sections of \p points at \p chosen (at least two), \p halfWidth
 * to each side along the averaged normal; for an open sub-path also one
 * more at each end, \p halfWidth beyond it, for the brush's caps.
 */
Sections crossSections(const QVector<QPointF> &points, const QVector<int> &chosen, bool cyclic, qreal halfWidth)
{
    Sections result;
    const int n = chosen.size();
    QPointF tangent(1, 0);
    for (int k = 0; k < n; k++) {
        const int prev = k > 0 ? chosen[k - 1] : (cyclic ? chosen[n - 1] : chosen[k]);
        const int next = k + 1 < n ? chosen[k + 1] : (cyclic ? chosen[0] : chosen[k]);
        tangent = unitVector(points[next] - points[prev], tangent);
        const QPointF p = points[chosen[k]];
        result.left << p + halfWidth * normalOf(tangent);
        result.right << p - halfWidth * normalOf(tangent);
    }
    if (!cyclic) {
        const QPointF startTangent = unitVector(points[chosen[1]] - points[chosen[0]], QPointF(1, 0));
        const QPointF endTangent = unitVector(points[chosen[n - 1]] - points[chosen[n - 2]], QPointF(1, 0));
        result.left.prepend(result.left.first() - halfWidth * startTangent);
        result.right.prepend(result.right.first() - halfWidth * startTangent);
        result.left << result.left.last() + halfWidth * endTangent;
        result.right << result.right.last() + halfWidth * endTangent;
    }
    return result;
}

/// the affine transform taking triangle \p from to triangle \p to; false for a degenerate \p from
bool triangleTransform(const QPointF from[3], const QPointF to[3], QTransform *result)
{
    const QPointF u = from[1] - from[0];
    const QPointF v = from[2] - from[0];
    const qreal det = u.x() * v.y() - u.y() * v.x();
    if (qAbs(det) < 1e-6) return false;
    const QPointF u2 = to[1] - to[0];
    const QPointF v2 = to[2] - to[0];
    // M maps u to u2 and v to v2: M = [u2 v2] [u v]^-1
    const qreal i11 = v.y() / det, i12 = -v.x() / det, i21 = -u.y() / det, i22 = u.x() / det;
    const qreal m11 = u2.x() * i11 + v2.x() * i21; // x' from x
    const qreal m21 = u2.x() * i12 + v2.x() * i22; // x' from y
    const qreal m12 = u2.y() * i11 + v2.y() * i21; // y' from x
    const qreal m22 = u2.y() * i12 + v2.y() * i22; // y' from y
    const qreal dx = to[0].x() - (m11 * from[0].x() + m21 * from[0].y());
    const qreal dy = to[0].y() - (m12 * from[0].x() + m22 * from[0].y());
    *result = QTransform(m11, m12, m21, m22, dx, dy);
    return true;
}

/// the same points in the same curves (a line may have become a bezier: sampledCurve() pairs them up)
bool samePoints(const VCurveData &a, const VCurveData &b)
{
    return a.pointCount() == b.pointCount() && a.curveOffsets == b.curveOffsets && a.cyclic == b.cyclic;
}

/// a move, turn or mirror, which keeps the brush's size
bool isRigid(const QTransform &t)
{
    const qreal eps = 1e-3;
    if (qAbs(t.m13()) > 1e-9 || qAbs(t.m23()) > 1e-9) return false;
    return qAbs(t.m11() * t.m11() + t.m12() * t.m12() - 1.0) < eps &&
           qAbs(t.m21() * t.m21() + t.m22() * t.m22() - 1.0) < eps &&
           qAbs(t.m11() * t.m21() + t.m12() * t.m22()) < eps;
}

qreal maxRadiusOf(const VCurveData &curves, int curve)
{
    qreal result = 0;
    for (int i = curves.curveBegin(curve); i < curves.curveEnd(curve); i++) {
        result = qMax(result, curves.radii.value(i));
    }
    return result;
}

bool samePoint(const QPointF &a, const QPointF &b)
{
    return qAbs(a.x() - b.x()) < 0.01 && qAbs(a.y() - b.y()) < 0.01;
}

KisPaintOpPresetSP findPreset(const QString &md5, const QString &fileName, const QString &name)
{
    if (md5.isEmpty() && fileName.isEmpty() && name.isEmpty()) return KisPaintOpPresetSP();

    auto source = KisGlobalResourcesInterface::instance()->source<KisPaintOpPreset>(ResourceType::PaintOpPresets);
    KisPaintOpPresetSP preset = source.bestMatch(md5, fileName, name);

    // render jobs create paintops in worker threads, so the linked
    // resources must be loaded here, as KisResourcesSnapshot does
    return preset ? preset->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(), nullptr, nullptr)
                  : preset;
}

}

VStrokeShape::VStrokeShape()
    : m_data(new VStrokeData),
      m_revision(nextRevision())
{
    m_cache.reset(new VStrokeRenderCache(this));
    m_cache->setCurrentRevision(m_revision);
    setShapeId(VStrokeShapeId);
    s_liveShapes.ref();
}

VStrokeShape::VStrokeShape(const VStrokeShape &rhs)
    : VCurveShape(rhs),
      m_data(rhs.m_data),
      m_pixelsPerPoint(rhs.m_pixelsPerPoint),
      m_revision(rhs.m_revision),
      m_renderedRect(rhs.m_renderedRect),
      m_cache(rhs.m_cache), // clones painted by KoShapeManager share the renderings
      m_revisionCurves(rhs.m_revisionCurves),
      m_kept(rhs.m_kept),
      m_motion(rhs.m_motion),
      m_settling(rhs.m_settling)
{
    s_liveShapes.ref();
}

VStrokeShape::~VStrokeShape()
{
    if (m_cache->owner() == this) {
        m_cache->setOwner(nullptr);
    }
    s_liveShapes.deref();
}

int VStrokeShape::liveCount()
{
    return s_liveShapes.loadAcquire();
}

int VStrokeShape::outlinePaints()
{
    return s_outlinePaints.loadAcquire();
}

VStrokeShape *VStrokeShape::fromImagePixels(const QVector<VStrokePrimitive> &primitives,
                                            qreal pixelsPerPoint,
                                            const VStrokeData &settings)
{
    VStrokeShape *shape = new VStrokeShape();
    shape->m_pixelsPerPoint = pixelsPerPoint;
    shape->m_data = new VStrokeData(settings);
    shape->m_data->presetDefinition = VStrokePresetStore::instance()->intern(settings.preset);
    if (shape->m_data->presetDefinition) {
        // strokes drawn with the same settings share one preset
        shape->m_data->preset = shape->m_data->presetDefinition->preset;
    }

    VCurveData curves = VStrokeCurves::fromPrimitives(primitives, settings.brushSize);
    curves.transform(QTransform::fromScale(1.0 / pixelsPerPoint, 1.0 / pixelsPerPoint));
    shape->setCurves(curves);
    shape->normalizeOrigin();
    return shape;
}

KoShape *VStrokeShape::cloneShape() const
{
    return new VStrokeShape(*this);
}

QColor VStrokeShape::color() const
{
    if (QSharedPointer<KoColorBackground> bg = qSharedPointerDynamicCast<KoColorBackground>(background())) {
        return bg->color();
    }
    return m_data->noLine ? m_data->noLineColor : QColor(Qt::black);
}

void VStrokeShape::paint(QPainter &painter) const
{
    VTrace::Scope trace("shape.paint");
    if (m_data->noLine) return;

    if (!m_data->preset) {
        paintOutline(painter);
        return;
    }

    const VStrokeRenderKey key{renderTransform(), m_revision};
    VStrokeRenderCache::Entry entry;
    if (m_cache->lookup(key, &entry)) {
        paintRendering(painter, entry);
        return;
    }

    // A copy for a repaint prepared before the stroke changed asks for
    // nothing: a newer repaint follows. This runs for every tile of the
    // repaint, so the primitives are only built when a render is queued.
    // While a Fast transform moves the stroke, nothing renders.
    if (!m_motion.rendersPaused && m_revision == m_cache->currentRevision() && m_cache->needsRequest(key)) {
        m_cache->request(key, renderSettings(), imagePixelPrimitives(), curves());
    }

    // until the render arrives: the newest rendering, of older curves while
    // points are dragged or under the old transform while the shape is
    // moved, so the stroke keeps its look and catches up render by render.
    // Bent to the current curves where drawing it as it is would be wrong
    // (bendsFrom()): a Fast transform, a perspective, a scale or shear
    if (m_cache->newest(&entry)) {
        if (bendsFrom(entry) && paintWarped(painter, entry)) return;
        if (poseDiffers(entry)) s_staleShapePaints.ref();
        paintRendering(painter, entry);
    } else {
        s_outlinePaints.ref();
        paintOutline(painter);
    }
}

void VStrokeShape::paintStroke(QPainter &painter) const
{
    Q_UNUSED(painter);
}

void VStrokeShape::paintMarkers(QPainter &painter) const
{
    Q_UNUSED(painter);
}

qreal VStrokeShape::maxRadius() const
{
    const QVector<qreal> &radii = curves().radii;
    return radii.isEmpty() ? 0.0 : *std::max_element(radii.begin(), radii.end());
}

KoShapeStrokeModelSP VStrokeShape::stroke() const
{
    if (m_savingSvg) return KoShapeStrokeModelSP();
    return toQShared(new KoShapeStroke(2.0 * maxRadius() / m_pixelsPerPoint, m_data->noLine ? QColor() : color()));
}

void VStrokeShape::setStroke(KoShapeStrokeModelSP stroke)
{
    if (!isLive()) return;

    if (const QSharedPointer<KoShapeStroke> s = qSharedPointerDynamicCast<KoShapeStroke>(stroke)) {
        const QColor newColor = s->color();
        // gradients don't make a color
        if (!s->lineBrush().gradient()) {
            if (!newColor.isValid() || newColor.alpha() == 0) {
                if (!m_data->noLine) setBackground(QSharedPointer<KoShapeBackground>());
            } else if (m_data->noLine || newColor != color()) {
                setBackground(toQShared(new KoColorBackground(newColor)));
            }
        }

        const qreal oldRadius = maxRadius();
        const qreal newRadius = 0.5 * s->lineWidth() * m_pixelsPerPoint;
        if (oldRadius > 0 && newRadius > 0 && !qFuzzyCompare(oldRadius, newRadius)) {
            VCurveData scaled = curves();
            for (qreal &r : scaled.radii) {
                // exact at the top: strokes given one width show one width
                r = r == oldRadius ? newRadius : r * newRadius / oldRadius;
            }
            setCurves(scaled);
        }
    }
    // tells whoever watches the shape; nothing is kept
    VCurveShape::setStroke(KoShapeStrokeModelSP());
}

KoStrokeFeatures::Features VStrokeShape::strokeFeatures() const
{
    // caps and joins change nothing here, but a mixed selection can pass them on to
    // the poly-tool strokes in it
    return Width | CapAndJoin;
}

QColor VStrokeShape::hiddenColor() const
{
    return m_data->noLine ? m_data->noLineColor : QColor();
}

void VStrokeShape::setBackground(QSharedPointer<KoShapeBackground> background)
{
    const bool hide = !background && isLive();
    const bool wasHidden = m_data->noLine;
    if (hide && !wasHidden) m_data->noLineColor = color();
    if (hide || background) m_data->noLine = hide;
    VCurveShape::setBackground(background);
    // nothing renders while hidden, and no finished render would update the area
    if (hide && !wasHidden) update();
}

QPainter::CompositionMode VStrokeData::compositionMode(const QString &compositeOp)
{
    static const QHash<QString, QPainter::CompositionMode> modes = {
        {COMPOSITE_ERASE, QPainter::CompositionMode_DestinationOut},
        {COMPOSITE_MULT, QPainter::CompositionMode_Multiply},
        {COMPOSITE_SCREEN, QPainter::CompositionMode_Screen},
        {COMPOSITE_OVERLAY, QPainter::CompositionMode_Overlay},
        {COMPOSITE_DARKEN, QPainter::CompositionMode_Darken},
        {COMPOSITE_LIGHTEN, QPainter::CompositionMode_Lighten},
        {COMPOSITE_DODGE, QPainter::CompositionMode_ColorDodge},
        {COMPOSITE_BURN, QPainter::CompositionMode_ColorBurn},
        {COMPOSITE_HARD_LIGHT, QPainter::CompositionMode_HardLight},
        {COMPOSITE_SOFT_LIGHT_SVG, QPainter::CompositionMode_SoftLight},
        {COMPOSITE_SOFT_LIGHT_PHOTOSHOP, QPainter::CompositionMode_SoftLight},
        {COMPOSITE_DIFF, QPainter::CompositionMode_Difference},
        {COMPOSITE_EXCLUSION, QPainter::CompositionMode_Exclusion},
        {COMPOSITE_ADD, QPainter::CompositionMode_Plus},
    };
    return modes.value(compositeOp, QPainter::CompositionMode_SourceOver);
}

bool VStrokeData::canBlendWith(const QString &compositeOp)
{
    return compositeOp == COMPOSITE_OVER || compositionMode(compositeOp) != QPainter::CompositionMode_SourceOver ||
           KoColorSpaceRegistry::instance()->rgb8()->hasCompositeOp(compositeOp);
}

void VStrokeData::drawImage(QPainter &painter, const QString &compositeOp, const QPointF &pos, const QImage &image)
{
    const QPainter::CompositionMode mode = compositionMode(compositeOp);
    QImage *target = dynamic_cast<QImage*>(painter.device());
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const bool kritaOp = mode == QPainter::CompositionMode_SourceOver && compositeOp != COMPOSITE_OVER &&
                         cs->hasCompositeOp(compositeOp) && target &&
                         (target->format() == QImage::Format_ARGB32 ||
                          target->format() == QImage::Format_ARGB32_Premultiplied);
    if (!kritaOp) {
        painter.setCompositionMode(mode);
        painter.drawImage(pos, image);
        return;
    }

    // QPainter doesn't have this mode, so Krita's op blends on the painter's image. That image
    // is 8-bit sRGB whatever the document's color space, so results can differ from a raster layer
    const QTransform toDevice = painter.deviceTransform();
    QRect area = toDevice.mapRect(QRectF(pos, QSizeF(image.size()))).toAlignedRect() & target->rect();
    if (painter.hasClipping()) area &= toDevice.mapRect(painter.clipBoundingRect()).toAlignedRect();
    if (area.isEmpty()) return;

    // the stroke alone, as the painter would have drawn it
    QImage stroke(area.size(), QImage::Format_ARGB32_Premultiplied);
    stroke.fill(Qt::transparent);
    {
        QPainter gc(&stroke);
        gc.setRenderHints(painter.renderHints());
        gc.setTransform(toDevice * QTransform::fromTranslate(-area.x(), -area.y()));
        if (painter.hasClipping()) gc.setClipPath(painter.clipPath());
        gc.setOpacity(painter.opacity());
        gc.drawImage(pos, image);
    }
    stroke = stroke.convertToFormat(QImage::Format_ARGB32);

    // ARGB32 is laid out like Krita's 8-bit RGBA
    QImage blended = target->copy(area).convertToFormat(QImage::Format_ARGB32);
    cs->compositeOp(compositeOp)->composite(blended.bits(), blended.bytesPerLine(), stroke.constBits(), stroke.bytesPerLine(),
                                            nullptr, 0, area.height(), area.width(), 1.0f);
    blended = blended.convertToFormat(target->format());

    // only under the stroke: some modes change the layer even where the source is transparent
    for (int y = 0; y < area.height(); y++) {
        const QRgb *from = reinterpret_cast<const QRgb*>(blended.constScanLine(y));
        const QRgb *coverage = reinterpret_cast<const QRgb*>(stroke.constScanLine(y));
        QRgb *to = reinterpret_cast<QRgb*>(target->scanLine(area.y() + y)) + area.x();
        for (int x = 0; x < area.width(); x++) {
            if (qAlpha(coverage[x])) to[x] = from[x];
        }
    }
}

void VStrokeShape::paintRendering(QPainter &painter, const VStrokeRenderCache::Entry &entry) const
{
    if (entry.image.isNull()) return;

    painter.save();
    // painter coordinates are shape-local; the image is in image pixels
    const QTransform imageToPainter = entry.key.transform.inverted() * painter.transform();
    if (isIntegerTranslation(imageToPainter)) {
        // the shape layer canvas: copy the pixels 1:1
        painter.setTransform(QTransform::fromTranslate(qRound(imageToPainter.dx()), qRound(imageToPainter.dy())));
    } else {
        painter.setTransform(imageToPainter);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
    }
    VStrokeData::drawImage(painter, m_data->compositeOp, entry.offset, entry.image);
    painter.restore();
}

bool VStrokeShape::poseDiffers(const VStrokeRenderCache::Entry &entry) const
{
    if (entry.curves.isEmpty()) return false;
    const VCurveData &now = curves();
    if (entry.curves.positions != now.positions || entry.curves.handlesOut != now.handlesOut ||
        entry.curves.handlesIn != now.handlesIn) {
        return true;
    }
    return !isRigid(entry.key.transform.inverted() * renderTransform());
}

bool VStrokeShape::bendsFrom(const VStrokeRenderCache::Entry &entry) const
{
    if (!samePoints(entry.curves, curves()) || !poseDiffers(entry)) return false;
    // Fast: no render comes while the moves go on, and the rendering of the
    // old pose would hop back once they pause; a perspective, by its hint
    if (m_motion.rendersPaused || m_settling || !m_motion.hint.isIdentity()) return true;
    // Accurate point moves (Liquify, Edit Shapes) draw the last rendering as
    // it is: the next render is on its way, and a rendering bent piece by
    // piece shows notches and steps at the joints wherever the stroke bends
    // more between the two poses. A scale or shear bends, so the brush keeps
    // its size meanwhile
    return !isRigid(entry.key.transform.inverted() * renderTransform());
}

bool VStrokeShape::warpHintFits(const VStrokeRenderCache::Entry &entry) const
{
    if (m_motion.hint.isIdentity() || entry.image.isNull()) return false;
    const VCurveData &now = curves();
    const VCurveData &then = entry.curves;
    if (now.pointCount() != then.pointCount()) return false;
    const QTransform oldToImage = entry.key.transform;
    const QTransform newToImage = renderTransform();
    for (int i = 0; i < now.pointCount(); i++) {
        const QPointF d = m_motion.hint.map(oldToImage.map(then.positions[i])) - newToImage.map(now.positions[i]);
        if (d.x() * d.x() + d.y() * d.y() >= 0.05 * 0.05) return false;
    }
    // near the horizon a perspective blows up: bend piece by piece there
    const QRectF source(entry.offset, entry.image.size());
    const QRectF mapped = m_motion.hint.mapRect(source);
    return !mapped.isEmpty() && mapped.width() * mapped.height() <= 16.0 * qMax(1.0, source.width() * source.height());
}

bool VStrokeShape::paintWarped(QPainter &painter, const VStrokeRenderCache::Entry &entry) const
{
    if (entry.image.isNull()) return true; // the stroke painted nothing

    // a perspective: the rendering drawn through it, with no bent copy to make for each pose
    if (warpHintFits(entry)) {
        painter.save();
        painter.setTransform(m_motion.hint * renderTransform().inverted() * painter.transform());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        VStrokeData::drawImage(painter, m_data->compositeOp, entry.offset, entry.image);
        painter.restore();
        return true;
    }

    // made once for all the tiles of a repaint
    const VStrokeRenderKey key{renderTransform(), m_revision};
    VStrokeRenderCache::Entry warped;
    if (!m_cache->warped(key, entry.key, &warped)) {
        warped = warpedRendering(entry);
        // a copy for an older repaint doesn't push out the current stand-in
        if (m_revision == m_cache->currentRevision()) m_cache->setWarped(warped, entry.key);
    }
    paintRendering(painter, warped);
    return true;
}

VStrokeRenderCache::Entry VStrokeShape::warpedRendering(const VStrokeRenderCache::Entry &entry) const
{
    const VCurveData &now = curves();
    const VCurveData &then = entry.curves;
    const QTransform oldToImage = entry.key.transform;
    const QTransform newToImage = renderTransform();

    VStrokeRenderCache::Entry result;
    result.key = VStrokeRenderKey{newToImage, m_revision};
    result.curves = now;

    // a perspective of the whole rendering: drawn through it in one go
    if (warpHintFits(entry)) {
        const QRectF source(entry.offset, entry.image.size());
        const QRect rect = m_motion.hint.mapRect(source).toAlignedRect().adjusted(-1, -1, 1, 1);
        {
            QImage image(rect.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter p(&image);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setTransform(QTransform::fromTranslate(entry.offset.x(), entry.offset.y()) * m_motion.hint *
                           QTransform::fromTranslate(-rect.x(), -rect.y()));
            p.drawImage(QPointF(), entry.image);
            p.end();
            result.image = image;
            result.offset = rect.topLeft();
            return result;
        }
    }

    // the pieces, in the rendering's pose and in the current one; a run of
    // pieces that didn't move is one polygon, copied as it is
    struct Piece { QPolygonF from; QPolygonF to; bool still; };
    QVector<Piece> pieces;
    QRectF bounds;
    auto addTriangle = [&] (const QPointF &a, const QPointF &b, const QPointF &c,
                            const QPointF &a2, const QPointF &b2, const QPointF &c2) {
        pieces << Piece{QPolygonF({a, b, c}), QPolygonF({a2, b2, c2}), false};
        bounds |= pieces.last().to.boundingRect();
    };

    QVector<QVector<QPointF>> fromCurves, toCurves;
    QRectF polylineBounds;
    for (int curve = 0; curve < now.curveCount(); curve++) {
        fromCurves << sampledCurve(then, curve, oldToImage, now);
        toCurves << sampledCurve(now, curve, newToImage, then);
        polylineBounds |= QPolygonF(fromCurves.last()).boundingRect();
    }
    // how far the rendering reaches past the curves (scatter, texture), as
    // the least width of the pieces
    const QRectF source(entry.offset, entry.image.size());
    const qreal reach = qMax(qMax(polylineBounds.left() - source.left(), source.right() - polylineBounds.right()),
                             qMax(polylineBounds.top() - source.top(), source.bottom() - polylineBounds.bottom())) + 1.0;

    const qreal spacing = 10.0;
    for (int curve = 0; curve < now.curveCount(); curve++) {
        const QVector<QPointF> &from = fromCurves[curve];
        const QVector<QPointF> &to = toCurves[curve];
        if (from.isEmpty() || from.size() != to.size()) continue;
        // brush dabs reach past the radius: half a brush more; each pose
        // has its own width, so scaled radii widen the stroke
        const qreal radiusFrom = maxRadiusOf(then, curve) + 0.5 * m_data->brushSize + 1.0;
        const qreal radiusTo = maxRadiusOf(now, curve) + 0.5 * m_data->brushSize + 1.0;
        const qreal halfFrom = qMax(radiusFrom, reach);
        const qreal halfTo = halfFrom * radiusTo / radiusFrom;
        const bool cyclic = now.isCyclic(curve) && from.size() > 2;

        // sections about every spacing pixels along the rendering's pose, and at the ends
        QVector<int> chosen = {0};
        qreal run = 0;
        for (int i = 1; i < from.size(); i++) {
            run += KisAlgebra2D::norm(from[i] - from[i - 1]);
            if (run >= spacing || i == from.size() - 1) {
                chosen << i;
                run = 0;
            }
        }
        if (chosen.size() == 1) {
            // a single dab: a square around it, moved
            const QPointF d(halfFrom, halfFrom), e(halfFrom, -halfFrom);
            const QPointF d2(halfTo, halfTo), e2(halfTo, -halfTo);
            addTriangle(from[0] - d, from[0] + e, from[0] + d, to[0] - d2, to[0] + e2, to[0] + d2);
            addTriangle(from[0] - d, from[0] + d, from[0] - e, to[0] - d2, to[0] + d2, to[0] - e2);
            continue;
        }
        const Sections a = crossSections(from, chosen, cyclic, halfFrom);
        const Sections b = crossSections(to, chosen, cyclic, halfTo);
        const int sections = a.left.size();
        auto still = [&] (int k) {
            return samePoint(a.left[k], b.left[k]) && samePoint(a.right[k], b.right[k]);
        };
        QPolygonF runLeft, runRight; // a run of sections that didn't move
        auto closeRun = [&] () {
            if (runLeft.size() >= 2) {
                QPolygonF polygon = runLeft;
                for (int i = runRight.size() - 1; i >= 0; i--) polygon << runRight[i];
                pieces << Piece{polygon, polygon, true};
                bounds |= polygon.boundingRect();
            }
            runLeft.clear();
            runRight.clear();
        };
        for (int k = 0; k + 1 < sections || (cyclic && k < sections); k++) {
            const int k1 = (k + 1) % sections;
            if (still(k) && still(k1)) {
                if (runLeft.isEmpty()) {
                    runLeft << a.left[k];
                    runRight << a.right[k];
                }
                runLeft << a.left[k1];
                runRight << a.right[k1];
                continue;
            }
            closeRun();
            addTriangle(a.left[k], a.left[k1], a.right[k1], b.left[k], b.left[k1], b.right[k1]);
            addTriangle(a.left[k], a.right[k1], a.right[k], b.left[k], b.right[k1], b.right[k]);
        }
        closeRun();
    }

    const QRect rect = bounds.toAlignedRect();
    if (rect.isEmpty()) return result;

    // each piece replaces what is under it, so overlapping pieces (inside
    // turns) don't add up; unantialiased clips share their edges exactly
    QImage image(rect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        const QTransform toImage = QTransform::fromTranslate(-rect.x(), -rect.y());
        for (const Piece &piece : pieces) {
            QTransform affine;
            if (piece.still) {
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            } else {
                const QPointF from[3] = {piece.from[0], piece.from[1], piece.from[2]};
                const QPointF to[3] = {piece.to[0], piece.to[1], piece.to[2]};
                if (!triangleTransform(from, to, &affine)) continue;
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            }
            // only the part of the rendering the piece takes
            const QRectF taken = piece.from.boundingRect().adjusted(-2, -2, 2, 2) & source;
            if (taken.isEmpty()) continue;
            QPainterPath clip;
            clip.addPolygon(piece.to);
            clip.closeSubpath();
            p.setTransform(toImage);
            p.setClipPath(clip);
            p.setTransform(affine * toImage);
            p.drawImage(taken.topLeft(), entry.image, taken.translated(-entry.offset));
        }
    }
    result.image = image;
    result.offset = rect.topLeft();
    return result;
}

void VStrokeShape::setMotion(const Motion &motion)
{
    const bool resumes = m_motion.rendersPaused && !motion.rendersPaused;
    m_motion = motion;
    if (!resumes) return;
    // the next paint asks for the rendering of the current curves; the bent
    // stand-in shows until it lands, never the rendering of the old pose
    VStrokeRenderCache::Entry entry;
    m_settling = !m_cache->lookup(VStrokeRenderKey{renderTransform(), m_revision}, &entry);
    update();
}

void VStrokeShape::clearMotion()
{
    setMotion(Motion{false, m_motion.hint});
}

int VStrokeShape::staleShapePaints()
{
    return s_staleShapePaints.loadAcquire();
}

void VStrokeShape::paintOutline(QPainter &painter) const
{
    QColor color = this->color();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(VStrokeData::compositionMode(m_data->compositeOp));
    if (m_data->erases()) color = Qt::black;
    painter.setOpacity(painter.opacity() * m_data->opacity);
    painter.fillPath(outline(), color);
    painter.restore();
}

VStrokeRenderSettings VStrokeShape::renderSettings() const
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();

    VStrokeRenderSettings s;
    s.preset = m_data->preset;
    s.paintColor = KoColor(m_data->erases() ? QColor(Qt::black) : color(), cs);
    s.backgroundColor = KoColor(m_data->backgroundColor, cs);
    s.opacity = m_data->opacity;
    s.seed = m_data->seed;
    s.perStrokeSeed = m_data->perStrokeSeed;
    if (VCurveOpacity::isVarying(curves())) {
        s.opacityCurves = curves();
        s.opacityCurves.transform(renderTransform());
    }
    if (!curves().isEmpty()) {
        s.startDistance = VStrokePainter::startDistance(m_data->preset,
                                                        renderTransform().map(curves().positions.first()),
                                                        m_data->startAngle);
    }
    return s;
}

void VStrokeShape::shapeChanged(ChangeType type, KoShape *shape)
{
    KoShape::shapeChanged(type, shape);
    if (shape) return; // a change of another shape

    switch (type) {
    case BackgroundChanged:
        newRevision();
        Q_FALLTHROUGH();
    case ParentChanged:
    case PositionChanged:
    case RotationChanged:
    case ScaleChanged:
    case ShearChanged:
    case SizeChanged:
    case GenericMatrixChange:
        // a shape in a layer gets its own cache: a copy (duplicated layer,
        // autosave) must not update the original instead of itself
        claimCache();
        break;
    default:
        break;
    }
}

bool VStrokeShape::isLive() const
{
    // in a vector layer; the clones KoShapeManager paints from are cloned
    // from the root down and have no layer above them
    const KoShape *shape = this;
    while (shape->parent()) {
        shape = shape->parent();
    }
    return dynamic_cast<const KoShapeLayer*>(shape);
}

void VStrokeShape::claimCache()
{
    if (!isLive()) return;
    if (m_cache->owner() != this) {
        m_cache = m_cache->detachedCopy(this);
        m_cache->setCurrentRevision(m_revision);
    }
}

void VStrokeShape::adoptRendering(KisPaintDeviceSP device)
{
    const VStrokeRenderCache::Entry entry =
        VStrokeRenderCache::fromDevice(VStrokeRenderKey{renderTransform(), m_revision}, device);
    VStrokeRenderCache::Entry withCurves = entry;
    withCurves.curves = curves();
    m_cache->insert(withCurves);
    // as renderingFinished() does: the next rendering must repaint this area
    if (!entry.image.isNull()) {
        m_renderedRect = entry.key.transform.inverted().mapRect(QRectF(entry.offset, entry.image.size()));
    }
}

void VStrokeShape::waitForRendering() const
{
    m_cache->waitForIdle();
}

void VStrokeShape::setSize(const QSizeF &newSize)
{
    VCurveShape::setSize(newSize);
    newRevision();
    claimCache();
}

qreal VStrokeShape::pixelsPerUnit() const
{
    return qSqrt(qAbs(renderTransform().determinant()));
}

qreal VStrokeShape::outlineRadiusScale() const
{
    const qreal ppu = pixelsPerUnit();
    return ppu > 0 ? 1.0 / ppu : 0.0;
}

qreal VStrokeShape::outlineMinRadius() const
{
    return 0.5 * outlineRadiusScale();
}

qreal VStrokeShape::outlineTolerance() const
{
    return 0.25 * outlineRadiusScale();
}

QRectF VStrokeShape::boundingRect() const
{
    // the brush may paint outside the pressure-width outline (scatter,
    // texture, ribbon), so allow one more brush diameter
    const qreal margin = (m_data->brushSize + 2.0) / m_pixelsPerPoint;
    QRectF rect = absoluteTransformation().mapRect(outlineRect()).adjusted(-margin, -margin, margin, margin);

    VStrokeRenderCache::Entry entry;
    if (m_cache->newest(&entry)) {
        rect |= documentRect(entry);
        // a perspective's stand-in (paintWarped()) shows where the hint takes the
        // rendering, which may reach past the outline's margin: the repaint mustn't clip it
        if (warpHintFits(entry)) {
            const QRectF source(entry.offset, entry.image.size());
            rect |= (renderTransform().inverted() * absoluteTransformation()).mapRect(m_motion.hint.mapRect(source));
        }
    }
    if (!m_renderedRect.isEmpty()) {
        rect |= absoluteTransformation().mapRect(m_renderedRect);
    }
    return rect;
}

void VStrokeShape::newRevision()
{
    // a new look: the kept rendering would show the old one
    m_kept = KeptRendering();
    nextCurvesRevision();
}

void VStrokeShape::nextCurvesRevision()
{
    // the older renderings are still painted until this revision's arrives:
    // m_renderedRect keeps their area
    m_revision = nextRevision();
    m_revisionCurves = curves();
    if (m_cache->owner() == this) {
        m_cache->setCurrentRevision(m_revision);
    }
}

void VStrokeShape::keepRendering(const VCurveData &next)
{
    if (m_kept.valid && m_kept.curves == next) return; // a redo: the rendering of next is kept
    VStrokeRenderCache::Entry entry;
    if (!m_cache->lookup(VStrokeRenderKey{renderTransform(), m_revision}, &entry)) return;
    m_kept.curves = curves();
    m_kept.entry = entry;
    m_kept.valid = true;
}

QRectF VStrokeShape::documentRect(const VStrokeRenderCache::Entry &entry) const
{
    if (entry.image.isNull()) return QRectF();
    // image pixels to document points
    const QTransform imageToDocument = entry.key.transform.inverted() * absoluteTransformation();
    return imageToDocument.mapRect(QRectF(entry.offset, entry.image.size()));
}

void VStrokeShape::renderingFinished(const VStrokeRenderCache::Entry &entry)
{
    const QRectF local = entry.image.isNull()
        ? QRectF() : entry.key.transform.inverted().mapRect(QRectF(entry.offset, entry.image.size()));
    m_renderedRect |= local;
    // the bounds grow with the rendering: the shape manager finds the
    // shapes to paint in each tile by the bounds it last indexed
    notifyChanged();
    update();
    if (entry.key == VStrokeRenderKey{renderTransform(), m_revision}) {
        // the cache dropped the older renderings, and update() covered their area
        m_renderedRect = local;
        // the stand-in is done with
        if (!m_motion.rendersPaused) m_motion.hint = QTransform();
        m_settling = false;
    }
}

bool VStrokeShape::saveSvg(SvgSavingContext &context)
{
    KoXmlWriter &writer = context.shapeWriter();
    const QTransform userSpace = context.userSpaceTransform();

    writer.startElement("path");
    writer.addAttribute("id", context.getID(this));
    SvgUtil::writeTransformAttributeLazy("transform", transformation(), writer);

    if (m_data->erases()) {
        // other readers cannot erase, so they show nothing
        writer.addAttribute("fill", "none");
        writer.addAttribute("stroke", "none");
    } else {
        m_savingSvg = true;
        SvgStyleWriter::saveSvgStyle(this, context);
        m_savingSvg = false;
    }
    writer.addAttribute("d", pathData(outline(), userSpace));

    writer.addAttribute("krita:type", "vstroke");
    writer.addAttribute("krita:vstroke-version", 3);
    if (const VStrokePresetDefinitionSP definition = m_data->presetDefinition) {
        // once per file, before the shapes (the defs come first)
        if (context.registerSharedDefinition(definition->key)) {
            KoXmlWriter &defs = context.styleWriter();
            defs.startElement(VStrokePresetStore::ElementName);
            defs.addAttribute("id", definition->key);
            QDomDocument preset;
            preset.setContent(definition->xml);
            writeDomElement(defs, preset.documentElement());
            defs.endElement();
        }
        writer.addAttribute("krita:preset-ref", definition->key);
    }
    // version 1 readers find the preset resource by these
    writer.addAttribute("krita:preset-name", m_data->presetName);
    writer.addAttribute("krita:preset-filename", m_data->presetFileName);
    writer.addAttribute("krita:preset-md5", m_data->presetMd5);
    writer.addAttribute("krita:brush-size", m_data->brushSize);
    writer.addAttribute("krita:opacity", m_data->opacity);
    writer.addAttribute("krita:seed", m_data->seed);
    writer.addAttribute("krita:per-stroke-seed", m_data->perStrokeSeed);
    writer.addAttribute("krita:start-angle", m_data->startAngle);
    writer.addAttribute("krita:composite-op", m_data->compositeOp);
    writer.addAttribute("krita:erase", m_data->erases() ? 1 : 0); // what builds without krita:composite-op read
    writer.addAttribute("krita:background-color", m_data->backgroundColor.name(QColor::HexArgb));
    if (m_data->noLine) {
        writer.addAttribute("krita:no-line", 1);
        writer.addAttribute("krita:no-line-color", m_data->noLineColor.name(QColor::HexArgb));
    }
    SvgStyleWriter::saveMetadata(this, context);

    QDomDocument doc;
    QDomElement data = doc.createElement(VCurveData::XmlElement);
    curves().toXML(doc, data, userSpace);
    writeDomElement(writer, data);

    writer.endElement();
    return true;
}

bool VStrokeShape::loadSvg(const QDomElement &element, SvgLoadingContext &context)
{
    if (element.tagName() != "path" || element.attribute("krita:type") != "vstroke") {
        return false;
    }

    VStrokeData &d = *m_data;
    d.brushSize = element.attribute("krita:brush-size", "10").toDouble();

    VCurveData curves;
    const QDomElement curvesElement = element.firstChildElement(VCurveData::XmlElement);
    if (!curvesElement.isNull()) {
        if (!VCurveData::fromXML(curvesElement, &curves)) return false;
    } else {
        // versions 1 and 2: the primitives
        QVector<VStrokePrimitive> primitives;
        const QDomElement data = element.firstChildElement("krita:vstroke-data");
        for (QDomElement e = data.firstChildElement("krita:p"); !e.isNull(); e = e.nextSiblingElement("krita:p")) {
            primitives << VStrokePrimitive::fromXML(e);
        }
        curves = VStrokeCurves::fromPrimitives(primitives, d.brushSize);
    }
    if (curves.isEmpty()) {
        return false;
    }

    d.presetName = element.attribute("krita:preset-name");
    d.presetFileName = element.attribute("krita:preset-filename");
    d.presetMd5 = element.attribute("krita:preset-md5");
    d.opacity = element.attribute("krita:opacity", "1").toDouble();
    d.seed = element.attribute("krita:seed", "0").toInt();
    d.perStrokeSeed = element.attribute("krita:per-stroke-seed", "0").toInt();
    d.startAngle = element.attribute("krita:start-angle", "0").toDouble();
    d.compositeOp = element.attribute("krita:composite-op",
                                      element.attribute("krita:erase", "0").toInt() ? COMPOSITE_ERASE : COMPOSITE_OVER);
    d.backgroundColor = QColor(element.attribute("krita:background-color", "#ffffffff"));
    // fill="none" alone isn't hidden: erasers save that too
    d.noLine = element.attribute("krita:no-line", "0").toInt();
    d.noLineColor = QColor(element.attribute("krita:no-line-color"));

    const QString ref = element.attribute("krita:preset-ref");
    if (!ref.isEmpty() && context.hasDefinition(ref)) {
        d.presetDefinition = VStrokePresetStore::instance()->fromSaved(context.definition(ref));
        if (d.presetDefinition) {
            d.preset = d.presetDefinition->preset;
        }
    }
    if (!d.preset) {
        // a version 1 file, a missing brush engine or a broken reference: the
        // preset resource, at the size the stroke was drawn with
        d.preset = findPreset(d.presetMd5, d.presetFileName, d.presetName);
        if (d.preset) {
            d.preset->settings()->setPaintOpSize(d.brushSize);
            if (!d.presetDefinition) {
                d.presetDefinition = VStrokePresetStore::instance()->intern(d.preset);
                d.preset = d.presetDefinition->preset;
            }
        }
    }

    setCurves(curves);
    return true;
}

const VStrokeData &VStrokeShape::data() const
{
    return *m_data;
}

void VStrokeShape::setData(const VStrokeData &data)
{
    update();
    m_data = new VStrokeData(data);
    if (m_data->preset && !m_data->presetDefinition) {
        m_data->presetDefinition = VStrokePresetStore::instance()->intern(m_data->preset);
        // strokes drawn with the same settings share one preset
        if (m_data->presetDefinition) m_data->preset = m_data->presetDefinition->preset;
    }
    newRevision();
    claimCache();
    update();
}

void VStrokeShape::setResolution(qreal xRes, qreal yRes)
{
    Q_UNUSED(yRes); // image pixels are square for the brush engine
    setPixelsPerPoint(xRes / 72.0);
}

void VStrokeShape::setPixelsPerPoint(qreal pixelsPerPoint)
{
    if (pixelsPerPoint <= 0 || qFuzzyCompare(pixelsPerPoint, m_pixelsPerPoint)) return;
    m_pixelsPerPoint = pixelsPerPoint;
    claimCache();
}

qreal VStrokeShape::resolution() const
{
    return m_pixelsPerPoint;
}

QTransform VStrokeShape::renderTransform() const
{
    return absoluteTransformation() * QTransform::fromScale(m_pixelsPerPoint, m_pixelsPerPoint);
}

QVector<VStrokePrimitive> VStrokeShape::imagePixelPrimitives() const
{
    return VStrokeCurves::toPrimitives(curves(), m_data->brushSize, renderTransform());
}

void VStrokeShape::curvesChanged()
{
    VCurveShape::curvesChanged();
    // moved on after a Fast pause (Accurate again): the stand-in is drawn as it is
    if (!m_motion.rendersPaused) m_settling = false;
    const quint64 previousRevision = m_revision;
    const VCurveData previousCurves = m_revisionCurves;
    nextCurvesRevision();
    claimCache();

    // back to the kept curves (an undo): their rendering under the new
    // revision, and the rendering of the curves left kept for the redo
    if (!m_kept.valid || !(m_kept.curves == curves())) return;
    if (!(VStrokeRenderKey{m_kept.entry.key.transform, 0} == VStrokeRenderKey{renderTransform(), 0})) {
        m_kept = KeptRendering(); // moved since: it renders anew
        return;
    }
    VStrokeRenderCache::Entry back = m_kept.entry;
    back.key.revision = m_revision;
    back.curves = curves();
    VStrokeRenderCache::Entry left;
    if (m_cache->lookup(VStrokeRenderKey{renderTransform(), previousRevision}, &left)) {
        m_kept.curves = previousCurves;
        m_kept.entry = left;
    }
    // else the curves left were never rendered (Liquify's Fast mode): the
    // kept rendering stays, for when the curves come back here again
    m_cache->insert(back);
    if (!back.image.isNull()) {
        // as renderingFinished(): the next rendering must repaint this area
        m_renderedRect |= back.key.transform.inverted().mapRect(QRectF(back.offset, back.image.size()));
    }
}
