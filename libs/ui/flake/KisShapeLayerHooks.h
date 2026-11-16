/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_SHAPE_LAYER_HOOKS_H
#define KIS_SHAPE_LAYER_HOOKS_H

#include <functional>
#include <memory>

#include <QHash>
#include <QList>
#include <QMutex>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <QVector>

#include <kis_types.h>
#include <kis_selection.h>

#include "kritaui_export.h"

class KoShape;
class KoShapeContainer;
class KoShapeControllerBase;
class KisNode;
class KisShapeLayer;
class KUndo2Command;

/**
 * Lets a plugin take part when shapes are added to vector layers by the
 * shape tools, paste and drag and drop: replace the shapes before they are
 * added (so the undo command holds the replacements from the start), and
 * decide whether pasted shapes need a new layer. Nothing is set in stock
 * Krita, and then every call does nothing.
 *
 * Lets a plugin give vector layers what Edit > Clear does, their icons, what
 * Copy and Cut do to them within a selection, what Free Transform moves of them
 * within a selection, and brush sizes that Free Transform can scale.
 *
 * Also lets a plugin take vector layers into the transform tool's Liquify:
 * their shapes then move by the Move and Offset dabs themselves, instead
 * of being left out. Likewise for the modes that send every point of the
 * image somewhere (Perspective, Warp, Cage, Mesh): the shapes move by that map themselves.
 */
class KRITAUI_EXPORT KisShapeLayerHooks
{
public:
    /// may replace shapes in \p shapes (deleting the ones it replaces) for \p parent, the container they go to (may be null)
    using ConvertShapes = std::function<void(QList<KoShape*> &shapes, KoShapeContainer *parent)>;
    /// whether \p shapes need a new layer although the active layer \p activeLayer is a vector layer
    using NeedsNewLayer = std::function<bool(const QList<KoShape*> &shapes, KisShapeLayer *activeLayer)>;
    /// a vector layer just created for \p shapes, before it is added to the image
    using InitNewLayer = std::function<void(KisShapeLayer *layer, const QList<KoShape*> &shapes)>;
    /// the command clearing \p layer within \p selection (may be null), or null when there is nothing to clear (GUI thread)
    using ClearCommand = std::function<KUndo2Command*(KisShapeLayer *layer, KoShapeControllerBase *controller, KisSelectionSP selection)>;
    /// the icon of \p layer; an empty name for the stock one
    using LayerIcon = std::function<QString(const KisShapeLayer *layer)>;
    /// cuts \p copy, a copy of a vector layer going to the clipboard, down to \p selection (GUI thread); false leaves it to be rasterized
    using KeepWithinSelection = std::function<bool(KisShapeLayer *copy, KisSelectionSP selection)>;
    /// the command removing from \p layer what \p selection holds, for Cut (GUI thread); null when it leaves the layer as it is
    using CutWithinSelection = std::function<KUndo2Command*(KisShapeLayer *layer, KoShapeControllerBase *controller, KisSelectionSP selection)>;
    /**
     * The command moving only what \p selection holds on \p layer by \p transform,
     * in image pixels; null leaves the layer to move as a whole. \p session starts
     * empty and stays with the transform stroke and the layer, for the plugin to
     * keep what its commands share. Called in a worker thread; the command does its
     * work in the GUI thread.
     */
    using SelectionMove = std::function<KUndo2Command*(KisShapeLayer *layer, KisSelectionSP selection, const QTransform &transform,
                                                       bool scaleBrushSizes, std::shared_ptr<void> &session)>;

    /// a transform stroke's SelectionMove: the selection as it started, and a session per layer
    class KRITAUI_EXPORT SelectionMoves
    {
    public:
        /// keeps a copy, as the stroke transforms \p selection itself in the end
        void setSelection(KisSelectionSP selection);
        /// the selection as the stroke started, or null
        KisSelectionSP selection() const { return m_selection; }
        /// null when there is no selection or no plugin moves part of \p layer
        KUndo2Command *command(KisShapeLayer *layer, const QTransform &transform, bool scaleBrushSizes);

    private:
        KisSelectionSP m_selection;
        QMutex m_mutex;
        QHash<KisShapeLayer*, std::shared_ptr<void>> m_sessions;
    };

    /// whether \p node is a vector layer whose strokes can scale their brush size
    using HasBrushSizes = std::function<bool(const KisNode *node)>;
    /// the command scaling the brush sizes of \p layer by what \p transform does to areas; runs its work in the GUI thread
    using ScaleBrushSizes = std::function<KUndo2Command*(KisShapeLayer *layer, const QTransform &transform)>;

