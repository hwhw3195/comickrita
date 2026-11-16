/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VDeform.h"

#include <QApplication>
#include <QMutex>
#include <QThread>
#include <QtMath>

#include <KoParameterShape.h>
#include <KoPathPoint.h>
#include <KoPathShape.h>
#include <kundo2command.h>

#include <kis_assert.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_random_accessor_ng.h>
#include <kis_selection.h>
#include <kis_shape_layer.h>
#include <klocalizedstring.h>

#include "VFreehandLayer.h"
#include "VGuiThreadCommand.h"
#include "VMoveSession.h"

namespace {

/// how far a mapped line may stray from straight before it becomes a bezier, in image pixels
const qreal BendTolerance = 0.2;
/// the step of the derivative's differences, in image pixels
const qreal DerivativeStep = 0.5;

/// a map in some units, its derivative, and its scale, for one shape
struct LocalMap {
    LocalMap(const QTransform &toImage, const VDeform::PointMap &map)
        : toImage(toImage),
          fromImage(toImage.inverted()),
          map(map)
    {
        pixelsPerUnit = qSqrt(qAbs(toImage.determinant()));
        if (pixelsPerUnit > 0) {
            step = DerivativeStep / pixelsPerUnit;
            tolerance = BendTolerance / pixelsPerUnit;
        }
    }

    bool isValid() const { return pixelsPerUnit > 0 && map; }

    QPointF operator()(const QPointF &p) const { return fromImage.map(map(toImage.map(p))); }

    /// the derivative at \p p: the columns for x and y
    struct Derivative {
        QPointF dx;
        QPointF dy;
        QPointF operator*(const QPointF &d) const { return dx * d.x() + dy * d.y(); }
        qreal determinant() const { return dx.x() * dy.y() - dy.x() * dx.y(); }
    };
    Derivative derivative(const QPointF &p) const
    {
        const QPointF h(step, 0);
        const QPointF v(0, step);
        return Derivative{((*this)(p + h) - (*this)(p - h)) / (2 * step),
                          ((*this)(p + v) - (*this)(p - v)) / (2 * step)};
    }

    bool bends(const QPointF &a, const QPointF &b, const QPointF &mappedA, const QPointF &mappedB) const
    {
        return bendsPast((*this)((a + b) / 2), mappedA, mappedB, tolerance);
    }

    void fitHandles(const QPointF &q0, const QPointF &q3, const QPointF &out, const QPointF &in,
                    const std::function<QPointF(qreal)> &segment, QPointF *handle1, QPointF *handle2) const
    {
        QPointF mappedSamples[5];
        for (int k = 1; k <= 5; k++) mappedSamples[k - 1] = (*this)(segment(k / 6.0));
        fitHandlesTo(q0, q3, out, in, mappedSamples, handle1, handle2);
    }

    /**
     * Whether a line whose ends go to \p mappedA and \p mappedB bends past
     * \p tolerance: its middle, going to \p mappedMiddle, lands off the line
     * between them. (Perspective keeps lines straight, only spaced unevenly.)
     */
    static bool bendsPast(const QPointF &mappedMiddle, const QPointF &mappedA, const QPointF &mappedB, qreal tolerance)
    {
        const QPointF middle = mappedMiddle - mappedA;
        const QPointF chord = mappedB - mappedA;
        const qreal length = qSqrt(chord.x() * chord.x() + chord.y() * chord.y());
        const qreal off = length > 0 ? qAbs(chord.x() * middle.y() - chord.y() * middle.x()) / length
                                     : qSqrt(middle.x() * middle.x() + middle.y() * middle.y());
        return off > tolerance;
    }

