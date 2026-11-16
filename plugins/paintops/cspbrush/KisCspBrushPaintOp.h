/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CSP_BRUSH_PAINTOP_H
#define KIS_CSP_BRUSH_PAINTOP_H

#include <functional>
#include <QPointF>
#include <memory>

#include <brushengine/kis_paintop.h>
#include <kis_random_source.h>
#include <kis_types.h>
#include <KisResourcesInterface.h>
#include <KoResourceLoadResult.h>

#include "CspBrushParams.h"


class CspColorMixer;
class CspRibbonBuilder;
class CspStrokeBuffer;
class CspTextureStage;
class CspMixedStrokeBuffer;
class CspColorJitter;
class CspTipSet;

/**
 * Clip Studio Paint style brush engine.
 *
 * Stamping with CSP dynamics (effectors), multi-tip repeat methods, gray
 * tips painted main..sub, angle sources, thickness, CSP gap and starting
 * taper; or a ribbon (CspRibbonBuilder). Opacity caps the stroke and
 * brush density is the per-dab flow (Krita's indirect "wash" painting).
 */
class KRITACSPBRUSHPAINTOP_EXPORT KisCspBrushPaintOp : public KisPaintOp
{
public:
    KisCspBrushPaintOp(const KisPaintOpSettingsSP settings, KisPainter *painter, KisNodeSP node, KisImageSP image);
    ~KisCspBrushPaintOp() override;

    /// Dab rotation in radians (counter-clockwise), exposed for tests
    static qreal dabRotation(const CspBrushParams &params, const KisPaintInformation &info, qreal randomSample);

    /// Brush size after dynamics (image px, never negative), exposed for tests
    static qreal dabDiameter(const CspBrushParams &params, qreal dynamics);

    /// Particle rotation in radians: the chosen direction, plus random when on; exposed for tests
    static qreal sprayParticleRotation(const CspBrushParams &params, qreal drawingAngle, qreal polarAngle,
                                       qreal randomSample);

    /// Particle distance from the spray centre, 0..1 of the radius (Krita's spray distributions)
    static std::function<qreal(KisRandomSourceSP)> sprayRadialDistribution(qreal deviation);

    // the texture pattern, for KisSimplePaintOpFactory
    static QList<KoResourceLoadResult> prepareLinkedResources(const KisPaintOpSettingsSP settings,
                                                              KisResourcesInterfaceSP resourcesInterface);
    static QList<KoResourceLoadResult> prepareEmbeddedResources(const KisPaintOpSettingsSP settings,
                                                                KisResourcesInterfaceSP resourcesInterface);

protected:
    KisSpacingInformation paintAt(const KisPaintInformation &info) override;
    KisSpacingInformation updateSpacingImpl(const KisPaintInformation &info) const override;
    KisTimingInformation updateTimingImpl(const KisPaintInformation &info) const override;

private:
    struct DabGeometry {
        qreal diameter {0.0};  ///< brush size after dynamics, image px
        qreal ratio {1.0};
    };

    qreal random(const KisPaintInformation &info) const;
    DabGeometry geometry(const KisPaintInformation &info) const;
    KisSpacingInformation spacing(const KisPaintInformation &info, qreal diameter) const;
    KisSpacingInformation paintRibbon(const KisPaintInformation &info);
    KisSpacingInformation paintSpray(const KisPaintInformation &info);
    /// Stamps tip @p tipIndex centred at @p pos (diameter in image px)
    void stamp(const KisPaintInformation &info, int tipIndex, const QPointF &pos, qreal diameter,
               qreal ratio, qreal rotation, const std::pair<qreal, qreal> &opacityDensity);
    /// CSP Opacity and Brush density (with dynamics and taper) for this dab; evaluate
    /// once per dab, the random input gives a new value every time
    std::pair<qreal, qreal> opacityAndDensity(const KisPaintInformation &info) const;
    /// Sets the dab painter's opacity and flow (= density)
    void applyOpacityAndDensity(const std::pair<qreal, qreal> &opacityDensity);
    /// Color mixing: picks up canvas color under @p dab and lays the paint down
    void mixDab(KisFixedPaintDeviceSP dab, const QRect &rect, const KisPaintInformation &info, const KoColor &color,
                qreal diameter,
                const std::pair<qreal, qreal> &opacityDensity);
    /// The main color of a dab, with color jitter
    KoColor dabColor(const KisPaintInformation &info) const;
    /// Painter the dabs go to: the stroke buffer when the stroke is post-processed
    KisPainter *dabPainter();
    /// Paints @p dab at @p rect, mirrored; @p preserveDab when the dab is shared
    void paintDab(KisFixedPaintDeviceSP dab, const QRect &rect, bool preserveDab);
    /// Returns @p dab with the texture applied (a copy when @p shared)
    KisFixedPaintDeviceSP applyTexture(KisFixedPaintDeviceSP dab, const QPoint &topLeft,
                                       const KisPaintInformation &info, bool shared);

    CspBrushParams m_params;
    std::unique_ptr<CspTipSet> m_tips;
    std::unique_ptr<CspRibbonBuilder> m_ribbon;
    std::unique_ptr<CspTextureStage> m_texture;
    std::unique_ptr<CspStrokeBuffer> m_strokeBuffer;
    std::unique_ptr<CspMixedStrokeBuffer> m_mixedBuffer;
    std::unique_ptr<CspColorMixer> m_mixer;
    KisPaintDeviceSP m_mixSource; ///< color mixing: the layer as it was when the stroke started
    QPointF m_lastMixPos;         ///< where the previous mixed dab was
    bool m_hasLastMixPos {false};
    std::unique_ptr<CspColorJitter> m_jitter;
    std::function<qreal(KisRandomSourceSP)> m_sprayDistance;
    bool m_finished {false};
};

#endif // KIS_CSP_BRUSH_PAINTOP_H