    /// one Move or Offset dab of the transform tool's Liquify (KisLiquifyPaintop), in image pixels
    struct LiquifyDab {
        QPointF position;
        QPointF offset;
        qreal sigma = 1.0;   ///< the brush size: the weight is exp(-d² / 2σ²), up to 3σ
        bool wash = false;   ///< wash mode: toward the original position plus the offset, by \p flow
        qreal flow = 1.0;

        bool operator==(const LiquifyDab &rhs) const {
            return position == rhs.position && offset == rhs.offset && sigma == rhs.sigma &&
                   wash == rhs.wash && flow == rhs.flow;
        }
    };
    /// whether Liquify takes \p node and moves its shapes itself
    using IsLiquifyTarget = std::function<bool(const KisNode *node)>;
    /**
     * A command that moves the shapes of \p layer by \p dabs, all of them,
     * from where they were before the Liquify stroke. Called in a worker
     * thread; the command does its work in the GUI thread.
     *
     * \p stroke numbers the transform stroke: the commands of its passes
     * (the preview at a level of detail and the final one) share it, and
     * no other stroke has it. A stroke leaves the preview's last command
     * applied when it applies the final one.
     */
    using LiquifyCommand = std::function<KUndo2Command*(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, quint64 stroke)>;
    /**
     * The live preview: moves the shapes of \p layer to where \p dabs, all
     * the dabs of the stroke so far, take them. Called in the GUI thread
     * before each preview update; it may only add dabs to the last list (then
     * only the new ones need applying), or be shorter after an in-tool undo.
     * While it runs, the stroke's LiquifyCommands find the shapes moved
     * already, and the layer's own repaints reach the canvas. \p fast is the
     * transform tool's Fast preview: the plugin may hold back slow repaints
     * while the dabs keep coming.
     */
    using LiquifyPreview = std::function<void(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, bool fast)>;
    /**
     * The end of the transform stroke, for a layer the preview moved: \p keep
     * when the stroke saves its commands (the last LiquifyCommand then holds
     * the change), otherwise the shapes go back to where they were. Called
     * in a worker thread.
     */
    using LiquifyPreviewDone = std::function<void(KisShapeLayer *layer, bool keep)>;

    /**
     * Where a deforming transform mode (Perspective, Warp, Cage, Mesh) sends the image: \p map
     * takes a point of the image, in pixels, to where it goes. It's called
     * in the stroke's worker threads (and possibly the GUI thread), so it keeps
     * what it needs and reads nothing that changes.
     */
    struct DeformMap {
        std::function<QPointF(const QPointF &)> map;
        /**
         * Optional: maps all of \p points into \p mapped (resized to match) in one
         * call, as map() would one by one. Unset, the plugin calls map() per point.
         */
        std::function<void(const QVector<QPointF> &points, QVector<QPointF> &mapped)> mapAll;
        /**
         * Optional, for maps that precalculate per point (Cage): returns what
         * mapPrepared() needs to map exactly \p points (any thread). The plugin keeps
         * it for the stroke and calls prepare again only when mapPrepared refuses it.
         */
        std::function<std::shared_ptr<void>(const QVector<QPointF> &points)> prepare;
        /**
         * Maps the points \p prepared was made for into \p mapped, in their order;
         * false when \p prepared doesn't fit this map (made for another cage): the
         * plugin then prepares again. Several threads may hold the same \p prepared.
         */
        std::function<bool(const std::shared_ptr<void> &prepared, QVector<QPointF> &mapped)> mapPrepared;
        /**
         * The map moves nothing (Cage while its cage is edited): shapes go back to
         * where they were, and a layer nothing moved yet isn't read for it.
         */
        bool identity = false;
        /// what a command covered, reported by the plugin (see \c changed)
        struct ChangedArea {
            bool reported = false;
            QRect rect; ///< image pixels: where the moved shapes were and are
        };
        /**
         * Set by the caller, filled by the plugin's command as it moves the shapes
         * (in the GUI thread, before the command's redo returns), so the caller
         * needn't read the layer's bounds while shapes may move; not reported:
         * the caller measures the layer.
         */
        std::shared_ptr<ChangedArea> changed;
        /// only what is selected moves, cut at the selection's edge; null: everything
        KisSelectionSP selection;
        /// Cage: only what is inside this polygon of the image moves, cut at its edge; empty: everything
        QPolygonF inside;
        /// "Scale brush size": brush strokes scale their size by what the map does to areas there
        bool scaleBrushSizes = false;
        /// the transform tool's Fast preview: the plugin may hold back slow repaints while the map keeps changing
        bool fast = false;
    };
    /// whether the deforming modes take \p node and move its shapes themselves
    using IsDeformTarget = std::function<bool(const KisNode *node)>;
    /**
     * A command moving the shapes of \p layer by \p map, from where they were
     * before the transform stroke. Called in a worker thread; the command
     * does its work in the GUI thread. \p session starts empty and stays
     * with the transform stroke and the layer, for the plugin to keep what
     * its commands share (as SelectionMove's).
     */
    using DeformCommand = std::function<KUndo2Command*(KisShapeLayer *layer, const DeformMap &map, std::shared_ptr<void> &session)>;
    /**
     * The end of the transform stroke, for a layer with a session: \p keep
     * when the stroke saves its commands (the last DeformCommand then holds
     * the change), otherwise the shapes go back to where they were. Called
     * in a worker thread, before the stroke's commands reach the undo stack.
     */
    using DeformDone = std::function<void(KisShapeLayer *layer, std::shared_ptr<void> &session, bool keep)>;