    /**
     * The handles of the bezier from \p q0 to \p q3 that follows a mapped
     * segment, given as \p mappedSamples (the segment at t = 1/6 ... 5/6,
     * mapped): they leave and arrive in the directions of \p out and \p in
     * (the derivative's handles), as long as fits those points best (least
     * squares). The derivative's own lengths stay when the fit is no good.
     */
    static void fitHandlesTo(const QPointF &q0, const QPointF &q3, const QPointF &out, const QPointF &in,
                             const QPointF *mappedSamples, QPointF *handle1, QPointF *handle2)
    {
        qreal alpha = 1;
        qreal beta = 1;
        // r(t) = mapped(t) - the bezier without handles' share = alpha b1 out + beta b2 in
        qreal a11 = 0, a12 = 0, a22 = 0, r1 = 0, r2 = 0;
        for (int k = 1; k <= 5; k++) {
            const qreal t = k / 6.0;
            const qreal s = 1 - t;
            const qreal b1 = 3 * s * s * t;
            const qreal b2 = 3 * s * t * t;
            const QPointF r = mappedSamples[k - 1] - (s * s * s + b1) * q0 - (b2 + t * t * t) * q3;
            const QPointF c1 = out * b1;
            const QPointF c2 = in * b2;
            a11 += QPointF::dotProduct(c1, c1);
            a12 += QPointF::dotProduct(c1, c2);
            a22 += QPointF::dotProduct(c2, c2);
            r1 += QPointF::dotProduct(c1, r);
            r2 += QPointF::dotProduct(c2, r);
        }
        const qreal det = a11 * a22 - a12 * a12;
        if (det > 1e-9 * a11 * a22 && a11 > 0 && a22 > 0) {
            const qreal fitAlpha = (r1 * a22 - r2 * a12) / det;
            const qreal fitBeta = (a11 * r2 - a12 * r1) / det;
            if (fitAlpha > 0.2 && fitAlpha < 5 && fitBeta > 0.2 && fitBeta < 5) {
                alpha = fitAlpha;
                beta = fitBeta;
            }
        }
        *handle1 = q0 + out * alpha;
        *handle2 = q3 + in * beta;
    }

    QTransform toImage;
    QTransform fromImage;
    VDeform::PointMap map;
    qreal pixelsPerUnit = 0;
    qreal step = 0;
    qreal tolerance = 0;
};

QPointF cubic(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t)
{
    const qreal s = 1 - t;
    return s * s * s * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t * p3;
}

/// whether a point of the image (pixels) is inside what moves; a null test when everything moves
VMoveSession::InsideTest insideTest(const KisShapeLayerHooks::DeformMap &map)
{
    if (map.selection) {
        KisRandomConstAccessorSP pixels = map.selection->projection()->createRandomConstAccessorNG();
        return [pixels] (const QPointF &p) mutable {
            const QPoint at = p.toPoint();
            pixels->moveTo(at.x(), at.y());
            return *pixels->oldRawData() > 127;
        };
    }
    if (!map.inside.isEmpty()) {
        const QPolygonF polygon = map.inside;
        return [polygon] (const QPointF &p) { return polygon.containsPoint(p, Qt::OddEvenFill); };
    }
    return VMoveSession::InsideTest();
}

/**
 * The perspective \p map is over \p area, when it is one: four corners
 * give it, and a grid of points checks it.
 */
bool projectiveOf(const VDeform::PointMap &map, const QRectF &area, QTransform *result)
{
    if (!map || area.isEmpty()) return false;
    const QPolygonF corners({area.topLeft(), area.topRight(), area.bottomRight(), area.bottomLeft()});
    QPolygonF mapped;
    for (const QPointF &p : corners) mapped << map(p);
    QTransform t;
    if (!QTransform::quadToQuad(corners, mapped, t)) return false;
    const qreal tolerance = 1e-3 * qMax(area.width(), area.height());
    for (int i = 0; i <= 4; i++) {
        for (int j = 0; j <= 4; j++) {
            const QPointF p(area.left() + area.width() * i / 4, area.top() + area.height() * j / 4);
            const QPointF d = t.map(p) - map(p);
            if (qAbs(d.x()) + qAbs(d.y()) > tolerance) return false;
        }
    }
    *result = t;
    return true;
}

/**
 * What a deforming stroke keeps per layer session (VMoveSession::modeData()):
 * the polygon it cuts at, and, built on the first pass, each part's sample list
 * and what the map prepared for it (Cage).
 */
struct DeformState {
    explicit DeformState(const QPolygonF &inside) : inside(inside) {}
    const QPolygonF inside;
    QMutex mutex;
    QVector<VDeform::Samples> parts;
    QVector<std::shared_ptr<void>> prepared;
};

std::shared_ptr<DeformState> stateOf(VMoveSession *session)
{
    return std::static_pointer_cast<DeformState>(session->modeData());
}

/**
 * One pass of a deforming mode on one layer. Its redo runs in the stroke's
 * worker: the first pass has the GUI thread read the layer (begin()), then
 * the targets are computed right there from the session's snapshot, and the
 * GUI thread only sets them (commit()). Stock paths are mapped in the GUI
 * thread: KoPathShape isn't safe to read elsewhere, and there are few.
 */
class DeformCommand : public VMoveCommand
{
public:
    /// \p previous: a session cut at another polygon (Cage's), which goes back before this one begins
    DeformCommand(KisShapeLayer *layer, std::shared_ptr<VMoveSession> session, const KisShapeLayerHooks::DeformMap &map,
                  std::shared_ptr<VMoveSession> previous)
        : VMoveCommand(kundo2_i18n("Transform"), layer, session),
          m_map(map),
          m_previous(previous),
          m_begin([this] (bool) { beginSession(); })
    {
    }

