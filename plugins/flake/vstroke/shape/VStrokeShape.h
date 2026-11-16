/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_SHAPE_H
#define VSTROKE_SHAPE_H

#include <QPainter>
#include <QSharedDataPointer>

#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>

#include "VCurveShape.h"
#include "VStrokePainter.h"
#include "VStrokePresetStore.h"
#include "VStrokePrimitive.h"
#include "VStrokeRenderCache.h"
#include "kritavstroke_export.h"

#define VStrokeShapeId "VStrokeShape"

class KoDocumentResourceManager;

/**
 * The brush settings of one stroke, shared copy-on-write between a shape and
 * its clones (KoShapeManager clones every shape of a layer on every
 * repaint). The samples are the shape's curves.
 */
class KRITAVSTROKE_EXPORT VStrokeData : public QSharedData
{
public:
    KisPaintOpPresetSP preset;       ///< a resources snapshot (see KisResourcesSnapshot); null when not found
    VStrokePresetDefinitionSP presetDefinition; ///< the settings as saved, shared by the strokes drawn with them
    QString presetName;              ///< the preset resource the stroke was drawn with: for older files and display
    QString presetFileName;
    QString presetMd5;
    qreal brushSize = 10.0;          ///< preset diameter in image pixels, for the outline preview

    qreal opacity = 1.0;
    int seed = 0;
    int perStrokeSeed = 0;
    qreal startAngle = 0.0;          ///< drawing angle at the first sample, see KisDistanceInitInfo
    QColor backgroundColor = Qt::white; ///< the background color resource, some paintops use it
    QString compositeOp = COMPOSITE_OVER; ///< how the stroke's rendering goes onto the layer, as the brush's blend mode
    bool noLine = false;             ///< "no fill" or "no line": the stroke draws nothing, and is still there to select
    QColor noLineColor;              ///< the paint color while hidden, for when it shows again

    bool erases() const { return compositeOp == COMPOSITE_ERASE; }

    /// The QPainter mode for a blend mode; SourceOver for the ones QPainter doesn't have
    static QPainter::CompositionMode compositionMode(const QString &compositeOp);
    /// False for ids Krita doesn't know, which draw as Normal (the id stays in the stroke)
    static bool canBlendWith(const QString &compositeOp);
    /// Draws a stroke's pixels with the painter's state, in the blend mode
    static void drawImage(QPainter &painter, const QString &compositeOp, const QPointF &pos, const QImage &image);
};

/**
 * A freehand stroke on a vector layer, drawn by a real brush preset.
 *
 * The stroke is stored as the samples and segments the freehand helper
 * produced (curves, see VStrokeCurves), plus the seeds of its random
 * sources, so it re-renders exactly. The color is the shape's
 * KoColorBackground.
 *
 * Saved as an SVG <path> whose "d" is the radius outline, so other SVG
 * readers (and stock Krita) show the outline; the curves are a child element
 * they ignore. The brush settings are saved once per file in the defs
 * section (see VStrokePresetStore) and the strokes refer to them.
 *
 * Shape-local units map to image pixels through renderTransform(). Radii are
 * in image pixels: the brush keeps its size when the shape is scaled.
 */
class KRITAVSTROKE_EXPORT VStrokeShape : public VCurveShape
{
public:
    VStrokeShape();
    ~VStrokeShape() override;

    /**
     * Creates a stroke from primitives in image pixels. \p settings gives
     * everything but the primitives; its preset is interned in
     * VStrokePresetStore (GUI thread). The new shape is in document
     * coordinates, as containers expect added shapes to be (a vector layer
     * moves it into its own transformation), with a translation so that
     * its local origin is the top left of the stroke.
     */
    static VStrokeShape *fromImagePixels(const QVector<VStrokePrimitive> &primitives,
                                         qreal pixelsPerPoint,
                                         const VStrokeData &settings);

    KoShape *cloneShape() const override;

    void paint(QPainter &painter) const override;
    /// the brush draws everything: there is no line around the stroke
    void paintStroke(QPainter &painter) const override;
    void paintMarkers(QPainter &painter) const override;

