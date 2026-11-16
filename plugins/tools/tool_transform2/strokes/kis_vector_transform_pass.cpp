/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_vector_transform_pass.h"

#include <kis_external_layer_iface.h>
#include <kis_projection_updates_filter.h>
#include <kis_shape_layer.h>
#include <kis_image_interfaces.h>
#include <kis_abstract_projection_plane.h>
#include <commands_new/KisDisableDirtyRequestsCommand.h>
#include <kundo2command.h>
#include <kis_assert.h>

#include "kis_transform_utils.h"

namespace {

/**
 * Drops the updates of every node but \p passed: vector layers whose shapes
 * the Liquify preview moves itself (KisShapeLayerHooks) in the GUI thread
 * between passes. They repaint on their own, and a repaint dropped while a
 * pass runs would leave the canvas stale.
 */
class DropAllButProjectionUpdatesFilter : public KisProjectionUpdatesFilter
{
public:
    DropAllButProjectionUpdatesFilter(const QSet<KisNode*> &passed) : m_passed(passed) {}

    bool filter(KisImage *, KisNode *node, const QVector<QRect> &, KisProjectionUpdateFlags) override {
        return !m_passed.contains(node);
    }
    bool filterRefreshGraph(KisImage *, KisNode *node, const QVector<QRect> &, const QRect &, KisProjectionUpdateFlags) override {
        return !m_passed.contains(node);
    }

private:
    QSet<KisNode*> m_passed;
};

/// KisDisableDirtyRequestsCommand with DropAllButProjectionUpdatesFilter
class DisableDirtyRequestsButCommand : public KisCommandUtils::FlipFlopCommand
{
public:
    DisableDirtyRequestsButCommand(KisUpdatesFacade *updatesFacade, const QSet<KisNode*> &passed,
                                   QSharedPointer<KisVectorTransformPass::FilterCookies> cookies, State state)
        : KisCommandUtils::FlipFlopCommand(state),
          m_updatesFacade(updatesFacade),
          m_passed(passed),
          m_cookies(cookies)
    {
    }

    void partA() override {
        QMutexLocker locker(&m_cookies->mutex);
        m_cookies->cookies.push(
            m_updatesFacade->addProjectionUpdatesFilter(toQShared(new DropAllButProjectionUpdatesFilter(m_passed))));
    }
    void partB() override {
        QMutexLocker locker(&m_cookies->mutex);
        KIS_SAFE_ASSERT_RECOVER_RETURN(!m_cookies->cookies.isEmpty());
        m_updatesFacade->removeProjectionUpdatesFilter(m_cookies->cookies.pop());
    }

private:
    KisUpdatesFacade *m_updatesFacade;
    QSet<KisNode*> m_passed;
    QSharedPointer<KisVectorTransformPass::FilterCookies> m_cookies;
};

QRect boundsOf(KisExternalLayer *layer)
{
    return layer->projectionPlane()->tightUserVisibleBounds() | layer->theoreticalBoundingRect();
}

}

void KisVectorTransformPass::init(ToolTransformArgs::TransformMode mode, const KisNodeList &processedNodes,
                                  bool hasSelection, bool hasExternalSource)
{
    m_liquifyPreviewNodes.clear();
    m_liquifyIncrementalAllowed = false;
    if (mode != ToolTransformArgs::LIQUIFY) return;

    m_liquifyIncrementalAllowed = !hasSelection && !hasExternalSource;
    Q_FOREACH (KisNodeSP node, processedNodes) {
        if (KisShapeLayerHooks::isLiquifyTarget(node.data())) {
            m_liquifyPreviewNodes.insert(node.data());
            m_liquifyIncrementalAllowed &= KisShapeLayerHooks::hasLiquifyPreview();
        } else if (!node->inherits("KisPaintLayer")) {
            m_liquifyIncrementalAllowed = false;
        }
    }
}

bool KisVectorTransformPass::forcesFullResolution(ToolTransformArgs::TransformMode mode, const KisNodeList &processedNodes) const
{
    // the shapes of the vector layers Liquify moves itself repaint at full
    // resolution only, so a reduced preview would show them where they were
    if (!m_liquifyPreviewNodes.isEmpty()) return true;
    Q_FOREACH (KisNodeSP node, processedNodes) {
        if (KisTransformUtils::vectorForcesFullResolution(mode, node.data())) return true;
    }
    return false;
}