    void redo() override
    {
        compute();
        VMoveCommand::redo();
    }

protected:
    void move() override
    {
        VMoveSession *s = session().get();
        if (!s->layer()) return;
        endPrevious();
        // a map moving nothing leaves a layer nothing moved yet unread
        if (m_map.identity && !s->hasBegun()) {
            report(QRect());
            return;
        }
        beginSession();
        // a Fast <-> Accurate switch reaches the session (begin() takes the options once)
        s->setFast(m_map.fast);
        // redone in the GUI thread (a test), or the session began only now
        if (!m_computed) computeTargets();

        const KisShapeLayerHooks::DeformMap map = m_map;
        const QRect changed = s->commit(this, m_targets,
                                        [map] (KoPathShape *path, const QTransform &documentToImage) {
                                            VDeform::mapPath(path, documentToImage, map.map);
                                        },
                                        m_hint);
        report(changed);
        // the shapes hold them now
        m_targets.clear();
        m_computed = false;
    }

private:
    /// GUI thread: what this command changed, for the pass
    void report(const QRect &changed)
    {
        if (!m_map.changed) return;
        m_map.changed->rect |= changed | m_previousChanged;
        m_map.changed->reported = true;
    }

    /// GUI thread: the session cut at another polygon goes back
    void endPrevious()
    {
        if (!m_previous) return;
        m_previous->end(false, &m_previousChanged);
        m_previous.reset();
    }

    /// GUI thread: reads the layer, the first pass only
    void beginSession()
    {
        endPrevious();
        VMoveSession *s = session().get();
        // the inside test reads the selection: built for the first pass only
        if (!s->hasBegun() && s->layer()) {
            s->begin(VMoveSession::Options{m_map.fast, insideTest(m_map), m_map.selection, true});
        }
    }

    /// in the thread the redo runs in
    void compute()
    {
        VMoveSession *s = session().get();
        if (!s->isLive() || !s->layer()) return;
        if (m_map.identity && !s->hasBegun()) return;
        if (!s->hasBegun()) {
            if (QThread::currentThread() == qApp->thread()) {
                beginSession();
            } else {
                m_begin.start(true);
            }
        }
        if (s->hasBegun()) computeTargets();
    }

