/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VMoveSession.h"
#include "VTrace.h"

#include <QMutexLocker>
#include <QTimer>
#include <QtMath>

#include <algorithm>

#include <KoParameterShape.h>
#include <KoPathPoint.h>
#include <KoPathShape.h>
#include <KoShapeContainer.h>
#include <KoShapeGroup.h>

#include <kis_assert.h>
#include <kis_image.h>
#include <kis_shape_layer.h>

#include "VCurveShape.h"
#include "VCurveTrim.h"
#include "VStrokeShape.h"
#include "VSelectionCover.h"

namespace {

/// in Fast mode, brush strokes render when the moves pause this long
const int RenderPauseMs = 300;
/**
 * ... or twice as long as the last pass took to come, when that is longer
 * (a slow map), up to this: a longer gap is the hand resting, not a slow pass
 */
const int LongestPassMs = 1000;

QAtomicInt s_restores(0);

VStrokeShape *strokeOf(VCurveShape *shape)
{
    return dynamic_cast<VStrokeShape*>(shape);
}

/// a brush stroke keeps its rendering before an undoable move to \p next
void keepRendering(VCurveShape *shape, const VCurveData &next)
{
    if (VStrokeShape *stroke = strokeOf(shape)) stroke->keepRendering(next);
}

/// as the layer's KisImageViewConverter: the image resolution, in pixels per point
QTransform documentToImageOf(KisShapeLayer *layer)
{
    KisImageSP image = layer->image();
    return image ? QTransform::fromScale(image->xRes(), image->yRes()) : QTransform();
}

}

struct VMoveSession::CurvePart {
    VCurveShape *shape = nullptr;
    QTransform toImage;
    VCurveData before;
    VCurveData moving;     ///< what moves: all of \p before, or the part inside
    VCurveData outside;    ///< what stays, when the shape is cut
    VCurveShape *piece = nullptr; ///< a cut shape: holds \p moving while it moves, and \p shape keeps \p outside
    /**
     * The session owns \p piece: it took it out of the layer, or never put it
     * in. Once in the layer, whoever takes it out owns it (a delete command
     * after the stroke), so a parentless piece isn't necessarily the session's.
     */
    bool pieceOwned = false;
    VCurveData after;      ///< where the moving shape ended (end() keeping the change)
    bool moved = false;

    VCurveShape *movingShape() const { return piece ? piece : shape; }
};

struct VMoveSession::PathPointState {
    QPointF point;
    QPointF controlPoint1;
    QPointF controlPoint2;
    bool active1 = false;
    bool active2 = false;
    KoPathPoint::PointProperties properties;
};

struct VMoveSession::PathPart {
    KoPathShape *shape = nullptr;
    QTransform transformation;
    PathState before;
    bool parametric = false;
    QTransform afterTransformation;
    PathState after;
};

VMoveSession::VMoveSession(KisShapeLayer *layer)
    : m_layer(layer)
{
}

VMoveSession::~VMoveSession()
{
    // only the pieces the session owns: one the layer had may be another
    // command's now, or deleted already
    for (const CurvePart &part : m_curves) {
        if (part.piece && part.pieceOwned) delete part.piece;
    }
    if (m_pause) {
        if (QThread::currentThread() == m_pause->thread()) {
            delete m_pause.data();
        } else {
            m_pause->deleteLater();
        }
    }
}

int VMoveSession::restoreCount()
{
    return s_restores.loadAcquire();
}

void VMoveSession::begin(const Options &options)
{
    QMutexLocker locker(&m_mutex);
    if (m_begun || !m_layer) return;
    KIS_SAFE_ASSERT_RECOVER_NOOP(QThread::currentThread() == qApp->thread());
    m_begun = true;
    m_options = options;
    m_documentToImage = documentToImageOf(m_layer);
    readShapes(m_layer->shapes(), m_documentToImage);

    if (m_options.fast) makePauseTimer();
}

