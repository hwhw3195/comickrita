/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_TOOL_H
#define VSTROKE_TOOL_H

#include <functional>

#include <QVector>

#include <KisToolPaintFactoryBase.h>
#include <KoCompositeOpRegistry.h>
#include <kis_resources_snapshot.h>
#include <kis_tool_freehand.h>

#include "VStrokeRecorderFacade.h"
#include "kritavstroke_export.h"

class KisShapeLayer;
class KoShape;
class VStrokeFreehandHelper;

/**
 * Draws brush strokes on vector layers: the brush tool's input handling
 * (KisToolFreehand, KisToolFreehandHelper) recorded through
 * VStrokeRecorderFacade, committed as a VStrokeShape.
 *
 * The live stroke is drawn inside its layer, through the layer's live
 * overlay (KisShapeLayer::setLiveOverlay()), until the committed shape
 * replaces it.
 */
class KRITAVSTROKE_EXPORT VStrokeTool : public KisToolFreehand
{
    Q_OBJECT
public:
    explicit VStrokeTool(KoCanvasBase *canvas);
    ~VStrokeTool() override;

    /// why the current preset cannot draw vector strokes, or empty
    static QString unsupportedPresetReason(KisPaintOpPresetSP preset);

    /// \p preset as vector strokes draw it: without color mixing, which
    /// would read the layer's pixels (the preset itself if nothing changes)
    static KisPaintOpPresetSP vectorPreset(KisPaintOpPresetSP preset);

    /// the shapes of a stock \p layer that Clear removes: all of them, or with
    /// an active \p selection the ones it fully covers
    static QList<KoShape*> shapesToClear(KisShapeLayer *layer, KisSelectionSP selection);
    /// whether \p selection holds every pixel \p shape paints
    static bool coveredBySelection(KoShape *shape, KisSelectionSP selection, const QTransform &documentToPixels);

    /// a stroke drawn in its layer's live overlay
    struct LiveStroke {
        int id = 0;
        KisShapeLayerSP layer;
        KisPaintDeviceSP device; ///< in image pixels, before \c opacity
        qreal opacity = 1.0;
        QString compositeOp = COMPOSITE_OVER;
    };

    /// the live overlay drawing \p strokes into their layer
    static std::function<void(QPainter&, const QRect&)> liveOverlay(const QVector<LiveStroke> &strokes);

protected:
    void beginPrimaryAction(KoPointerEvent *event) override;
    void initStroke(KoPointerEvent *event) override;

private:
    KisShapeLayer *shapeLayer() const;
    void showMessage(const QString &message);
    void slotDirty(const QVector<QRect> &rects);
    void commitStroke();

    void addLiveStroke(const LiveStroke &stroke);
    void removeLiveStroke(int id);
    void updateLiveOverlay(KisShapeLayerSP layer);

private:
    VStrokeFreehandHelper *m_helper = nullptr;
    VStrokeRecorderFacade m_facade;
    KisResourcesSnapshotSP m_resources;
    VStrokeRenderSettings m_settings;
    QString m_compositeOp = COMPOSITE_OVER;
    qreal m_startAngle = 0.0;

    /// the stroke being drawn, and committed ones whose shape is not in the layer yet
    QVector<LiveStroke> m_liveStrokes;
    int m_liveStrokeId = 0; ///< of the stroke being drawn
    int m_lastLiveStrokeId = 0;
};

class KRITAVSTROKE_EXPORT VStrokeToolFactory : public KisToolPaintFactoryBase
{
public:
    VStrokeToolFactory();
    KoToolBase *createTool(KoCanvasBase *canvas) override;
};

#endif // VSTROKE_TOOL_H
