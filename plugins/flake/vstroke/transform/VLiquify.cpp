/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VLiquify.h"
#include "VTrace.h"

#include <QApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <algorithm>

#include <kis_image.h>
#include <kis_shape_layer.h>
#include <klocalizedstring.h>
#include <kis_assert.h>

#include "VCurveShape.h"
#include "VFreehandLayer.h"
#include "VGuiThreadCommand.h"
#include "VMoveSession.h"

namespace {

QVector<VCurveWarp::Dab> warpDabs(const QVector<KisShapeLayerHooks::LiquifyDab> &dabs)
{
    QVector<VCurveWarp::Dab> result;
    result.reserve(dabs.size());
    for (const KisShapeLayerHooks::LiquifyDab &dab : dabs) {
        result << VCurveWarp::Dab{dab.position, dab.offset, dab.sigma, dab.wash, dab.flow};
    }
    return result;
}

/// the curves moved by all of \p dabs from where they were before the stroke
VMoveSession::CurveTarget allDabs(const QVector<VCurveWarp::Dab> &dabs)
{
    return [dabs] (const VCurveData &before, const VCurveData &, const QTransform &toImage) {
        return dabs.isEmpty() ? before : VCurveWarp::translated(before, dabs, toImage);
    };
}

/**
 * The Liquify stroke of a layer, from its first command or preview to its
 * end (LiquifyPreviewDone): its session, and the dabs the curves show.
 */
struct Stroke {
    std::shared_ptr<VMoveSession> session;
    /// the dabs the curves show
    QVector<VCurveWarp::Dab> applied;
    /// the live preview moved the curves: the commands find them moved already
    bool previewed = false;
};
QMutex s_strokesMutex;
QHash<KisShapeLayer*, Stroke> s_strokes;

/// the stroke of \p layer, a new one when the last has ended (any thread)
std::shared_ptr<VMoveSession> sessionOf(KisShapeLayer *layer)
{
    QMutexLocker locker(&s_strokesMutex);
    Stroke &stroke = s_strokes[layer];
    if (!stroke.session || !stroke.session->layer() || (stroke.session->hasBegun() && !stroke.session->isLive())) {
        stroke = Stroke();
        stroke.session = std::make_shared<VMoveSession>(layer);
    }
    return stroke.session;
}

/// ends the stroke of \p layer, in the GUI thread (KisShapeLayerHooks::LiquifyPreviewDone)
void endStroke(KisShapeLayer *layer, bool keep)
{
    Stroke stroke;
    {
        QMutexLocker locker(&s_strokesMutex);
        if (!s_strokes.contains(layer)) return;
        stroke = s_strokes.take(layer);
    }
    if (!stroke.session) return;
    VMoveSession *session = stroke.session.get();
    if (!session->hasBegun()) session->begin(VMoveSession::Options());
    // the last command's dabs: the live preview may have gone further
    const VLiquifyCommand *last = dynamic_cast<const VLiquifyCommand*>(session->lastCommand());
    if (keep && last && last->dabs() != stroke.applied) {
        session->apply(last, allDabs(last->dabs()), VMoveSession::PathTarget(), QTransform());
    }
    session->end(keep);
}

struct SessionEnd {
    KisShapeLayer *layer;
    bool keep;
};
VGuiThreadCall<SessionEnd> *s_sessionEnd = nullptr;
}

VLiquifyCommand::VLiquifyCommand(KisShapeLayer *layer, const QVector<KisShapeLayerHooks::LiquifyDab> &dabs, quint64 stroke)
    : VMoveCommand(kundo2_i18n("Liquify"), layer, sessionOf(layer)),
      m_dabs(warpDabs(dabs))
{
    Q_UNUSED(stroke);
}

void VLiquifyCommand::move()
{
    VMoveSession *s = session().get();
    if (!s->hasBegun()) s->begin(VMoveSession::Options());
    bool previewed = false;
    {
        QMutexLocker locker(&s_strokesMutex);
        auto it = s_strokes.find(s->layer());
        if (it != s_strokes.end() && it->session.get() == s) {
            previewed = it->previewed;
            if (!previewed) it->applied = m_dabs;
        }
    }
    // the live preview moved the curves already, and further than these dabs
    // when moves came after this pass was queued: taking them back would
    // move the curves backward. The stroke's end brings the last command's dabs
    if (previewed) {
        s->adopt(this);
    } else {
        s->apply(this, allDabs(m_dabs), VMoveSession::PathTarget(), QTransform());
    }
}

void VLiquify::installHooks()
{
    KisShapeLayerHooks::setLiquifyHooks(
        [] (const KisNode *node) {
            return dynamic_cast<const KisShapeLayer*>(node) && VFreehandLayer::isFreehand(node);
        },
        [] (KisShapeLayer *layer, const QVector<KisShapeLayerHooks::LiquifyDab> &dabs, quint64 stroke) -> KUndo2Command* {
            return new VLiquifyCommand(layer, dabs, stroke);
        });
    // made in the GUI thread, for the worker threads the strokes end in
    if (!s_sessionEnd) {
        s_sessionEnd = new VGuiThreadCall<SessionEnd>([] (SessionEnd end) {
            endStroke(end.layer, end.keep);
        });
    }
    KisShapeLayerHooks::setLiquifyPreview(&VLiquify::preview, [] (KisShapeLayer *layer, bool keep) {
        (*s_sessionEnd)(SessionEnd{layer, keep});
    });
}

void VLiquify::preview(KisShapeLayer *layer, const QVector<KisShapeLayerHooks::LiquifyDab> &liquifyDabs, bool fast)
{
    VTrace::Scope trace("liquify.preview");
    KIS_SAFE_ASSERT_RECOVER_RETURN(QThread::currentThread() == qApp->thread());
    const std::shared_ptr<VMoveSession> session = sessionOf(layer);
    VMoveSession::Options options;
    options.fast = fast;
    session->begin(options);
    session->setFast(fast);

    const QVector<VCurveWarp::Dab> dabs = warpDabs(liquifyDabs);
    QVector<VCurveWarp::Dab> applied;
    {
        QMutexLocker locker(&s_strokesMutex);
        Stroke &stroke = s_strokes[layer];
        applied = stroke.applied;
        stroke.applied = dabs;
        stroke.previewed = true;
    }
    const bool continues = dabs.size() >= applied.size() &&
        std::equal(applied.constBegin(), applied.constEnd(), dabs.constBegin());
    if (continues && dabs.size() == applied.size()) return;

    if (continues) {
        // only the new dabs, on the curves as they are
        const int from = applied.size();
        session->apply(nullptr,
                       [dabs, from] (const VCurveData &before, const VCurveData &now, const QTransform &toImage) {
                           VCurveData moved = now;
                           VCurveWarp::translateFrom(moved, dabs, from, toImage, before);
                           return moved;
                       },
                       VMoveSession::PathTarget(), QTransform());
    } else {
        // fewer dabs (an in-tool undo) or others: each curve goes to the new
        // dabs from where the stroke found it, in one step. A curve that ends
        // where it is isn't set at all (no new revision, no render)
        session->apply(nullptr, allDabs(dabs), VMoveSession::PathTarget(), QTransform());
    }
}
