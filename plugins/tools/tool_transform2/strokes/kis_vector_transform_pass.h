/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VECTOR_TRANSFORM_PASS_H
#define KIS_VECTOR_TRANSFORM_PASS_H

#include <functional>

#include <QMutex>
#include <QRect>
#include <QSet>
#include <QSharedPointer>
#include <QStack>
#include <QTransform>

#include <KisShapeLayerHooks.h>
#include <kis_command_utils.h>
#include <kis_types.h>

#include "tool_transform_args.h"

class KisExternalLayer;
class KisUpdatesFacade;
class KUndo2Command;

/**
 * Everything one transform stroke does with the vector layers a plugin
 * moves itself (KisShapeLayerHooks): Liquify, the deforming modes
 * (Perspective...), and Free Transform's selection part and brush sizes.
 * Both strokes (in-place and overlay) hold one and call it from their
 * stock code, which keeps one-line calls.
 *
 * Every pass of a vector layer follows one contract: run the plugin's
 * command, force the layer's repaint (KisShapeLayer::forceUpdateTimedNode()),
 * and add the old and new bounds as dirty. The layer's updates are dropped
 * during the pass like any layer's, except for the layers the Liquify live
 * preview moves in the GUI thread between passes.
 */
class KisVectorTransformPass
{
public:
    /// how the stroke keeps what a pass does with one layer
    struct Sink {
        /// runs \p command and keeps it with the pass's commands
        std::function<void(KUndo2Command *command)> execute;
        /// \p rect, in full-resolution image pixels, needs an update
        std::function<void(const QRect &rect)> addDirtyRect;
    };

    /// the layers of the stroke, after the stroke has fetched them (in initStrokeCallback())
    void init(ToolTransformArgs::TransformMode mode, const KisNodeList &processedNodes, bool hasSelection, bool hasExternalSource);
    /// the transform tool's Fast preview (set before the stroke starts)
    void setFastPreview(bool fast) { m_fast = fast; }
    bool isFastPreview() const { return m_fast; }
    /// the selection as the stroke starts, after continuing brings back an earlier one
    void setSelection(KisSelectionSP selection) { m_selectionMoves.setSelection(selection); }

    /// the vector layers repaint at full resolution only: a reduced preview would show them wrong
    bool forcesFullResolution(ToolTransformArgs::TransformMode mode, const KisNodeList &processedNodes) const;
    /// whether the incremental Liquify preview may run (the vector layers allow it)
    bool liquifyIncrementalAllowed() const { return m_liquifyIncrementalAllowed; }

    /**
     * One pass of \p node, when it is a vector layer Liquify or a deforming
     * mode moves itself: true when it handled it.
     */
    bool transformNode(KisNodeSP node, const ToolTransformArgs &config, const Sink &sink);
    /**
     * Free Transform's command for \p layer by \p transform: only the selected
     * part of a vector layer that takes it, or the whole layer, with its brush
     * sizes when "Scale brush size" is on.
     */
    void freeTransform(KisExternalLayer *layer, const ToolTransformArgs &config, const QTransform &transform, const Sink &sink);

    /// the command that drops updates during a pass, but those of the layers the Liquify live preview moves
    KUndo2Command *disableDirtyRequestsCommand(KisUpdatesFacade *updatesFacade, KisCommandUtils::FlipFlopCommand::State state) const;
    /// the stroke ends (a worker thread): \p keep when it saves its commands, before they go to the undo stack
    void finish(bool keep);

    /**
     * The filters the pass's "drop updates" commands added, as
     * KisImage::disableDirtyRequests() keeps its own: each removes the one it
     * added, not whichever is on top. Shared: the adding and the removing
     * command are two objects, and both can outlive the stroke in the undo
     * history.
     */
    struct FilterCookies {
        QMutex mutex;
        QStack<KisProjectionUpdatesFilterCookie> cookies;
    };

private:
    bool m_fast = false;
    bool m_liquifyIncrementalAllowed = false;
    KisShapeLayerHooks::SelectionMoves m_selectionMoves;
    KisShapeLayerHooks::Deforms m_deforms;
    /// for the commands of vector layers Liquify moves
    const quint64 m_liquifyStroke = KisShapeLayerHooks::newLiquifyStroke();
    /// the vector layers whose shapes the Liquify preview moves itself
    QSet<KisNode*> m_liquifyPreviewNodes;
    const QSharedPointer<FilterCookies> m_filterCookies = QSharedPointer<FilterCookies>::create();
};

#endif // KIS_VECTOR_TRANSFORM_PASS_H
