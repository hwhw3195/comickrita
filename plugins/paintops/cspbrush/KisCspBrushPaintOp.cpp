/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCspBrushPaintOp.h"

#include <QElapsedTimer>
#include <QLineF>
#include <QtMath>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_dab_cache.h>
#include <kis_dab_shape.h>
#include <kis_fixed_paint_device.h>
#include <kis_global.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <kis_paint_information.h>
#include <kis_painter.h>
#include <kis_paintop_utils.h>
#include <kis_spacing_information.h>
#include <kis_timing_information.h>
#include <kis_texture_option.h>
#include <KoResourceLoadResult.h>
#include <kis_default_bounds_base.h>

#include "KisSprayRandomDistributions.h"

#include "CspColorJitter.h"
#include "CspColorMixer.h"
#include "CspRibbonBuilder.h"
#include "CspStrokeBuffer.h"
#include "CspTextureStage.h"
#include "CspTipSet.h"

KisCspBrushPaintOp::KisCspBrushPaintOp(const KisPaintOpSettingsSP settings, KisPainter *painter, KisNodeSP node, KisImageSP image)
    : KisPaintOp(painter)
{
    Q_UNUSED(node);
    Q_UNUSED(image);

    m_params = CspBrushParams::fromSettings(settings.data());
    m_tips.reset(new CspTipSet(m_params));
    if (m_params.ribbonActive()) {
        m_ribbon.reset(new CspRibbonBuilder(m_params, m_tips.get()));
    }
    m_jitter.reset(new CspColorJitter(m_params));
    if (!m_jitter->isActive()) m_jitter.reset();
    CspBrushParams textureParams = m_params;
    if (CspTextureStage::invertsForEraser(settings.data())) textureParams.textureInvert = !textureParams.textureInvert;
    m_texture.reset(new CspTextureStage(textureParams,
                                        CspTextureStage::loadPattern(settings.data(), settings->resourcesInterface())));
    if (!m_texture->isEnabled()) m_texture.reset();
    if (m_params.mixingActive()) {
        const QString compositeOp = painter->compositeOpId();
        m_mixer.reset(new CspColorMixer(m_params, compositeOp == COMPOSITE_ERASE));
        // Normal and Erase are the mix itself; other modes are composed from the layer at the stroke start
        if (m_params.waterEdge || (compositeOp != COMPOSITE_OVER && compositeOp != COMPOSITE_ERASE)) {
            m_mixedBuffer.reset(new CspMixedStrokeBuffer(m_params, painter));
        } else {
            // the canvas color is the layer's without this stroke's paint (tiles are shared until written)
            m_mixSource = new KisPaintDevice(*painter->device());
        }
    } else if (m_params.waterEdge) {
        m_strokeBuffer.reset(new CspStrokeBuffer(m_params, painter->device()->compositionSourceColorSpace()));
    }
}

KisCspBrushPaintOp::~KisCspBrushPaintOp()
{
}

QList<KoResourceLoadResult> KisCspBrushPaintOp::prepareLinkedResources(const KisPaintOpSettingsSP settings,
                                                                       KisResourcesInterfaceSP resourcesInterface)
{
    return KisTextureOption::prepareLinkedResources(settings, resourcesInterface);
}

QList<KoResourceLoadResult> KisCspBrushPaintOp::prepareEmbeddedResources(const KisPaintOpSettingsSP settings,
                                                                         KisResourcesInterfaceSP resourcesInterface)
{
    return KisTextureOption::prepareEmbeddedResources(settings, resourcesInterface);
}

qreal KisCspBrushPaintOp::random(const KisPaintInformation &info) const
{
    return info.randomSource() ? info.randomSource()->generateNormalized() : 0.5;
}

KisCspBrushPaintOp::DabGeometry KisCspBrushPaintOp::geometry(const KisPaintInformation &info) const
{
    DabGeometry g;
    // sizeMultiplier: a vector stroke replayed with a per-point radius
    g.diameter = dabDiameter(m_params, m_params.sizeDynamics.value(info) * info.sizeMultiplier());
    g.ratio = qBound(0.01, m_params.thickness * m_params.thicknessDynamics.value(info), 1.0);
    return g;
}