    /**
     * The stroke as the Stroke tab sees it, made from the stroke's own data
     * and not stored: the paint color, and the diameter at the thickest point
     * as the width, in document points.
     */
    KoShapeStrokeModelSP stroke() const override;
    /**
     * Takes a new color and width: the color becomes the paint color, and the
     * width scales all radii, so a stroke keeps its taper. No color hides the
     * stroke. Only for a stroke in a layer: the style a file saved on a stroke
     * is not its own.
     */
    void setStroke(KoShapeStrokeModelSP stroke) override;
    /// the brush decides how corners, ends and dashes look
    Features strokeFeatures() const override;
    /// the paint color of a hidden stroke
    QColor hiddenColor() const override;

    /// No background in a layer hides the stroke, keeping its paint color. A file's says nothing: see loadSvg
    void setBackground(QSharedPointer<KoShapeBackground> background) override;

    void setSize(const QSizeF &size) override;
    QRectF boundingRect() const override;

    bool saveSvg(SvgSavingContext &context) override;
    bool loadSvg(const QDomElement &element, SvgLoadingContext &context) override;

    const VStrokeData &data() const;
    /**
     * Replaces the brush settings (another preset or brush size: VEditActions::withBrush()).
     * A preset without a definition is interned in VStrokePresetStore. A new look: the stroke
     * renders anew. GUI thread.
     */
    void setData(const VStrokeData &data);

    /// image pixels per document point (KoDocumentResourceManager::documentResolution() / 72)
    void setPixelsPerPoint(qreal pixelsPerPoint);
    qreal resolution() const;

    /// in ppi; vector layers call it for the shapes added to them and on image resolution changes
    void setResolution(qreal xRes, qreal yRes) override;

    /// shape-local coordinates to image pixels
    QTransform renderTransform() const;

    /// the primitives to replay, in image pixels (renderTransform())
    QVector<VStrokePrimitive> imagePixelPrimitives() const;

    /// the paint color: the color of the KoColorBackground
    QColor color() const;

    VStrokeRenderSettings renderSettings() const;

    /// blocks until the brush renders requested so far are done (for tests)
    void waitForRendering() const;

    /// shapes alive, including clones (for tests: catches leaks)
    static int liveCount();

    /// the revision of the curves and look; a new one needs a new rendering
    quint64 revision() const { return m_revision; }

    /**
     * Keeps the rendering of the current curves, before an undoable edit
     * moves them to \p next: when the curves come back (undo, cancel), the
     * stroke shows it again without a render or showing a stale one. Its
     * rendering at \p next is kept in turn, for the redo. Nothing changes
     * when the kept rendering is already of \p next (a redo), or when the
     * current curves aren't rendered yet. One rendering is kept; a change
     * of the look (color, size) drops it. GUI thread.
     */
    void keepRendering(const VCurveData &next);

    /**
     * How a transform stroke moves this stroke right now (VMoveSession).
     * While \p rendersPaused (the transform tool's Fast preview), the stroke
     * renders nothing and shows its newest rendering bent to the current
     * curves. \p hint, in image pixels, takes the stroke from a rendering's
     * pose to the current one (a perspective): the stand-in is then that
     * rendering drawn through it in one go rather than bent piece by piece.
     * The hint is used only while it fits the points; identity: none.
     */
    struct Motion {
        bool rendersPaused = false;
        QTransform hint;
    };
    /// GUI thread
    void setMotion(const Motion &motion);
    /**
     * Renders again (the move paused or ended). The hint stays for the bent
     * stand-in until the rendering of the current curves lands. GUI thread.
     */
    void clearMotion();
    const Motion &motion() const { return m_motion; }

    /// whether the stroke holds its renders back (for tests)
    bool isRendersPaused() const { return m_motion.rendersPaused; }

    /**
     * Times a stroke painted a rendering of another pose unbent, for want of
     * the current one (for tests: zero where bendsFrom() bends, as in Fast or
     * a perspective; Accurate point moves count here by design).
     */
    static int staleShapePaints();