void VMoveSession::makePauseTimer()
{
    if (m_pause) return;
    QTimer *timer = new QTimer();
    timer->setSingleShot(true);
    timer->setInterval(RenderPauseMs);
    const std::weak_ptr<VMoveSession> self = weak_from_this();
    QObject::connect(timer, &QTimer::timeout, [self] () {
        if (std::shared_ptr<VMoveSession> session = self.lock()) session->pauseEnded();
    });
    m_pause = timer;
}

bool VMoveSession::isFast() const
{
    QMutexLocker locker(&m_mutex);
    return m_options.fast;
}

void VMoveSession::setFast(bool fast)
{
    QMutexLocker locker(&m_mutex);
    if (m_options.fast == fast || !m_live) return;
    m_options.fast = fast;
    if (fast) {
        makePauseTimer();
    } else if (m_pause) {
        m_pause->stop();
        locker.unlock();
        pauseEnded();
    }
}

void VMoveSession::readShapes(const QList<KoShape*> &shapes, const QTransform &documentToImage)
{
    const InsideTest &inside = m_options.inside;
    for (KoShape *shape : shapes) {
        if (KoShapeGroup *group = dynamic_cast<KoShapeGroup*>(shape)) {
            readShapes(group->shapes(), documentToImage);
        } else if (!shape->isShapeEditable()) {
            continue;
        } else if (VCurveShape *curve = dynamic_cast<VCurveShape*>(shape)) {
            readCurve(curve, documentToImage);
        } else if (KoPathShape *path = dynamic_cast<KoPathShape*>(shape)) {
            if (!m_options.paths) continue;
            if (!inside) {
                readPath(path);
            } else if (m_options.selection) {
                if (VSelectionCover::coveredBySelection(path, m_options.selection, documentToImage)) readPath(path);
            } else {
                const QPolygonF outline = (path->absoluteTransformation() * documentToImage).map(path->outline().toFillPolygon());
                if (std::all_of(outline.begin(), outline.end(), inside)) readPath(path);
            }
        }
    }
}

void VMoveSession::readCurve(VCurveShape *shape, const QTransform &documentToImage)
{
    CurvePart part;
    part.shape = shape;
    part.toImage = shape->absoluteTransformation() * documentToImage;
    part.before = shape->curves();
    if (part.before.isEmpty()) return;

    const InsideTest &inside = m_options.inside;
    if (!inside) {
        part.moving = part.before;
        m_curves << part;
        return;
    }

    const qreal pixelsPerUnit = qSqrt(qAbs(part.toImage.determinant()));
    if (pixelsPerUnit <= 0) return;
    const QTransform toImage = part.toImage;
    const InsideTest in = [&inside, toImage] (const QPointF &p) { return inside(toImage.map(p)); };
    const InsideTest out = [&in] (const QPointF &p) { return !in(p); };
    const qreal step = 1.0 / pixelsPerUnit;

    for (int c = 0; c < part.before.curveCount(); c++) {
        const VCurveData curve = part.before.curve(c);
        const QVector<VCurveData> inPieces = VCurveTrim::keepInside(curve, in, step);
        const bool allIn = inPieces.size() == 1 && inPieces.first() == curve;
        const QVector<VCurveData> outPieces = allIn ? QVector<VCurveData>() : VCurveTrim::keepInside(curve, out, step);
        for (const VCurveData &piece : inPieces) part.moving.append(piece);
        for (const VCurveData &piece : outPieces) part.outside.append(piece);
        // both sides of a line too short for its width would count as slivers: it goes where it starts
        if (inPieces.isEmpty() && outPieces.isEmpty() && curve.pointCount() > 0) {
            (in(curve.positions.first()) ? part.moving : part.outside).append(curve);
        }
    }
    if (part.moving.isEmpty()) return;
    if (!part.outside.isEmpty()) {
        part.piece = dynamic_cast<VCurveShape*>(shape->cloneShape());
        KIS_SAFE_ASSERT_RECOVER_RETURN(part.piece);
        part.pieceOwned = true;
        part.piece->setCurves(part.moving);
    }
    m_curves << part;
}

