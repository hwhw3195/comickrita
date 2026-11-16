/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VMOVE_SESSION_H
#define VMOVE_SESSION_H

#include <QElapsedTimer>
#include <QMutex>
#include <QPointer>
#include <QTransform>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include <kis_selection.h>
#include <kis_types.h>

#include "VCurveData.h"
#include "VGuiThreadCommand.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KoPathShape;
class KoShape;
class QTimer;
class VCurveShape;
class VMoveCommand;

/**
 * Moves the curves (and stock paths) of one vector layer for one transform
 * stroke: Liquify and the deforming modes (Perspective, Warp...). The mode
 * gives where things go; the session does the rest the same way for all.
 *
 * - begin() reads the layer once: each curve as it was before the stroke,
 *   cut at the edge of what moves when there's an inside test (the pieces
 *   inside move as a new shape), and the points of stock paths.
 * - While the stroke runs, the session is live: apply() moves everything
 *   from where it was before the stroke, in the GUI thread. Undoing a pass's
 *   command does nothing (the next pass moves on from the same start), so
 *   the canvas never shows the start between two poses.
 * - Brush strokes get a VStrokeShape::Motion: the hint, and in the Fast
 *   preview no renders until the moves pause (one timer per session).
 * - end() finishes it: keep, and the last command applied holds the change
 *   (its undo and redo swap the curves from then on); otherwise everything
 *   goes back.
 *
 * Everything but forget() runs in the GUI thread. Commands live in worker
 * threads too: they hold the session, and do their work in the GUI thread.
 */
class KRITAVSTROKE_EXPORT VMoveSession : public std::enable_shared_from_this<VMoveSession>
{
public:
    /// whether a point of the image, in pixels, is inside what moves
    using InsideTest = std::function<bool(const QPointF &)>;

    struct Options {
        /// the transform tool's Fast preview: no renders while the moves go on
        bool fast = false;
        /// only what is inside moves, cut at the edge; null: everything
        InsideTest inside;
        /// with a selection's inside test: the paths it covers move
        KisSelectionSP selection;
        /// stock paths move too (paths aren't cut: they move when all of them is inside)
        bool paths = false;
    };

    /**
     * Where the curves of one shape go: from \p before (where they were before
     * the stroke, or the part inside) or \p now (where they are), with
     * \p toImage taking their units to image pixels.
     */
    using CurveTarget = std::function<VCurveData(const VCurveData &before, const VCurveData &now, const QTransform &toImage)>;
    /// moves the points of \p path, which is where it was before the stroke
    using PathTarget = std::function<void(KoPathShape *path, const QTransform &documentToImage)>;

    explicit VMoveSession(KisShapeLayer *layer);
    ~VMoveSession();

    KisShapeLayer *layer() const { return m_layer; }

    /// reads the layer, the first time only (GUI thread)
    void begin(const Options &options);
    bool hasBegun() const { return m_begun; }
    /// the stroke still runs
    bool isLive() const { return m_live; }
    bool isFast() const;
    /// the Fast preview changes while the stroke runs (GUI thread)
    void setFast(bool fast);

    /**
     * Live: moves everything to where \p curves and \p paths take it, and
     * \p command (may be null: a preview) becomes the last one applied.
     * \p hint is the brush strokes' VStrokeShape::Motion::hint.
     */
    void apply(const VMoveCommand *command, const CurveTarget &curves, const PathTarget &paths, const QTransform &hint);