    /// times a stroke painted its flat outline for want of any rendering (for tests: catches flicker)
    static int outlinePaints();

    /**
     * Uses \p device, the finished stroke in image pixels, as the brush
     * rendering, for the shape at its current absolute position (which
     * adding it to a vector layer keeps). The pen tool passes its live
     * stroke, so the stroke does not render a second time.
     */
    void adoptRendering(KisPaintDeviceSP device);

    /**
     * A brush render finished (GUI thread). Updates the area of that
     * rendering too, not only the shape: until the current revision is
     * rendered, the stroke paints the newest rendering it has, which may
     * be of older curves and reach outside anything the current ones cover.
     */
    void renderingFinished(const VStrokeRenderCache::Entry &entry);

protected:
    void shapeChanged(ChangeType type, KoShape *shape = 0) override;

    void curvesChanged() override;
    /// the area \p entry paints in, in document points
    QRectF documentRect(const VStrokeRenderCache::Entry &entry) const;
    /// the curves, transform-independent settings or color changed
    void newRevision();
    qreal outlineRadiusScale() const override;
    qreal outlineMinRadius() const override;
    qreal outlineTolerance() const override;

private:
    VStrokeShape(const VStrokeShape &rhs);

    /// image pixels per shape-local unit
    qreal pixelsPerUnit() const;

    /// the largest radius, in image pixels
    qreal maxRadius() const;

    /// in a vector layer (not a clone made for painting)
    bool isLive() const;

    /// gives this shape its own render cache if it is live and shares one it does not own
    void claimCache();

    void paintRendering(QPainter &painter, const VStrokeRenderCache::Entry &entry) const;

    /**
     * Whether \p entry shows the stroke in another pose than the current
     * one: its points or handles were elsewhere, or the transform since
     * scales or shears it (a move or turn keeps the pose).
     */
    bool poseDiffers(const VStrokeRenderCache::Entry &entry) const;
    /**
     * Whether the stand-in for want of the current rendering is \p entry
     * bent to the current curves: its pose differs, it has the same points,
     * and either renders are paused or settling after a pause (Fast), the
     * motion has a hint (a perspective), or the transform since scales or
     * shears. Point moves in Accurate draw \p entry as it is.
     */
    bool bendsFrom(const VStrokeRenderCache::Entry &entry) const;
    /// whether the motion's hint \p entry's curves to the current ones, and keeps the rendering a sane size
    bool warpHintFits(const VStrokeRenderCache::Entry &entry) const;
    /// paints \p entry bent to the current curves, piece by piece along the stroke
    bool paintWarped(QPainter &painter, const VStrokeRenderCache::Entry &entry) const;
    /// \p entry bent to the current curves, as a rendering of the current revision
    VStrokeRenderCache::Entry warpedRendering(const VStrokeRenderCache::Entry &entry) const;
    void paintOutline(QPainter &painter) const;

    /// a new revision of the same look: the kept rendering stays
    void nextCurvesRevision();


private:
    QSharedDataPointer<VStrokeData> m_data;
    qreal m_pixelsPerPoint = 1.0;
    bool m_savingSvg = false;        ///< other readers see a filled outline without a line
    quint64 m_revision = 0;          ///< changes with anything the brush rendering depends on
    /**
     * Shape-local: where renderings may have been painted since the
     * rendering of the current curves arrived. Renderings of older curves
     * are painted while points are dragged, and the cache drops them; the
     * next update must still clear their pixels.
     */
    QRectF m_renderedRect;
    VStrokeRenderCacheSP m_cache;

    /// the curves of m_revision, to keep with its rendering (keepRendering())
    VCurveData m_revisionCurves;
    /// a rendering of other curves, kept for an undo (keepRendering())
    struct KeptRendering {
        VCurveData curves;
        VStrokeRenderCache::Entry entry;
        bool valid = false;
    } m_kept;

    Motion m_motion;
    /// renders resumed after a Fast pause: bent until the current rendering lands or the curves move on
    bool m_settling = false;
};

#endif // VSTROKE_SHAPE_H