    void computeTargets()
    {
        VTrace::Scope trace("deform.compute");
        VMoveSession *s = session().get();
        const VMoveSession::Snapshot snapshot = s->snapshot();
        std::shared_ptr<DeformState> state = stateOf(s);
        if (!state) {
            state = std::make_shared<DeformState>(m_map.inside);
            s->setModeData(state);
        }
        QMutexLocker locker(&state->mutex);
        if (state->parts.size() != snapshot.curves.size()) {
            state->parts.clear();
            state->parts.reserve(snapshot.curves.size());
            for (int i = 0; i < snapshot.curves.size(); i++) {
                state->parts << VDeform::samples(snapshot.curves[i], snapshot.toImage[i]);
            }
            state->prepared = QVector<std::shared_ptr<void>>(snapshot.curves.size());
        }
        const VDeform::BatchMap map = m_map.mapAll ? m_map.mapAll : VDeform::batchOf(m_map.map);
        const bool prepares = m_map.prepare && m_map.mapPrepared;
        m_targets.resize(snapshot.curves.size());
        for (int i = 0; i < snapshot.curves.size(); i++) {
            // a map that precalculates per point (Cage) does so once per stroke and part
            const VDeform::BatchMap partMap = !prepares ? map :
                VDeform::BatchMap([this, &state, i, &map] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
                    std::shared_ptr<void> &prepared = state->prepared[i];
                    if (prepared && m_map.mapPrepared(prepared, mapped)) return;
                    prepared = m_map.prepare(points);
                    if (prepared && m_map.mapPrepared(prepared, mapped)) return;
                    map(points, mapped);
                });
            m_targets[i] = VDeform::mapped(snapshot.curves[i], state->parts[i], partMap, m_map.scaleBrushSizes);
        }
        trace.setDetail(QStringLiteral("%1 parts").arg(snapshot.curves.size()));

        // a perspective: the stand-in is the rendering drawn through it in one go
        m_hint = QTransform();
        if (KisShapeLayer *layer = s->layer()) {
            if (layer->image()) projectiveOf(m_map.map, layer->image()->bounds(), &m_hint);
        }
        m_computed = true;
    }

    const KisShapeLayerHooks::DeformMap m_map;
    std::shared_ptr<VMoveSession> m_previous;
    QRect m_previousChanged;
    KisSafeBlockingQueueConnectionProxy<bool> m_begin;
    QVector<VCurveData> m_targets;
    QTransform m_hint;
    bool m_computed = false;
};

}

VDeform::Samples VDeform::samples(const VCurveData &curves, const QTransform &toImage)
{
    Samples result;
    const LocalMap f(toImage, PointMap());
    if (f.pixelsPerUnit <= 0) return result;
    result.fromImage = f.fromImage;
    result.step = f.step;
    result.tolerance = f.tolerance;

    const int count = curves.pointCount();
    QVector<QPointF> local;
    local.reserve(count * 11);
    const QPointF h(f.step, 0);
    const QPointF v(0, f.step);
    // the point, then the derivative's probes: +x, -x, +y, -y
    for (int i = 0; i < count; i++) {
        const QPointF p = curves.positions[i];
        local << p << p + h << p - h << p + v << p - v;
    }
    result.segmentStart.fill(-1, count);
    for (int i = 0; i < count; i++) {
        const int next = curves.nextIndex(i);
        if (next < 0) continue;
        const QPointF a = curves.positions[i];
        const QPointF b = curves.positions[next];
        result.segmentStart[i] = local.size();
        // the middle of the straight line (the bend test), then five points along the segment (the fit)
        local << (a + b) / 2;
        const bool curve = curves.segment(i) == VCurveData::CurveSegment;
        for (int k = 1; k <= 5; k++) {
            const qreal t = k / 6.0;
            local << (curve ? cubic(a, curves.handlesOut[i], curves.handlesIn[next], b, t) : a + (b - a) * t);
        }
    }
    result.points.resize(local.size());
    for (int i = 0; i < local.size(); i++) result.points[i] = toImage.map(local[i]);
    return result;
}

VDeform::BatchMap VDeform::batchOf(const PointMap &map)
{
    return [map] (const QVector<QPointF> &points, QVector<QPointF> &mapped) {
        mapped.resize(points.size());
        for (int i = 0; i < points.size(); i++) mapped[i] = map(points[i]);
    };
}