void VMoveSession::readPath(KoPathShape *shape)
{
    PathPart part;
    part.shape = shape;
    part.transformation = shape->transformation();
    if (KoParameterShape *parameter = dynamic_cast<KoParameterShape*>(shape)) part.parametric = parameter->isParametricShape();
    part.before = pathState(shape);
    m_paths << part;
}

VMoveSession::PathState VMoveSession::pathState(KoPathShape *shape)
{
    PathState result;
    for (int s = 0; s < shape->subpathCount(); s++) {
        QVector<PathPointState> points;
        for (int i = 0; i < shape->subpathPointCount(s); i++) {
            const KoPathPoint *point = shape->pointByIndex(KoPathPointIndex(s, i));
            points << PathPointState{point->point(), point->controlPoint1(), point->controlPoint2(),
                                     point->activeControlPoint1(), point->activeControlPoint2(), point->properties()};
        }
        result << points;
    }
    return result;
}

void VMoveSession::setPathState(KoPathShape *shape, const PathState &state, const QTransform &transformation, bool parametric)
{
    const QRectF before = shape->boundingRect();
    for (int s = 0; s < state.size() && s < shape->subpathCount(); s++) {
        const QVector<PathPointState> &points = state[s];
        for (int i = 0; i < points.size() && i < shape->subpathPointCount(s); i++) {
            KoPathPoint *point = shape->pointByIndex(KoPathPointIndex(s, i));
            const PathPointState &p = points[i];
            point->setPoint(p.point);
            if (p.active1) point->setControlPoint1(p.controlPoint1); else point->removeControlPoint1();
            if (p.active2) point->setControlPoint2(p.controlPoint2); else point->removeControlPoint2();
            point->setProperties(p.properties);
        }
    }
    if (KoParameterShape *parameter = dynamic_cast<KoParameterShape*>(shape)) parameter->setParametricShape(parametric);
    shape->setTransformation(transformation);
    shape->updateAbsolute(before);
    shape->update();
}

void VMoveSession::apply(const VMoveCommand *command, const CurveTarget &curves, const PathTarget &paths, const QTransform &hint)
{
    VTrace::Scope trace("session.apply");
    QMutexLocker locker(&m_mutex);
    if (!m_layer || !m_live) return;
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_begun);

    trace.setDetail(QStringLiteral("%1 curves, %2 paths").arg(m_curves.size()).arg(m_paths.size()));
    moveParts(command,
              [&curves] (int, const CurvePart &part, const VCurveData &now) {
                  return curves ? curves(part.moving, now, part.toImage) : now;
              },
              paths, hint);
}

VMoveSession::Snapshot VMoveSession::snapshot() const
{
    QMutexLocker locker(&m_mutex);
    Snapshot result;
    result.curves.reserve(m_curves.size());
    result.toImage.reserve(m_curves.size());
    for (const CurvePart &part : m_curves) {
        result.curves << part.moving;
        result.toImage << part.toImage;
    }
    return result;
}

QRect VMoveSession::commit(const VMoveCommand *command, const QVector<VCurveData> &targets, const PathTarget &paths,
                           const QTransform &hint)
{
    VTrace::Scope trace("session.commit");
    QMutexLocker locker(&m_mutex);
    if (!m_layer || !m_live) return QRect();
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_begun, QRect());
    KIS_SAFE_ASSERT_RECOVER_NOOP(QThread::currentThread() == qApp->thread());

    trace.setDetail(QStringLiteral("%1 curves, %2 paths").arg(m_curves.size()).arg(m_paths.size()));
    const QRectF changed = moveParts(command,
                                     [&targets] (int index, const CurvePart &, const VCurveData &now) {
                                         return index < targets.size() ? targets[index] : now;
                                     },
                                     paths, hint);
    if (changed.isEmpty()) return QRect();
    // a pixel more: antialiased edges
    return m_documentToImage.mapRect(changed).toAlignedRect().adjusted(-1, -1, 1, 1);
}

std::shared_ptr<void> VMoveSession::modeData() const
{
    QMutexLocker locker(&m_mutex);
    return m_modeData;
}

void VMoveSession::setModeData(const std::shared_ptr<void> &data)
{
    QMutexLocker locker(&m_mutex);
    m_modeData = data;
}