    /// a transform stroke's DeformCommands: a session per layer
    class KRITAUI_EXPORT Deforms
    {
    public:
        /// null when no hook is set
        KUndo2Command *command(KisShapeLayer *layer, const DeformMap &map);
        /// the stroke ends (DeformDone for every session); its commands are done with the sessions
        void finish(bool keep);

    private:
        QMutex m_mutex;
        QHash<KisShapeLayer*, std::shared_ptr<void>> m_sessions;
    };

    static void setConvertShapes(ConvertShapes hook);
    static void setNeedsNewLayer(NeedsNewLayer hook);
    static void setInitNewLayer(InitNewLayer hook);
    static void setClearCommand(ClearCommand hook);
    static void setLayerIcon(LayerIcon hook);
    static void setKeepWithinSelection(KeepWithinSelection hook);
    static void setCutWithinSelection(CutWithinSelection hook);
    static void setSelectionMove(SelectionMove hook);
    static void setBrushSizeHooks(HasBrushSizes hasBrushSizes, ScaleBrushSizes scaleBrushSizes);
    static void setLiquifyHooks(IsLiquifyTarget isTarget, LiquifyCommand command);
    static void setLiquifyPreview(LiquifyPreview preview, LiquifyPreviewDone done);
    static void setDeformHooks(IsDeformTarget isTarget, DeformCommand command, DeformDone done);

    static void convertShapes(QList<KoShape*> &shapes, KoShapeContainer *parent);
    static bool needsNewLayer(const QList<KoShape*> &shapes, KisShapeLayer *activeLayer);
    static void initNewLayer(KisShapeLayer *layer, const QList<KoShape*> &shapes);
    /// null when no hook is set or there is nothing to clear
    static KUndo2Command *clearCommand(KisShapeLayer *layer, KisSelectionSP selection);
    static QString layerIcon(const KisShapeLayer *layer);
    /// false when no hook is set or the hook leaves \p copy to be rasterized
    static bool keepWithinSelection(KisShapeLayer *copy, KisSelectionSP selection);
    /// null when no hook is set or it leaves \p layer as it is
    static KUndo2Command *cutWithinSelection(KisShapeLayer *layer, KisSelectionSP selection);
    static bool hasBrushSizes(const KisNode *node);
    /// null when no hook is set or there is nothing to scale
    static KUndo2Command *scaleBrushSizes(KisShapeLayer *layer, const QTransform &transform);
    static bool isLiquifyTarget(const KisNode *node);
    /// null when no hook is set
    static KUndo2Command *liquifyCommand(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, quint64 stroke);
    /// whether a LiquifyPreview is set: the preview then leaves Liquify's layers to it
    static bool hasLiquifyPreview();
    static void liquifyPreview(KisShapeLayer *layer, const QVector<LiquifyDab> &dabs, bool fast);
    static void liquifyPreviewDone(KisShapeLayer *layer, bool keep);
    static bool isDeformTarget(const KisNode *node);
    /// a new number for a transform stroke (see LiquifyCommand)
    static quint64 newLiquifyStroke();
};

#endif // KIS_SHAPE_LAYER_HOOKS_H