qreal KisCspBrushPaintOp::dabDiameter(const CspBrushParams &params, qreal dynamics)
{
    qreal diameter = qMax(0.0, params.size * dynamics);
    if (params.atLeast1Pixel) diameter = qMax(1.0, diameter);
    return diameter;
}

qreal KisCspBrushPaintOp::sprayParticleRotation(const CspBrushParams &params, qreal drawingAngle, qreal polarAngle,
                                                qreal randomSample)
{
    qreal rotation = kisDegreesToRadians(params.sprayAngle);
    // one direction (CSP's choices are exclusive; old presets with several bits: the first of these wins)
    const int direction = params.sprayDirection & CspBrushParams::SprayDirectionMask;
    if (direction & CspBrushParams::SprayLineDirection) {
        rotation -= drawingAngle;
    } else if (direction & CspBrushParams::SprayWhole) {
        rotation += kisDegreesToRadians(params.angle);
    } else if (direction & CspBrushParams::SprayTowardCenter) {
        // TODO calibrate: does the particle's +x point at the centre or away from it
        rotation -= polarAngle;
    }
    // random on top of any of them
    if (params.sprayDirection & CspBrushParams::SprayRandom) {
        rotation += (2.0 * randomSample - 1.0) * M_PI * params.sprayRandom;
    }
    return rotation;
}

std::function<qreal(KisRandomSourceSP)> KisCspBrushPaintOp::sprayRadialDistribution(qreal deviation)
{
    // CSP "Spray deviation" (-100..100, higher gathers the particles at the centre) on Krita's
    // clustering amount, whose useful range is about -10..10 (the spray engine's soft range).
    // Unscaled, -50 put every particle on the rim. TODO calibrate the scale against CSP
    if (qFuzzyIsNull(deviation)) {
        return [](KisRandomSourceSP rs) { return KisSprayUniformDistributionPolarDistance()(rs); };
    }
    const KisSprayClusterBasedDistributionPolarDistance cluster(qBound(-100.0, deviation * 0.1, 100.0));
    return [cluster](KisRandomSourceSP rs) { return cluster(rs); };
}

qreal KisCspBrushPaintOp::dabRotation(const CspBrushParams &params, const KisPaintInformation &info, qreal randomSample)
{
    // Krita dabs rotate counter-clockwise for positive angles (KisQImagePyramid
    // renders -rotation through a clockwise QTransform), like CSP's Angle.
    qreal rotation = kisDegreesToRadians(params.angle);
    if (params.verticalThickness) rotation += M_PI_2;

    if (params.angleInputs & CspBrushParams::AngleLineDirection) {
        // drawingAngle() is clockwise on screen (y down)
        rotation -= info.drawingAngle();
    }
    if (params.angleInputs & CspBrushParams::AnglePenDirection) {
        // TODO calibrate against CSP: tip +x along the pen body, see sut2kpp TILT_DIRECTION_OFFSET
        rotation -= KisPaintInformation::tiltDirection(info, false);
    }
    if (params.angleInputs & CspBrushParams::AnglePenRotation) {
        // TODO calibrate the barrel rotation sign against CSP
        rotation -= kisDegreesToRadians(info.rotation());
    }
    if (params.angleInputs & CspBrushParams::AngleRandom) {
        rotation += (2.0 * randomSample - 1.0) * M_PI * params.angleRandom;
    }
    return normalizeAngle(rotation);
}

KisSpacingInformation KisCspBrushPaintOp::spacing(const KisPaintInformation &info, qreal diameter) const
{
    const qreal lodScale = KisLodTransform::lodToScale(painter()->device());
    const qreal extraScale = qMax(0.0, m_params.spacingDynamics.value(info));

    return KisPaintOpUtils::effectiveSpacing(diameter * lodScale, diameter * lodScale,
                                             extraScale, true, true, 0.0, false,
                                             m_params.spacingFraction(),
                                             false, 1.0, lodScale);
}