QRectF VMoveSession::moveParts(const VMoveCommand *command,
                               const std::function<VCurveData(int, const CurvePart &, const VCurveData &)> &target,
                               const PathTarget &paths, const QTransform &hint)
{
    QRectF changed;
    for (int i = 0; i < m_curves.size(); i++) {
        CurvePart &part = m_curves[i];
        VCurveShape *moving = part.movingShape();
        if (part.piece && part.pieceOwned) {
            changed |= part.shape->boundingRect();
            part.shape->setCurvesAndUpdate(part.outside);
            changed |= part.shape->boundingRect();
            part.shape->parent()->addShape(part.piece);
            part.pieceOwned = false;
            changed |= part.piece->boundingRect();
        }
        const VCurveData now = moving->curves();
        const VCurveData next = target(i, part, now);
        // the same curves aren't set: that would make a new revision, and a render
        if (next == now) continue;
        if (!part.moved) {
            // the rendering before the stroke stays for an undo
            if (!part.piece) keepRendering(part.shape, next);
            part.moved = true;
        }
        if (VStrokeShape *stroke = strokeOf(moving)) stroke->setMotion(VStrokeShape::Motion{m_options.fast, hint});
        // the shape's bounds hold its renderings too, so its old ink is in
        changed |= moving->boundingRect();
        moving->setCurvesAndUpdate(next);
        changed |= moving->boundingRect();
    }

    if (paths) {
        for (const PathPart &part : m_paths) {
            // where it shows now
            changed |= part.shape->boundingRect();
            // from where it was: a path changes its transformation as it normalizes
            if (m_pathsMoved) setPathState(part.shape, part.before, part.transformation, part.parametric);
            const QRectF before = part.shape->boundingRect();
            changed |= before;
            paths(part.shape, m_documentToImage);
            part.shape->updateAbsolute(before);
            part.shape->update();
            changed |= part.shape->boundingRect();
        }
        m_pathsMoved |= !m_paths.isEmpty();
    }

    if (command) m_applied = command;
    if (m_pause) {
        // a pass slower than the pause would let the strokes render between passes, mid-drag
        const qint64 gap = m_sinceMove.isValid() ? m_sinceMove.elapsed() : 0;
        m_pause->setInterval(gap <= LongestPassMs ? qMax(RenderPauseMs, int(2 * gap)) : RenderPauseMs);
        m_pause->start();
    }
    m_sinceMove.start();
    return changed;
}

void VMoveSession::adopt(const VMoveCommand *command)
{
    QMutexLocker locker(&m_mutex);
    if (m_live && command) m_applied = command;
}

void VMoveSession::pauseEnded()
{
    QMutexLocker locker(&m_mutex);
    if (!m_layer || !m_live) return;
    for (const CurvePart &part : m_curves) {
        if (!part.moved) continue;
        if (VStrokeShape *stroke = strokeOf(part.movingShape())) stroke->clearMotion();
    }
}

QRectF VMoveSession::restore()
{
    s_restores.ref();
    QRectF changed;
    for (CurvePart &part : m_curves) {
        if (part.piece) {
            if (!part.pieceOwned) {
                if (KoShapeContainer *parent = part.piece->parent()) {
                    changed |= part.piece->boundingRect();
                    part.piece->update();
                    parent->removeShape(part.piece);
                }
                part.pieceOwned = true;
            }
        } else if (part.moved) {
            keepRendering(part.shape, part.before);
        }
        if (part.shape->curves() != part.before) {
            changed |= part.shape->boundingRect();
            part.shape->setCurvesAndUpdate(part.before);
            changed |= part.shape->boundingRect();
        }
    }
    for (const PathPart &part : m_paths) {
        if (!m_pathsMoved) continue;
        changed |= part.shape->boundingRect();
        setPathState(part.shape, part.before, part.transformation, part.parametric);
        changed |= part.shape->boundingRect();
    }
    return changed;
}