    /**
     * What a pass computes its targets from, off the GUI thread: each moving
     * part's curves as they were before the stroke (all of a shape, or the
     * piece inside) and what takes their units to image pixels. They never
     * change after begin(). Any thread; empty before begin().
     */
    struct Snapshot {
        QVector<VCurveData> curves;
        QVector<QTransform> toImage;
    };
    Snapshot snapshot() const;
    /**
     * Live, GUI thread: moves the parts to \p targets (by the snapshot's
     * index; a missing one stays where it is) and the paths by \p paths, as
     * apply() does. Returns the image pixels the changed shapes covered
     * before and after.
     */
    QRect commit(const VMoveCommand *command, const QVector<VCurveData> &targets, const PathTarget &paths,
                 const QTransform &hint);
    /// what a mode keeps for the whole stroke (the deform's sample lists); any thread
    std::shared_ptr<void> modeData() const;
    void setModeData(const std::shared_ptr<void> &data);
    /// live: \p command becomes the last one applied, with things where they are
    void adopt(const VMoveCommand *command);
    const VMoveCommand *lastCommand() const { return m_applied.load(); }

    /// a command's undo: nothing while live; after end(), back to before the stroke if \p command holds the change
    void undo(const VMoveCommand *command);
    /// a command's redo after end(): forward again if \p command holds the change
    void redo(const VMoveCommand *command);

    /**
     * The stroke ends: \p keep, and the last command applied holds the change;
     * otherwise everything goes back, and \p changed (when given) gets the image
     * pixels the shapes going back covered before and after.
     */
    void end(bool keep, QRect *changed = nullptr);

    /// \p command is deleted (any thread)
    void forget(const VMoveCommand *command);

    /// times a session put things back where they were before a stroke (for tests)
    static int restoreCount();

private:
    struct CurvePart;
    struct PathPart;
    struct PathPointState;
    using PathState = QVector<QVector<PathPointState>>;

    /// apply() and commit(), with the lock held: returns the document area the changed shapes covered
    QRectF moveParts(const VMoveCommand *command,
                     const std::function<VCurveData(int index, const CurvePart &part, const VCurveData &now)> &target,
                     const PathTarget &paths, const QTransform &hint);
    void readShapes(const QList<KoShape*> &shapes, const QTransform &documentToImage);
    void readCurve(VCurveShape *shape, const QTransform &documentToImage);
    void readPath(KoPathShape *shape);
    static PathState pathState(KoPathShape *shape);
    static void setPathState(KoPathShape *shape, const PathState &state, const QTransform &transformation, bool parametric);
    /// returns the document area the shapes going back covered
    QRectF restore();
    void forward();
    void pauseEnded();
    void makePauseTimer();

    QPointer<KisShapeLayer> m_layer;
    mutable QMutex m_mutex;
    Options m_options;
    // read without the lock by the getters, from any thread
    std::atomic<bool> m_begun{false};
    std::atomic<bool> m_live{true};
    /// paths moved since the start
    bool m_pathsMoved = false;
    /// the command whose target things are at; after end(), the one holding the change
    std::atomic<const VMoveCommand*> m_applied{nullptr};
    /// after end(): the change is applied
    bool m_keptApplied = false;
    QTransform m_documentToImage;
    QVector<CurvePart> m_curves;
    QVector<PathPart> m_paths;
    /// Fast: renders resume when it times out; made and deleted in the GUI thread
    QPointer<QTimer> m_pause;
    /// since the last pass: a slow pass stretches the pause (F7)
    QElapsedTimer m_sinceMove;
    std::shared_ptr<void> m_modeData;
};

/**
 * One command of a VMoveSession's stroke (a pass). While the stroke runs,
 * its redo brings the session to its target (move()) and its undo does
 * nothing; after the stroke, the command holding the change swaps it.
 */
class KRITAVSTROKE_EXPORT VMoveCommand : public VGuiThreadCommand
{
public:
    VMoveCommand(const KUndo2MagicString &text, KisShapeLayer *layer, std::shared_ptr<VMoveSession> session);
    ~VMoveCommand() override;

    const std::shared_ptr<VMoveSession> &session() const { return m_session; }

protected:
    void apply(bool redo) override;
    /// live, in the GUI thread: brings the session to this command's target
    virtual void move() = 0;

private:
    KisShapeLayerSP m_layer;
    std::shared_ptr<VMoveSession> m_session;
};

#endif // VMOVE_SESSION_H
