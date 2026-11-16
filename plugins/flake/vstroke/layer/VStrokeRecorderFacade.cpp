/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeRecorderFacade.h"
#include "VTrace.h"

#include <typeinfo>

#include <QByteArray>

#include <KoColorSpaceRegistry.h>
#include <kis_debug.h>
#include <kis_paint_device.h>
#include <kis_stroke.h>
#include <kis_stroke_strategy.h>
#include <strokes/freehand_stroke.h>

struct VStrokeRecorderFacade::Private
{
    VStrokeRenderSettings settings;
    KisDistanceInformation *distance = nullptr;

    /// gives the helper a valid stroke id; it has no jobs
    KisStrokeSP stroke;

    KisPaintDeviceSP device;
    QScopedPointer<VStrokePainter> painter;
    qreal mergeOpacity = 1.0;
    QVector<VStrokePrimitive> primitives;

    void reset() {
        if (stroke) {
            stroke->endStroke();
            stroke.clear();
        }
        painter.reset();
    }
};

namespace {

/**
 * FreehandStrokeStrategy::Data has only inline virtual functions, so
 * kritaui and this plugin each carry their own type_info for it. Android
 * loads plugins with RTLD_LOCAL and compares type_info by address, so a
 * dynamic_cast of the helper's jobs fails there and every stroke records
 * nothing. Compare the mangled names instead.
 */
FreehandStrokeStrategy::Data *freehandData(KisStrokeJobData *data)
{
    if (!data) return nullptr;
    if (FreehandStrokeStrategy::Data *d = dynamic_cast<FreehandStrokeStrategy::Data*>(data)) {
        return d;
    }
    if (qstrcmp(typeid(*data).name(), typeid(FreehandStrokeStrategy::Data).name()) == 0) {
        return static_cast<FreehandStrokeStrategy::Data*>(data);
    }
    return nullptr;
}

} // namespace

VStrokeRecorderFacade::VStrokeRecorderFacade()
    : m_d(new Private)
{
}

VStrokeRecorderFacade::~VStrokeRecorderFacade()
{
    m_d->reset();
}

void VStrokeRecorderFacade::prepare(const VStrokeRenderSettings &settings, KisDistanceInformation *distance)
{
    m_d->settings = settings;
    m_d->distance = distance;
}

KisStrokeId VStrokeRecorderFacade::startStroke(KisStrokeStrategy *strokeStrategy)
{
    // the painting happens here, not in the strategy
    delete strokeStrategy;

    m_d->reset();
    m_d->primitives.clear();
    m_d->device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    m_d->painter.reset(new VStrokePainter(m_d->device, m_d->settings, KisImageSP(), KisNodeSP(), m_d->distance));
    m_d->mergeOpacity = m_d->painter->mergeOpacity();

    m_d->stroke.reset(new KisStroke(new KisStrokeStrategy(QLatin1String("VSTROKE_RECORDER"))));
    return m_d->stroke.toWeakRef();
}

void VStrokeRecorderFacade::addJob(KisStrokeId id, KisStrokeJobData *data)
{
    QScopedPointer<KisStrokeJobData> owner(data);
    if (id.toStrongRef() != m_d->stroke || !m_d->painter) return;

    FreehandStrokeStrategy::Data *d = freehandData(data);
    if (!d) return; // asynchronous update requests: VStrokePainter updates synchronously

    VStrokePrimitive primitive;
    switch (d->type) {
    case FreehandStrokeStrategy::Data::POINT:
        primitive = VStrokePrimitive::point(d->pi1);
        break;
    case FreehandStrokeStrategy::Data::LINE:
        primitive = VStrokePrimitive::line(d->pi1, d->pi2);
        break;
    case FreehandStrokeStrategy::Data::CURVE:
        primitive = VStrokePrimitive::curve(d->pi1, d->control1, d->control2, d->pi2);
        break;
    default:
        warnKrita << "VStrokeRecorderFacade: ignoring a non-freehand painting job" << d->type;
        return;
    }

    m_d->primitives << primitive;
    {
        VTrace::Scope trace("brush.dab");
        m_d->painter->paint(primitive);
    }

    const QVector<QRect> dirty = m_d->painter->takeDirtyRegion();
    if (dirtyCallback && !dirty.isEmpty()) {
        dirtyCallback(dirty);
    }
}

void VStrokeRecorderFacade::endStroke(KisStrokeId id)
{
    if (id.toStrongRef() != m_d->stroke || !m_d->painter) return;

    VTrace::Scope trace("brush.end");
    trace.setDetail(QString::number(m_d->primitives.size()) + " primitives");
    m_d->painter->finish();
    const QVector<QRect> dirty = m_d->painter->takeDirtyRegion();
    m_d->reset();

    if (dirtyCallback && !dirty.isEmpty()) {
        dirtyCallback(dirty);
    }
    if (finishedCallback) {
        finishedCallback();
    }
}

bool VStrokeRecorderFacade::cancelStroke(KisStrokeId id)
{
    if (id.toStrongRef() != m_d->stroke) return false;

    m_d->reset();
    m_d->primitives.clear();
    if (m_d->device) {
        const QRect rc = m_d->device->extent();
        m_d->device->clear();
        if (dirtyCallback && !rc.isEmpty()) {
            dirtyCallback({rc});
        }
    }
    return true;
}

bool VStrokeRecorderFacade::isRunning() const
{
    return m_d->stroke;
}

QVector<VStrokePrimitive> VStrokeRecorderFacade::primitives() const
{
    return m_d->primitives;
}

KisPaintDeviceSP VStrokeRecorderFacade::device() const
{
    return m_d->device;
}

qreal VStrokeRecorderFacade::mergeOpacity() const
{
    return m_d->mergeOpacity;
}

KisPaintDeviceSP VStrokeRecorderFacade::mergedDevice() const
{
    if (!m_d->device) return KisPaintDeviceSP();

    KisPaintDeviceSP merged = new KisPaintDevice(m_d->device->colorSpace());
    VStrokePainter::merge(merged, m_d->device, m_d->mergeOpacity);
    return merged;
}