std::pair<qreal, qreal> KisCspBrushPaintOp::opacityAndDensity(const KisPaintInformation &info) const
{
    const qreal opacity = qBound(0.0, m_params.opacity * m_params.opacityDynamics.value(info), 1.0);
    const qreal density = qBound(0.0, m_params.density * m_params.densityDynamics.value(info), 1.0);
    // a vector stroke replayed with a per-point density, as the Krita opacity and flow
    return {info.applyDensity(opacity), info.applyDensity(density)};
}

void KisCspBrushPaintOp::applyOpacityAndDensity(const std::pair<qreal, qreal> &opacityDensity)
{
    dabPainter()->setOpacityUpdateAverage(opacityDensity.first);
    dabPainter()->setFlow(opacityDensity.second);
}

void KisCspBrushPaintOp::mixDab(KisFixedPaintDeviceSP dab, const QRect &rect, const KisPaintInformation &info,
                                const KoColor &color, qreal diameter,
                                const std::pair<qreal, qreal> &opacityDensity)
{
    // with a watercolor edge or a blend mode, mixing works on a copy of the layer
    KisPaintDeviceSP device = m_mixedBuffer ? m_mixedBuffer->device() : painter()->device();
    KisPainter *target = m_mixedBuffer ? m_mixedBuffer->painter() : painter();
    const int n = rect.width() * rect.height();

    const bool profile = CspMixProfile::enabled;
    QElapsedTimer timer;
    if (profile) timer.start();
    auto lap = [&](qint64 &total) {
        if (profile) {
            total += timer.nsecsElapsed();
            timer.restart();
        }
    };

    KisFixedPaintDeviceSP mask = new KisFixedPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    mask->setRect(rect);
    mask->lazyGrowBufferWithoutInitialization();
    dab->colorSpace()->copyOpacityU8(dab->data(), mask->data(), n);

    lap(CspMixProfile::prepNs);

    // the layer as it was when the stroke started: the stroke does not pick up its own paint
    KisPaintDeviceSP source = m_mixedBuffer ? m_mixedBuffer->original() : m_mixSource;

    // Running color samples a blurred neighbourhood
    KoColor canvas;
    const qreal blur = m_params.mixBlurFollowsSize ? m_params.mixBlur * diameter
                                                   : m_params.mixBlur * KisLodTransform::lodToScale(device);
    if (m_params.mixingType == CspBrushParams::MixRunningColor && blur >= 1.0) {
        const int r = qRound(blur);
        canvas = CspColorMixer::sampleCanvas(source, rect.adjusted(-r, -r, r, r), QVector<qint16>(), m_params.mixingMode);
    } else {
        canvas = CspColorMixer::sampleCanvas(source, rect, mask->data(), m_params.mixingMode);
    }

    lap(CspMixProfile::sampleNs);

    const qreal amount = qBound(0.0, m_params.paintAmount * m_params.paintAmountDynamics.value(info), 1.0);
    const qreal paintDensity = qBound(0.0, m_params.paintDensity * m_params.paintDensityDynamics.value(info), 1.0);
    // how far the brush moved since the last dab, in brush diameters (a spray's particles share one position)
    const qreal brushDiameter = qMax(1.0, geometry(info).diameter * KisLodTransform::lodToScale(device));
    const qreal distance = m_hasLastMixPos ? QLineF(m_lastMixPos, info.pos()).length() / brushDiameter : 0.0;
    m_lastMixPos = info.pos();
    m_hasLastMixPos = true;
    const KoColor paint = m_mixer->paintColor(canvas, color.convertedTo(device->colorSpace()),
                                              amount, paintDensity, distance);

    const qreal strength = opacityDensity.first * opacityDensity.second;
    lap(CspMixProfile::prepNs);
    if (m_mixedBuffer) {
        m_mixedBuffer->addCoverage(rect, mask, opacityDensity.first, opacityDensity.second); // layDown may mirror the mask
        lap(CspMixProfile::flushNs);
    }
    CspColorMixer::layDown(target, rect, mask, paint, strength, m_params.mixingMode);
    lap(CspMixProfile::layDownNs);
    if (m_mixedBuffer) m_mixedBuffer->flush(rect);
    lap(CspMixProfile::flushNs);
    if (profile) CspMixProfile::dabs++;
}