void VMoveSession::forward()
{
    for (CurvePart &part : m_curves) {
        if (!part.moved) continue;
        if (part.piece) {
            if (part.pieceOwned) {
                part.shape->setCurvesAndUpdate(part.outside);
                part.shape->parent()->addShape(part.piece);
                part.pieceOwned = false;
                part.piece->update();
            }
        } else if (part.shape->curves() != part.after) {
            keepRendering(part.shape, part.after);
            part.shape->setCurvesAndUpdate(part.after);
        }
    }
    for (const PathPart &part : m_paths) {
        if (m_pathsMoved) setPathState(part.shape, part.after, part.afterTransformation, false);
    }
}

void VMoveSession::undo(const VMoveCommand *command)
{
    QMutexLocker locker(&m_mutex);
    // live: the next pass moves on from the same start
    if (m_live || !m_layer) return;
    if (command != m_applied || !m_keptApplied) return;
    restore();
    m_keptApplied = false;
}

void VMoveSession::redo(const VMoveCommand *command)
{
    QMutexLocker locker(&m_mutex);
    if (m_live || !m_layer) return;
    if (command != m_applied || m_keptApplied) return;
    forward();
    m_keptApplied = true;
}

void VMoveSession::end(bool keep, QRect *changed)
{
    if (changed) *changed = QRect();
    QMutexLocker locker(&m_mutex);
    if (!m_live) return;
    m_live = false;
    if (m_pause) m_pause->stop();
    if (!m_layer) return;

    auto clearMotions = [this] () {
        for (const CurvePart &part : m_curves) {
            if (!part.moved) continue;
            if (VStrokeShape *stroke = strokeOf(part.movingShape())) stroke->clearMotion();
            if (part.piece) {
                if (VStrokeShape *stroke = strokeOf(part.shape)) stroke->clearMotion();
            }
        }
    };

    // keeping a moved session needs the command that moved it; without one the
    // shapes go back below. A warning, not an assert: a live preview may have
    // moved them before the pass's command was adopted
    if (keep && !m_applied && (m_pathsMoved || std::any_of(m_curves.begin(), m_curves.end(),
                                                           [] (const CurvePart &part) { return part.moved; }))) {
        qWarning() << "VMoveSession::end(true): moved shapes but no command applied them; restoring";
    }

    if (keep && m_applied) {
        clearMotions();
        for (CurvePart &part : m_curves) {
            if (part.moved && !part.piece) part.after = part.shape->curves();
        }
        for (PathPart &part : m_paths) {
            part.after = pathState(part.shape);
            part.afterTransformation = part.shape->transformation();
        }
        m_keptApplied = true;
    } else {
        // Nothing renders until everything is back. The layer repaints in
        // image threads meanwhile: unpaused before restore(), such a paint
        // renders the pose being dropped, or the curves going back before
        // their kept rendering is in the cache again
        for (const CurvePart &part : m_curves) {
            if (!part.moved) continue;
            for (VCurveShape *shape : {part.movingShape(), part.shape}) {
                if (VStrokeShape *stroke = strokeOf(shape)) {
                    stroke->setMotion(VStrokeShape::Motion{true, stroke->motion().hint});
                }
            }
        }
        const QRectF area = restore();
        if (changed && !area.isEmpty()) {
            *changed = m_documentToImage.mapRect(area).toAlignedRect().adjusted(-1, -1, 1, 1);
        }
        clearMotions();
        m_applied = nullptr;
        m_keptApplied = false;
    }
}

void VMoveSession::forget(const VMoveCommand *command)
{
    QMutexLocker locker(&m_mutex);
    if (m_applied != command) return;
    m_applied = nullptr;
    m_keptApplied = false;
}

VMoveCommand::VMoveCommand(const KUndo2MagicString &text, KisShapeLayer *layer, std::shared_ptr<VMoveSession> session)
    : VGuiThreadCommand(text),
      m_layer(layer),
      m_session(session)
{
}

VMoveCommand::~VMoveCommand()
{
    m_session->forget(this);
}

void VMoveCommand::apply(bool redo)
{
    if (!redo) {
        m_session->undo(this);
    } else if (m_session->isLive()) {
        move();
    } else {
        m_session->redo(this);
    }
}