VCurveData VDeform::mapped(const VCurveData &curves, const Samples &samples, const BatchMap &map, bool scaleRadii)
{
    VCurveData result = curves;
    const int count = curves.pointCount();
    if (count == 0 || !samples.isValid() || !map) return result;

    QVector<QPointF> m;
    map(samples.points, m);
    if (m.size() != samples.points.size()) return result;
    for (QPointF &p : m) p = samples.fromImage.map(p);

    const qreal step = samples.step;
    QVector<LocalMap::Derivative> derivatives(count);
    for (int i = 0; i < count; i++) {
        result.positions[i] = m[5 * i];
        derivatives[i] = LocalMap::Derivative{(m[5 * i + 1] - m[5 * i + 2]) / (2 * step),
                                              (m[5 * i + 3] - m[5 * i + 4]) / (2 * step)};
        if (scaleRadii && i < result.radii.size()) {
            result.radii[i] *= qSqrt(qAbs(derivatives[i].determinant()));
        }
    }

    // line handles sit at their points
    if (!curves.handlesOut.isEmpty()) {
        result.handlesOut = result.positions;
        result.handlesIn = result.positions;
    }

    for (int i = 0; i < count; i++) {
        const int next = curves.nextIndex(i);
        const int start = samples.segmentStart.value(i, -1);
        if (next < 0 || start < 0) continue;
        const QPointF a = curves.positions[i];
        const QPointF b = curves.positions[next];
        const QPointF *along = m.constData() + start + 1;
        if (curves.segment(i) == VCurveData::CurveSegment) {
            // handles keep their direction relative to the space around their points
            const QPointF c1 = curves.handlesOut[i];
            const QPointF c2 = curves.handlesIn[next];
            LocalMap::fitHandlesTo(result.positions[i], result.positions[next], derivatives[i] * (c1 - a),
                                   derivatives[next] * (c2 - b), along, &result.handlesOut[i], &result.handlesIn[next]);
        } else if (LocalMap::bendsPast(m[start], result.positions[i], result.positions[next], samples.tolerance)) {
            // a line the map bends: the bezier that leaves and arrives as the map does
            const QPointF third = (b - a) / 3;
            QPointF h1, h2;
            LocalMap::fitHandlesTo(result.positions[i], result.positions[next], derivatives[i] * third,
                                   -(derivatives[next] * third), along, &h1, &h2);
            result.setCurveSegment(i, h1, h2);
        }
    }
    return result;
}

VCurveData VDeform::mapped(const VCurveData &curves, const QTransform &toImage, const PointMap &map, bool scaleRadii)
{
    if (!map) return curves;
    return mapped(curves, samples(curves, toImage), batchOf(map), scaleRadii);
}

