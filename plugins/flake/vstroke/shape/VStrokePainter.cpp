/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokePainter.h"

#include <KoCompositeOpRegistry.h>
#include <KisRunnableStrokeJobData.h>
#include <KisRunnableStrokeJobsInterface.h>
#include <brushengine/KisPerStrokeRandomSource.h>
#include <brushengine/kis_paintop_registry.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_debug.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_paintop.h>
#include <kis_random_source.h>
#include <kis_timing_information.h>

struct VStrokePainter::Private
{
    KisPainter painter;
    KisDistanceInformation ownDistance;
    KisDistanceInformation *distance = &ownDistance;
    KisRandomSourceSP randomSource;
    KisPerStrokeRandomSourceSP perStrokeRandomSource;
    qreal mergeOpacity = 1.0;

    void doAsynchronousUpdate(bool forceEnd);
};

void VStrokePainter::Private::doAsynchronousUpdate(bool forceEnd)
{
    KisPaintOp *op = painter.paintOp();
    if (!op) return;

    // as FreehandStrokeStrategy::tryDoUpdate, without the frame rate limit;
    // the painter's fallback executor runs the jobs synchronously
    for (int i = 0; i < 10000; i++) {
        QVector<KisRunnableStrokeJobData*> jobs;
        bool needsMoreUpdates = false;
        std::tie(std::ignore, needsMoreUpdates) = op->doAsynchronousUpdate(jobs);

        if (!jobs.isEmpty()) {
            painter.runnableStrokeJobsInterface()->addRunnableJobs(jobs);
        }
        if (!forceEnd || !needsMoreUpdates) return;
    }
    warnKrita << "VStrokePainter: the paintop kept asking for updates";
}

VStrokePainter::VStrokePainter(KisPaintDeviceSP device,
                               const VStrokeRenderSettings &settings,
                               KisImageSP image,
                               KisNodeSP node,
                               KisDistanceInformation *distance)
    : m_d(new Private)
{
    if (distance) {
        m_d->distance = distance;
    } else {
        m_d->ownDistance = KisDistanceInitInfo(settings.startDistance).makeDistInfo();
    }
    m_d->randomSource = new KisRandomSource(settings.seed);
    m_d->perStrokeRandomSource = new KisPerStrokeRandomSource(settings.perStrokeSeed);

    KisPainter &gc = m_d->painter;
    gc.begin(device);
    gc.setPaintColor(settings.paintColor);
    gc.setBackgroundColor(settings.backgroundColor);

    // the same choice KisResourcesSnapshot::needsIndirectPainting() makes
    const bool indirect = settings.preset && !settings.preset->settings()->paintIncremental();
    if (indirect) {
        gc.setCompositeOpId(settings.preset->settings()->indirectPaintingCompositeOp());
        gc.setOpacityToUnit();
        m_d->mergeOpacity = settings.opacity;
    } else {
        gc.setCompositeOpId(COMPOSITE_OVER);
        gc.setOpacityF(settings.opacity);
        m_d->mergeOpacity = 1.0;
    }

    // KisPainter asserts when the paintop is missing, so check first
    if (settings.preset && KisPaintOpRegistry::instance()->value(settings.preset->paintOp().id())) {
        if (settings.preset->hasMaskingPreset()) {
            warnKrita << "VStrokePainter: masking brushes are not supported, painting without the mask";
        }
        // the paintop is initialized last, it may read the painter's options
        gc.setPaintOpPreset(settings.preset, node, image);
    }
}

VStrokePainter::~VStrokePainter()
{
    finish();
    m_d->painter.end();
}

bool VStrokePainter::isValid() const
{
    return m_d->painter.paintOp();
}