bool KisVectorTransformPass::transformNode(KisNodeSP node, const ToolTransformArgs &config, const Sink &sink)
{
    KisShapeLayer *layer = dynamic_cast<KisShapeLayer*>(node.data());
    if (!layer) return false;

    KUndo2Command *command = nullptr;
    if (config.mode() == ToolTransformArgs::LIQUIFY && KisShapeLayerHooks::isLiquifyTarget(layer)) {
        // the shapes move by the dabs in full resolution
        const bool livePreview = KisShapeLayerHooks::hasLiquifyPreview();
        // with the live preview the GUI thread moves the shapes meanwhile: their
        // bounds aren't read here, in a worker thread
        const QRect oldRect = livePreview ? QRect() : boundsOf(layer);
        command = KisShapeLayerHooks::liquifyCommand(layer, config.liquifyDabs(), m_liquifyStroke);
        if (command) sink.execute(command);
        // with the live preview, the shapes moved already in the GUI thread
        // (KisToolTransform::requestImageRecalculation()), and the layer
        // repaints on its own: its updates pass the pass's filter
        if (livePreview) return true;
        layer->forceUpdateTimedNode();
        sink.addDirtyRect(oldRect | boundsOf(layer));
        return true;
    }

    if (KisTransformUtils::isVectorDeformTarget(config.mode(), layer)) {
        // the shapes move by the mode's map in full resolution
        KisShapeLayerHooks::DeformMap map = KisTransformUtils::vectorDeformMap(config, m_selectionMoves.selection());
        map.fast = m_fast;
        // the plugin reports what its shapes covered, from the GUI thread where they move:
        // the layer's bounds aren't read here meanwhile
        map.changed = std::make_shared<KisShapeLayerHooks::DeformMap::ChangedArea>();
        command = m_deforms.command(layer, map);
        if (command) sink.execute(command);
        // the repaint is queued now, so the pass's update finds it under way
        layer->forceUpdateTimedNode();
        sink.addDirtyRect(map.changed->reported ? map.changed->rect : boundsOf(layer));
        return true;
    }
    return false;
}

void KisVectorTransformPass::freeTransform(KisExternalLayer *layer, const ToolTransformArgs &config, const QTransform &transform, const Sink &sink)
{
    KisShapeLayer *partLayer = config.mode() == ToolTransformArgs::FREE_TRANSFORM ? dynamic_cast<KisShapeLayer*>(layer) : nullptr;
    if (KUndo2Command *partCommand = partLayer ? m_selectionMoves.command(partLayer, transform, config.scaleBrushSizes()) : nullptr) {
        sink.execute(partCommand);
        return;
    }
    sink.execute(layer->transform(transform));
    if (partLayer && config.scaleBrushSizes()) {
        if (KUndo2Command *brushCommand = KisShapeLayerHooks::scaleBrushSizes(partLayer, transform)) {
            sink.execute(brushCommand);
        }
    }
}

KUndo2Command *KisVectorTransformPass::disableDirtyRequestsCommand(KisUpdatesFacade *updatesFacade,
                                                                   KisCommandUtils::FlipFlopCommand::State state) const
{
    if (KisShapeLayerHooks::hasLiquifyPreview() && !m_liquifyPreviewNodes.isEmpty()) {
        return new DisableDirtyRequestsButCommand(updatesFacade, m_liquifyPreviewNodes, m_filterCookies, state);
    }
    return new KisDisableDirtyRequestsCommand(updatesFacade, state);
}

void KisVectorTransformPass::finish(bool keep)
{
    // on a fresh stroke's cancel this runs twice (the cancel barrier, then
    // finalizeStrokeImpl()): the second finds no sessions and does nothing
    m_deforms.finish(keep);
    if (KisShapeLayerHooks::hasLiquifyPreview()) {
        Q_FOREACH (KisNode *node, m_liquifyPreviewNodes) {
            KisShapeLayerHooks::liquifyPreviewDone(static_cast<KisShapeLayer*>(node), keep);
        }
    }
}