KoColor KisCspBrushPaintOp::dabColor(const KisPaintInformation &info) const
{
    return m_jitter ? m_jitter->apply(painter()->paintColor(), painter()->backgroundColor(), info)
                    : painter()->paintColor();
}

KisPainter *KisCspBrushPaintOp::dabPainter()
{
    return m_strokeBuffer ? m_strokeBuffer->painter() : painter();
}

void KisCspBrushPaintOp::paintDab(KisFixedPaintDeviceSP dab, const QRect &rect, bool preserveDab)
{
    if (m_strokeBuffer) {
        m_strokeBuffer->painter()->bltFixed(rect.topLeft(), dab, dab->bounds());
        m_strokeBuffer->flush(rect, painter());
    } else {
        painter()->bltFixed(rect.topLeft(), dab, dab->bounds());
        painter()->renderMirrorMaskSafe(rect, dab, preserveDab);
    }
}

KisFixedPaintDeviceSP KisCspBrushPaintOp::applyTexture(KisFixedPaintDeviceSP dab, const QPoint &topLeft,
                                                       const KisPaintInformation &info, bool shared)
{
    if (!m_texture) return dab;
    // dabs from KisDabCache may be reused for the next dab: never modify them
    const qreal k = m_params.textureDensity * m_params.textureDensityDynamics.value(info);
    if (k <= 0.0) return dab;
    KisFixedPaintDeviceSP result = shared ? new KisFixedPaintDevice(*dab) : dab;
    m_texture->apply(result, topLeft, k, KisLodTransform::lodToScale(painter()->device()));
    return result;
}

KisSpacingInformation KisCspBrushPaintOp::paintRibbon(const KisPaintInformation &info)
{
    const qreal lodScale = KisLodTransform::lodToScale(painter()->device());
    const DabGeometry g = geometry(info);

    CspRibbonBuilder::Sample sample;
    sample.pos = info.pos();
    sample.brushSize = g.diameter * lodScale;
    sample.thickness = g.ratio;

    m_tips->setColors(dabColor(info), painter()->backgroundColor(), m_params.mixingMode);

    KisFixedPaintDeviceSP dab;
    QRect rect;
    if (m_ribbon->addSample(sample, painter()->device()->compositionSourceColorSpace(),
                            info.randomSource(), &dab, &rect)) {
        // alpha darken (wash mode) keeps overlapping quads of one stroke from darkening
        const std::pair<qreal, qreal> od = opacityAndDensity(info);
        dab = applyTexture(dab, rect.topLeft(), info, false);
        applyOpacityAndDensity(od);
        paintDab(dab, rect, false);
    }
    return KisSpacingInformation(CspRibbonBuilder::sampleSpacing(sample.brushSize));
}

void KisCspBrushPaintOp::stamp(const KisPaintInformation &info, int tipIndex, const QPointF &pos,
                               qreal diameter, qreal ratio, qreal rotation,
                               const std::pair<qreal, qreal> &opacityDensity)
{
    const qreal lodScale = KisLodTransform::lodToScale(painter()->device());

    auto flip = [&](int mode) {
        // TODO confirm the .sut codes: 1 = flip, 2 = random
        return mode == 1 || (mode == 2 && random(info) < 0.5);
    };
    CspTipSet::Variant &tip = m_tips->variant(tipIndex, flip(m_params.flipHorizontal), flip(m_params.flipVertical));

    const qreal scale = diameter * lodScale / tip.longestSide;
    if (KisPaintOpUtils::checkSizeTooSmall(scale, tip.brush->width(), tip.brush->height())) return;

    const KoColor color = dabColor(info);
    m_tips->setColors(color, painter()->backgroundColor(), m_params.mixingMode);

    const KisDabShape shape(scale, ratio, rotation);
    QRect dstRect;
    QElapsedTimer dabTimer;
    if (CspMixProfile::enabled) dabTimer.start();
    KisFixedPaintDeviceSP dab =
        tip.dabCache->fetchDab(painter()->device()->compositionSourceColorSpace(),
                               color, pos, shape, info, 1.0, &dstRect);
    if (CspMixProfile::enabled) {
        CspMixProfile::dabNs += dabTimer.nsecsElapsed();
        if (!m_mixer) CspMixProfile::dabs++; // mixDab() counts the others
    }
    if (!dab || dstRect.isEmpty()) return;

    const bool textured = bool(m_texture);
    dab = applyTexture(dab, dstRect.topLeft(), info, true);

    if (m_mixer) {
        mixDab(dab, dstRect, info, color, diameter * lodScale, opacityDensity);
        return;
    }
    applyOpacityAndDensity(opacityDensity);
    paintDab(dab, dstRect, !textured && !tip.dabCache->needSeparateOriginal());
}