void VStrokePainter::paint(const VStrokePrimitive &primitive)
{
    if (!isValid()) return;

    // as FreehandStrokeStrategy::doStrokeCallback
    KisPaintInformation pi1 = primitive.pi1;
    pi1.setRandomSource(m_d->randomSource);
    pi1.setPerStrokeRandomSource(m_d->perStrokeRandomSource);

    if (primitive.type == VStrokePrimitive::Point) {
        m_d->painter.paintAt(pi1, m_d->distance);
        m_d->doAsynchronousUpdate(false);
        return;
    }

    KisPaintInformation pi2 = primitive.pi2;
    pi2.setRandomSource(m_d->randomSource);
    pi2.setPerStrokeRandomSource(m_d->perStrokeRandomSource);

    if (primitive.type == VStrokePrimitive::Line) {
        m_d->painter.paintLine(pi1, pi2, m_d->distance);
    } else {
        m_d->painter.paintBezierCurve(pi1, primitive.control1, primitive.control2, pi2, m_d->distance);
    }
    m_d->doAsynchronousUpdate(false);
}

void VStrokePainter::finish()
{
    m_d->doAsynchronousUpdate(true);
}

QVector<QRect> VStrokePainter::takeDirtyRegion()
{
    return m_d->painter.takeDirtyRegion();
}

KisDistanceInitInfo VStrokePainter::startDistance(KisPaintOpPresetSP preset, const QPointF &firstPos, qreal startAngle)
{
    // the intervals of kis_tool_freehand_helper.cpp
    const qreal spacingUpdateInterval = 50.0;
    const qreal timingUpdateInterval = 50.0;

    const bool airbrushing = preset && preset->settings() && preset->settings()->isAirbrushing();
    const bool useSpacingUpdates = preset && preset->settings() && preset->settings()->useSpacingUpdates();

    return KisDistanceInitInfo(firstPos, startAngle,
                               useSpacingUpdates ? spacingUpdateInterval : LONG_TIME,
                               airbrushing ? timingUpdateInterval : LONG_TIME,
                               0);
}

qreal VStrokePainter::mergeOpacity() const
{
    return m_d->mergeOpacity;
}

bool VStrokePainter::render(KisPaintDeviceSP device,
                            const VStrokeRenderSettings &settings,
                            const QVector<VStrokePrimitive> &primitives,
                            KisImageSP image,
                            KisNodeSP node)
{
    // like the temporary device of a freehand stroke, merged afterwards
    KisPaintDeviceSP temp = new KisPaintDevice(device->colorSpace());
    qreal mergeOpacity = 1.0;
    int begin = 0;
    int piece = 0;
    do {
        int end = begin + 1;
        while (end < primitives.size() && !primitives[end].restartsStroke) end++;

        // a new paintop and spacing, on the same device: the piece
        // starts as a stroke does, and doesn't build up on the others
        VStrokeRenderSettings pieceSettings = settings;
        if (piece > 0) {
            pieceSettings.seed = settings.seed + piece;
            pieceSettings.perStrokeSeed = settings.perStrokeSeed + piece;
            pieceSettings.startDistance = startDistance(settings.preset, primitives[begin].pi1.pos(), 0.0);
        }
        // A brush painting indirectly paints a stroke on a device of its own,
        // and may rewrite whole dab rectangles there (the CSP watercolor
        // edge's stroke buffer): a later piece gets a fresh device, laid over
        // the pieces before it, or it would clear them. A brush painting
        // into the layer directly (color mixing) paints over them, as there.
        const bool indirect = settings.preset && !settings.preset->settings()->paintIncremental();
        KisPaintDeviceSP target = piece > 0 && indirect ? new KisPaintDevice(device->colorSpace()) : temp;
        {
            VStrokePainter painter(target, pieceSettings, image, node);
            if (!painter.isValid()) return false;

            mergeOpacity = painter.mergeOpacity();
            for (int i = begin; i < end && i < primitives.size(); i++) {
                painter.paint(primitives[i]);
            }
            painter.finish();
        }
        if (target != temp) merge(temp, target, 1.0);

        begin = end;
        piece++;
    } while (begin < primitives.size());

    merge(device, temp, mergeOpacity);
    return true;
}

void VStrokePainter::merge(KisPaintDeviceSP target, KisPaintDeviceSP coverage, qreal opacity)
{
    const QRect rect = coverage->extent();
    if (rect.isEmpty()) return;

    KisPainter gc(target);
    gc.setCompositeOpId(COMPOSITE_OVER);
    gc.setOpacityF(opacity);
    gc.bitBlt(rect.topLeft(), coverage, rect);
}
