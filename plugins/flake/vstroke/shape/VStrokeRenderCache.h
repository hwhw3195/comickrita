/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_RENDER_CACHE_H
#define VSTROKE_RENDER_CACHE_H

#include <QAtomicInteger>
#include <QEnableSharedFromThis>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QRecursiveMutex>
#include <QTransform>
#include <QWaitCondition>

#include "VCurveData.h"
#include "VStrokePainter.h"
#include "kritavstroke_export.h"

class VStrokeShape;

/**
 * Identifies one rendering of a stroke: the stroke data revision and the
 * shape-local to image pixel transform it was rendered with.
 */
struct KRITAVSTROKE_EXPORT VStrokeRenderKey
{
    QTransform transform;
    quint64 revision = 0;

    /**
     * The same rendering: tolerant of rounding (a shape moved into a rotated
     * layer has its absolute transform computed back through the layer's
     * inverse), and of a move by whole image pixels, as the Move tool and
     * layer offsets make. A rendering moved by whole pixels is the same
     * pixels, so it's copied to the new place instead of rendered again.
     * Everything placing a rendering goes through its own key's transform.
     */
    bool operator==(const VStrokeRenderKey &rhs) const {
        if (revision != rhs.revision) return false;
        const QTransform &a = transform;
        const QTransform &b = rhs.transform;
        const qreal m[] = {a.m11() - b.m11(), a.m12() - b.m12(), a.m13() - b.m13(),
                           a.m21() - b.m21(), a.m22() - b.m22(), a.m23() - b.m23(),
                           a.m33() - b.m33()};
        for (qreal d : m) {
            if (qAbs(d) > 1e-9) return false;
        }
        const qreal dx = a.m31() - b.m31();
        const qreal dy = a.m32() - b.m32();
        return qAbs(dx - qRound(dx)) < 1e-6 && qAbs(dy - qRound(dy)) < 1e-6;
    }
};

/**
 * The brush renderings of one stroke, shared by a shape and the clones
 * KoShapeManager paints from (in a worker thread).
 *
 * A render is requested when a paint finds no rendering for its key, so
 * only strokes that are actually painted get rendered. Renders run on a
 * thread pool, at most one per cache; requests made meanwhile are collapsed
 * into the newest one. When a render finishes, the owner shape gets
 * renderingFinished() in the GUI thread.
 *
 * The image is the finished stroke in image pixels, converted to 8-bit sRGB
 * the way KisShapeLayerCanvas converts its tiles.
 */
class KRITAVSTROKE_EXPORT VStrokeRenderCache : public QEnableSharedFromThis<VStrokeRenderCache>
{
public:
    struct Entry {
        VStrokeRenderKey key;
        QImage image;      ///< null when the stroke painted nothing
        QPoint offset;     ///< position of the image in image pixels
        /// the shape-local curves it was rendered from, for the warped stand-in; may be empty
        VCurveData curves;
    };

    explicit VStrokeRenderCache(VStrokeShape *owner);

    /// a new cache for \p owner holding the same renderings
    QSharedPointer<VStrokeRenderCache> detachedCopy(VStrokeShape *owner) const;

    VStrokeShape *owner() const;
    void setOwner(VStrokeShape *owner);

    /**
     * The revision of the owner's curves (set by the owner in the GUI
     * thread). A copy painting with an older revision belongs to a repaint
     * prepared before the change, and a newer repaint follows.
     */
    quint64 currentRevision() const;
    void setCurrentRevision(quint64 revision);

    /// the rendering for \p key
    bool lookup(const VStrokeRenderKey &key, Entry *entry) const;

    /**
     * The newest rendering, for any transform. Only the newest revision's
     * renderings are kept: older ones are dropped when a newer one arrives.
     */
    bool newest(Entry *entry) const;

    /// whether request() would queue a render for \p key (it is neither queued nor rendering)
    bool needsRequest(const VStrokeRenderKey &key) const;

    /// stores a rendering made elsewhere (the live stroke of the pen tool)
    void insert(const Entry &entry);

    /// \p curves: the shape-local curves, kept with the rendering (Entry::curves)
    void request(const VStrokeRenderKey &key,
                 const VStrokeRenderSettings &settings,
                 const QVector<VStrokePrimitive> &imagePixelPrimitives,
                 const VCurveData &curves = VCurveData());

    /**
     * The warped stand-in made for \p key from the rendering at \p from
     * (VStrokeShape::paintWarped()), so the tiles of a repaint warp once.
     * One is kept; a rendering of its revision or newer drops it.
     */
    bool warped(const VStrokeRenderKey &key, const VStrokeRenderKey &from, Entry *entry) const;
    void setWarped(const Entry &entry, const VStrokeRenderKey &from);

    /// blocks until no render is running or queued (for tests)
    void waitForIdle() const;

    /// renders started so far, by all caches (for tests: catches render loops)
    static int totalRenders();

    /// caches alive and the bytes of their images (for tests: catches leaks)
    static int liveCaches();
    static qint64 liveImageBytes();

    ~VStrokeRenderCache();

    /// \p device converted to an entry, as a render job converts its result
    static Entry fromDevice(const VStrokeRenderKey &key, KisPaintDeviceSP device);

    /// renders synchronously, as a render job would
    static Entry render(const VStrokeRenderKey &key,
                        const VStrokeRenderSettings &settings,
                        const QVector<VStrokePrimitive> &imagePixelPrimitives);

private:
    struct Request {
        VStrokeRenderKey key;
        VStrokeRenderSettings settings;
        QVector<VStrokePrimitive> primitives;
        VCurveData curves;
    };

    void startLocked();
    void storeLocked(const Entry &entry);
    void finished(const Entry &entry);
    void notifyOwner(const Entry &entry);

private:
    mutable QMutex m_mutex;
    mutable QWaitCondition m_idle;
    QList<Entry> m_entries;          ///< newest first
    bool m_rendering = false;
    VStrokeRenderKey m_renderingKey;

    /**
     * Guards m_owner, and is held while the owner gets its update(), so the
     * owner cannot be destroyed (in any thread) in the middle of it.
     * Recursive: update() may synchronously clone or destroy shapes sharing
     * this cache, in the same thread.
     */
    mutable QRecursiveMutex m_ownerMutex;
    VStrokeShape *m_owner = nullptr;
    QAtomicInteger<quint64> m_currentRevision {0};
    bool m_hasPending = false;
    Request m_pending;
    bool m_hasWarped = false;
    Entry m_warped;
    VStrokeRenderKey m_warpedFrom;
};

typedef QSharedPointer<VStrokeRenderCache> VStrokeRenderCacheSP;

#endif // VSTROKE_RENDER_CACHE_H
