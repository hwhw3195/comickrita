/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeRenderCache.h"
#include "VTrace.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThreadPool>

#include <KoColorSpaceRegistry.h>
#include <kis_paint_device.h>

#include "VCurveOpacity.h"
#include "VStrokeShape.h"

namespace {

/// renderings kept per stroke: the stroke, a copy (duplicated layer) and transforms in between
const int MaxEntries = 3;

QAtomicInt s_totalRenders(0);
QAtomicInt s_liveCaches(0);
QAtomicInteger<qint64> s_liveImageBytes(0);

qint64 imageBytes(const QList<VStrokeRenderCache::Entry> &entries)
{
    qint64 bytes = 0;
    Q_FOREACH (const VStrokeRenderCache::Entry &e, entries) {
        bytes += e.image.sizeInBytes();
    }
    return bytes;
}

QThreadPool *renderPool()
{
    static QThreadPool *pool = [] () {
        QThreadPool *p = new QThreadPool();
        p->setMaxThreadCount(qMax(1, QThread::idealThreadCount() - 1));
        return p;
    }();
    return pool;
}

}

VStrokeRenderCache::VStrokeRenderCache(VStrokeShape *owner)
    : m_owner(owner)
{
    s_liveCaches.ref();
}

VStrokeRenderCache::~VStrokeRenderCache()
{
    s_liveImageBytes.fetchAndAddRelaxed(-imageBytes(m_entries));
    s_liveCaches.deref();
}

int VStrokeRenderCache::liveCaches()
{
    return s_liveCaches.loadAcquire();
}

qint64 VStrokeRenderCache::liveImageBytes()
{
    return s_liveImageBytes.loadAcquire();
}

QSharedPointer<VStrokeRenderCache> VStrokeRenderCache::detachedCopy(VStrokeShape *owner) const
{
    QSharedPointer<VStrokeRenderCache> copy(new VStrokeRenderCache(owner));
    QMutexLocker l(&m_mutex);
    copy->m_entries = m_entries;
    s_liveImageBytes.fetchAndAddRelaxed(imageBytes(m_entries));
    return copy;
}

quint64 VStrokeRenderCache::currentRevision() const
{
    return m_currentRevision.loadAcquire();
}

void VStrokeRenderCache::setCurrentRevision(quint64 revision)
{
    m_currentRevision.storeRelease(revision);
}

VStrokeShape *VStrokeRenderCache::owner() const
{
    QMutexLocker l(&m_ownerMutex);
    return m_owner;
}

void VStrokeRenderCache::setOwner(VStrokeShape *owner)
{
    QMutexLocker l(&m_ownerMutex);
    m_owner = owner;
}

bool VStrokeRenderCache::lookup(const VStrokeRenderKey &key, Entry *entry) const
{
    QMutexLocker l(&m_mutex);
    Q_FOREACH (const Entry &e, m_entries) {
        if (e.key == key) {
            *entry = e;
            return true;
        }
    }
    return false;
}

bool VStrokeRenderCache::newest(Entry *entry) const
{
    QMutexLocker l(&m_mutex);
    if (m_entries.isEmpty()) return false;
    *entry = m_entries.first();
    return true;
}

bool VStrokeRenderCache::warped(const VStrokeRenderKey &key, const VStrokeRenderKey &from, Entry *entry) const
{
    QMutexLocker l(&m_mutex);
    if (!m_hasWarped || !(m_warped.key == key) || !(m_warpedFrom == from)) return false;
    *entry = m_warped;
    return true;
}

void VStrokeRenderCache::setWarped(const Entry &entry, const VStrokeRenderKey &from)
{
    QMutexLocker l(&m_mutex);
    // a rendering of that revision arrived meanwhile
    if (!m_entries.isEmpty() && m_entries.first().key.revision >= entry.key.revision) return;
    m_warped = entry;
    m_warpedFrom = from;
    m_hasWarped = true;
}

bool VStrokeRenderCache::needsRequest(const VStrokeRenderKey &key) const
{
    QMutexLocker l(&m_mutex);
    if (m_hasPending) return !(m_pending.key == key);
    return !(m_rendering && m_renderingKey == key);
}

void VStrokeRenderCache::insert(const Entry &entry)
{
    QMutexLocker l(&m_mutex);
    storeLocked(entry);
}