KisSpacingInformation KisCspBrushPaintOp::paintSpray(const KisPaintInformation &info)
{
    const DabGeometry g = geometry(info);
    const KisSpacingInformation spacingInfo = spacing(info, g.diameter);
    const qreal lodScale = KisLodTransform::lodToScale(painter()->device());

    if (!m_sprayDistance) m_sprayDistance = sprayRadialDistribution(m_params.sprayBias);

    const qreal radius = 0.5 * g.diameter * lodScale;
    const int count = qMax(1, qRound(m_params.sprayDensity * m_params.sprayDensityDynamics.value(info)));
    qreal particle = qMax(0.0, m_params.spraySize * m_params.spraySizeDynamics.value(info));
    if (m_params.spraySizeFollowsBrush) particle *= g.diameter / m_params.size;

    // one plot: all its particles share opacity and density
    const std::pair<qreal, qreal> od = opacityAndDensity(info);
    KisRandomSourceSP rs = info.randomSource();

    for (int i = 0; i < count; i++) {
        const int tipIndex = m_tips->nextTipIndex(rs);
        if (tipIndex < 0) {
            m_finished = true;
            break;
        }

        // SprayBrush::paintImpl: a uniform angle, then the distance from the radial distribution
        const qreal angle = rs ? KisSprayUniformDistribution()(rs) * 2.0 * M_PI : 0.0;
        const qreal distance = rs ? m_sprayDistance(rs) * radius : 0.0;
        const QPointF offset(distance * std::cos(angle), distance * std::sin(angle));

        const qreal rotation = sprayParticleRotation(m_params, info.drawingAngle(), angle, random(info));

        stamp(info, tipIndex, info.pos() + offset, particle, g.ratio, normalizeAngle(rotation), od);
    }
    return spacingInfo;
}

KisSpacingInformation KisCspBrushPaintOp::paintAt(const KisPaintInformation &info)
{
    if (!painter()->device() || m_finished) return KisSpacingInformation(1.0);
    if (m_ribbon) return paintRibbon(info);
    if (m_params.useSpray) return paintSpray(info);

    const DabGeometry g = geometry(info);
    const KisSpacingInformation spacingInfo = spacing(info, g.diameter);

    const int tipIndex = m_tips->nextTipIndex(info.randomSource());
    if (tipIndex < 0) {
        m_finished = true;
        return spacingInfo;
    }

    stamp(info, tipIndex, info.pos(), g.diameter, g.ratio, dabRotation(m_params, info, random(info)),
          opacityAndDensity(info));
    return spacingInfo;
}

KisSpacingInformation KisCspBrushPaintOp::updateSpacingImpl(const KisPaintInformation &info) const
{
    if (m_ribbon) {
        const qreal lodScale = KisLodTransform::lodToScale(painter()->device());
        return KisSpacingInformation(CspRibbonBuilder::sampleSpacing(geometry(info).diameter * lodScale));
    }
    return spacing(info, geometry(info).diameter);
}

KisTimingInformation KisCspBrushPaintOp::updateTimingImpl(const KisPaintInformation &info) const
{
    Q_UNUSED(info);
    // CSP "Continuous spraying": keep plotting while the pen rests
    return m_params.continuousSpray ? KisTimingInformation(1000.0 / 60.0) : KisTimingInformation();
}