void VDeform::mapPath(KoPathShape *path, const QTransform &documentToImage, const PointMap &map)
{
    const LocalMap f(path->absoluteTransformation() * documentToImage, map);
    if (!f.isValid()) return;

    // a rectangle or ellipse becomes a plain path: its parameters can't say where it went
    if (KoParameterShape *parameter = dynamic_cast<KoParameterShape*>(path)) parameter->setParametricShape(false);

    for (int s = 0; s < path->subpathCount(); s++) {
        const int count = path->subpathPointCount(s);
        QVector<KoPathPoint*> points;
        QVector<QPointF> before;
        QVector<QPointF> before1;
        QVector<QPointF> before2;
        QVector<LocalMap::Derivative> derivatives;
        for (int i = 0; i < count; i++) {
            KoPathPoint *point = path->pointByIndex(KoPathPointIndex(s, i));
            points << point;
            before << point->point();
            before1 << point->controlPoint1();
            before2 << point->controlPoint2();
            derivatives << f.derivative(point->point());
        }
        for (int i = 0; i < count; i++) {
            KoPathPoint *point = points[i];
            const QPointF p = before[i];
            const QPointF q = f(p);
            const QPointF c1 = point->controlPoint1();
            const QPointF c2 = point->controlPoint2();
            point->setPoint(q);
            if (point->activeControlPoint1()) point->setControlPoint1(q + derivatives[i] * (c1 - p));
            if (point->activeControlPoint2()) point->setControlPoint2(q + derivatives[i] * (c2 - p));
        }
        // then each curved segment's handle lengths, fitted to the mapped segment
        const bool closedPath = path->isClosedSubpath(s);
        for (int i = 0; i < count; i++) {
            const int next = i + 1 < count ? i + 1 : (closedPath ? 0 : -1);
            if (next < 0 || next == i) continue;
            KoPathPoint *a = points[i];
            KoPathPoint *b = points[next];
            if (!a->activeControlPoint2() || !b->activeControlPoint1()) continue;
            const QPointF p0 = before[i];
            const QPointF p3 = before[next];
            const QPointF c1 = before2[i];
            const QPointF c2 = before1[next];
            QPointF h1, h2;
            f.fitHandles(a->point(), b->point(), a->controlPoint2() - a->point(), b->controlPoint1() - b->point(),
                         [p0, p3, c1, c2] (qreal t) { return cubic(p0, c1, c2, p3, t); }, &h1, &h2);
            a->setControlPoint2(h1);
            b->setControlPoint1(h2);
        }
        for (int i = 0; i < count; i++) {
            const int next = i + 1 < count ? i + 1 : (closedPath ? 0 : -1);
            if (next < 0 || next == i) continue;
            KoPathPoint *a = points[i];
            KoPathPoint *b = points[next];
            if (a->activeControlPoint2() || b->activeControlPoint1()) continue;
            if (!f.bends(before[i], before[next], a->point(), b->point())) continue;
            const QPointF third = (before[next] - before[i]) / 3;
            a->setControlPoint2(a->point() + derivatives[i] * third);
            b->setControlPoint1(b->point() - derivatives[next] * third);
        }
    }
    path->normalize();
}

KUndo2Command *VDeform::command(KisShapeLayer *layer, const KisShapeLayerHooks::DeformMap &map, std::shared_ptr<void> &session)
{
    if (!layer || !VFreehandLayer::isFreehand(layer) || !map.map) return nullptr;
    // a session cut at another polygon (Cage's cage, edited again) goes back, and a new one cuts at this
    std::shared_ptr<VMoveSession> previous;
    if (session) {
        std::shared_ptr<VMoveSession> s = std::static_pointer_cast<VMoveSession>(session);
        std::shared_ptr<DeformState> state = stateOf(s.get());
        if (state && state->inside != map.inside) {
            previous = s;
            session.reset();
        }
    }
    if (!session) {
        std::shared_ptr<VMoveSession> s = std::make_shared<VMoveSession>(layer);
        s->setModeData(std::make_shared<DeformState>(map.inside));
        session = s;
    }
    return new DeformCommand(layer, std::static_pointer_cast<VMoveSession>(session), map, previous);
}

namespace {
struct SessionEnd {
    std::shared_ptr<void> session;
    bool keep;
};
VGuiThreadCall<SessionEnd> *s_sessionEnd = nullptr;

void endSession(const SessionEnd &end)
{
    if (end.session) std::static_pointer_cast<VMoveSession>(end.session)->end(end.keep);
}
}

void VDeform::done(KisShapeLayer *layer, std::shared_ptr<void> &session, bool keep)
{
    Q_UNUSED(layer);
    if (!session) return;
    const SessionEnd end{session, keep};
    if (s_sessionEnd) {
        (*s_sessionEnd)(end);
    } else {
        endSession(end);
    }
}

void VDeform::installHooks()
{
    // made in the GUI thread, for the worker threads the strokes end in
    if (!s_sessionEnd) s_sessionEnd = new VGuiThreadCall<SessionEnd>(&endSession);
    KisShapeLayerHooks::setDeformHooks(
        [] (const KisNode *node) {
            return dynamic_cast<const KisShapeLayer*>(node) && VFreehandLayer::isFreehand(node);
        },
        &VDeform::command,
        &VDeform::done);
}