void VStrokeRenderCache::storeLocked(const Entry &entry)
{
    // renders finish in the order of their requests, but a rendering
    // inserted meanwhile (the pen tool's) may be newer
    if (!m_entries.isEmpty() && m_entries.first().key.revision > entry.key.revision) return;

    // older revisions are never painted again once this one is here
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->key.revision < entry.key.revision) {
            s_liveImageBytes.fetchAndAddRelaxed(-it->image.sizeInBytes());
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // the same rendering again (a render racing an adopted one): replace it
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->key == entry.key) {
            s_liveImageBytes.fetchAndAddRelaxed(-it->image.sizeInBytes());
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // a stand-in is only painted until its revision is rendered
    if (m_hasWarped && m_warped.key.revision <= entry.key.revision) {
        m_hasWarped = false;
        m_warped = Entry();
    }
    s_liveImageBytes.fetchAndAddRelaxed(entry.image.sizeInBytes());
    m_entries.prepend(entry);
    while (m_entries.size() > MaxEntries) {
        s_liveImageBytes.fetchAndAddRelaxed(-m_entries.last().image.sizeInBytes());
        m_entries.removeLast();
    }
}

void VStrokeRenderCache::request(const VStrokeRenderKey &key,
                                 const VStrokeRenderSettings &settings,
                                 const QVector<VStrokePrimitive> &imagePixelPrimitives,
                                 const VCurveData &curves)
{
    QMutexLocker l(&m_mutex);
    if (m_hasPending && m_pending.key == key) return;
    if (m_rendering && !m_hasPending && m_renderingKey == key) return;

    m_pending = Request{key, settings, imagePixelPrimitives, curves};
    m_hasPending = true;
    if (!m_rendering) {
        startLocked();
    }
}

void VStrokeRenderCache::startLocked()
{
    Request request = m_pending;
    m_hasPending = false;
    m_rendering = true;
    m_renderingKey = request.key;

    // paintops may cache things in their settings: never share them between threads
    if (request.settings.preset) {
        request.settings.preset = request.settings.preset->clone().dynamicCast<KisPaintOpPreset>();
    }

    s_totalRenders.ref();
    QSharedPointer<VStrokeRenderCache> self = sharedFromThis();
    QElapsedTimer queued;
    queued.start();
    renderPool()->start([self, request, queued] () {
        if (VTrace::enabled()) VTrace::record("render.queue", queued.nsecsElapsed() / 1000);
        VTrace::Scope trace("render");
        trace.setDetail(QString::number(request.primitives.size()) + " primitives");
        Entry entry = render(request.key, request.settings, request.primitives);
        entry.curves = request.curves;
        self->finished(entry);
    });
}

void VStrokeRenderCache::finished(const Entry &entry)
{
    insert(entry);
    {
        QMutexLocker l(&m_mutex);
        m_rendering = false;
        if (m_hasPending) {
            startLocked();
        } else {
            m_idle.wakeAll();
        }
    }

    if (QCoreApplication *app = QCoreApplication::instance()) {
        QSharedPointer<VStrokeRenderCache> self = sharedFromThis();
        QMetaObject::invokeMethod(app, [self, entry] () { self->notifyOwner(entry); }, Qt::QueuedConnection);
    }
}

void VStrokeRenderCache::notifyOwner(const Entry &entry)
{
    // GUI thread. The owner's destructor clears m_owner under the same
    // lock, so it waits for this update to finish.
    VTrace::Scope trace("render.notify");
    QMutexLocker l(&m_ownerMutex);
    if (m_owner) {
        m_owner->renderingFinished(entry);
    }
}

int VStrokeRenderCache::totalRenders()
{
    return s_totalRenders.loadAcquire();
}

void VStrokeRenderCache::waitForIdle() const
{
    QMutexLocker l(&m_mutex);
    while (m_rendering || m_hasPending) {
        m_idle.wait(&m_mutex);
    }
}

VStrokeRenderCache::Entry VStrokeRenderCache::render(const VStrokeRenderKey &key,
                                                     const VStrokeRenderSettings &settings,
                                                     const QVector<VStrokePrimitive> &imagePixelPrimitives)
{
    Entry entry;
    entry.key = key;

    // TODO: render in the layer's color space; for now the vector layer
    //       buffer is 8-bit sRGB anyway (see KisShapeLayerCanvas::repaint)
    KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    if (!VStrokePainter::render(device, settings, imagePixelPrimitives)) {
        return entry;
    }
    entry = fromDevice(key, device);
    VCurveOpacity::applyMask(entry.image, entry.offset, settings.opacityCurves, QTransform());
    return entry;
}

VStrokeRenderCache::Entry VStrokeRenderCache::fromDevice(const VStrokeRenderKey &key, KisPaintDeviceSP device)
{
    Entry entry;
    entry.key = key;

    const QRect rect = device->exactBounds();
    if (!rect.isEmpty()) {
        const KoColorSpace *rgb8 = KoColorSpaceRegistry::instance()->rgb8();
        if (*device->colorSpace() == *rgb8) {
            entry.image = device->convertToQImage(rgb8->profile(), rect);
        } else {
            KisPaintDeviceSP converted = new KisPaintDevice(*device);
            converted->convertTo(rgb8);
            entry.image = converted->convertToQImage(rgb8->profile(), rect);
        }
        entry.offset = rect.topLeft();
    }
    return entry;
}
