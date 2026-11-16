/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PAINTER_H
#define VSTROKE_PAINTER_H

#include <QScopedPointer>
#include <QVector>

#include <KoColor.h>
#include <brushengine/kis_paintop_preset.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_types.h>

#include "VStrokePrimitive.h"
#include "VCurveData.h"
#include "kritavstroke_export.h"

/**
 * Everything besides the primitives that decides how a stroke looks.
 */
struct KRITAVSTROKE_EXPORT VStrokeRenderSettings
{
    KisPaintOpPresetSP preset;
    KoColor paintColor;
    KoColor backgroundColor;
    qreal opacity = 1.0;              ///< the canvas opacity resource
    int seed = 0;                     ///< seeds the stroke's KisRandomSource
    int perStrokeSeed = 0;            ///< seeds the stroke's KisPerStrokeRandomSource
    KisDistanceInitInfo startDistance;
    /// the curves in image pixels when their opacity varies (VCurveOpacity), applied after rendering
    VCurveData opacityCurves;
};

// TODO: be able to switchout vector strokes with another paintop

/**
 * Paints the primitives of one vector stroke with a real paintop, with
 * seeded random sources, so the same primitives and settings always give
 * the same pixels. The live pen tool and offline re-renders both use it.
 *
 * The painter renders the stroke's own coverage on a transparent device in
 * absolute image pixels, the way a freehand stroke fills its temporary
 * device: in indirect mode with the preset's indirect composite op at unit
 * opacity, otherwise with normal compositing at the stroke opacity.
 * mergeOpacity() is the opacity that still has to be applied when the
 * result is composited, as Krita does when it merges the temporary device.
 *
 * It mirrors FreehandStrokeStrategy::doStrokeCallback. Masking brushes are
 * not supported yet.
 */
class KRITAVSTROKE_EXPORT VStrokePainter
{
public:
    /**
     * \p distance: the distance information to paint with, e.g. the one
     * of KisToolFreehandHelper, which reads it for the brush outline; by
     * default the painter makes its own from settings.startDistance.
     */
    VStrokePainter(KisPaintDeviceSP device,
                   const VStrokeRenderSettings &settings,
                   KisImageSP image = KisImageSP(),
                   KisNodeSP node = KisNodeSP(),
                   KisDistanceInformation *distance = nullptr);
    ~VStrokePainter();

    /// false if no paintop could be created for the preset
    bool isValid() const;

    /**
     * Paints one primitive. Paintops with asynchronous updates (the pixel
     * brush) may keep some dabs queued until the next call or finish().
     */
    void paint(const VStrokePrimitive &primitive);

    /// writes all queued dabs; called by the destructor too
    void finish();

    /// the area painted since the last call
    QVector<QRect> takeDirtyRegion();

    qreal mergeOpacity() const;

    /**
     * The distance information a freehand stroke starts with, as
     * KisToolFreehandHelper::initPaintImpl() creates it.
     */
    static KisDistanceInitInfo startDistance(KisPaintOpPresetSP preset, const QPointF &firstPos, qreal startAngle);

    /**
     * Composites the painted \p coverage onto \p target with \p opacity,
     * as Krita merges the temporary device of a freehand stroke.
     */
    static void merge(KisPaintDeviceSP target, KisPaintDeviceSP coverage, qreal opacity);

    /**
     * Paints all \p primitives into \p device and applies mergeOpacity(),
     * so \p device holds the finished stroke. A primitive marked
     * restartsStroke starts over with a new paintop; with a brush painting
     * indirectly, on a device of its own, laid over the pieces before. Returns false if the
     * preset has no paintop.
     */
    static bool render(KisPaintDeviceSP device,
                       const VStrokeRenderSettings &settings,
                       const QVector<VStrokePrimitive> &primitives,
                       KisImageSP image = KisImageSP(),
                       KisNodeSP node = KisNodeSP());

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif // VSTROKE_PAINTER_H
