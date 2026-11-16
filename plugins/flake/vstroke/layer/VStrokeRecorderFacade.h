/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_RECORDER_FACADE_H
#define VSTROKE_RECORDER_FACADE_H

#include <functional>

#include <QScopedPointer>

#include <kis_image_interfaces.h>
#include <kis_types.h>

#include "VStrokePainter.h"
#include "kritavstroke_export.h"

/**
 * Lets the stock KisToolFreehandHelper (stabilizer, smoothing, pressure
 * curve, airbrush timing, assistants) drive a vector stroke.
 *
 * The helper believes it runs a FreehandStrokeStrategy on an image. This
 * facade drops the strategy, records every POINT/LINE/CURVE job as a
 * VStrokePrimitive and paints it right away with a VStrokePainter, so the
 * live stroke and later re-renders of the recorded primitives match.
 *
 * Everything runs in the GUI thread, synchronously.
 */
class KRITAVSTROKE_EXPORT VStrokeRecorderFacade : public KisStrokesFacade
{
public:
    VStrokeRecorderFacade();
    ~VStrokeRecorderFacade() override;

    /**
     * Prepares the next stroke; call before the helper starts it.
     * \p distance is the helper's drag distance (see VStrokePainter).
     */
    void prepare(const VStrokeRenderSettings &settings, KisDistanceInformation *distance = nullptr);

    /// called with the painted image rects after every primitive
    std::function<void(const QVector<QRect> &)> dirtyCallback;
    /// called when the stroke ends (not when it is cancelled)
    std::function<void()> finishedCallback;

    KisStrokeId startStroke(KisStrokeStrategy *strokeStrategy) override;
    void addJob(KisStrokeId id, KisStrokeJobData *data) override;
    void endStroke(KisStrokeId id) override;
    bool cancelStroke(KisStrokeId id) override;

    bool isRunning() const;

    /// the recorded primitives, in image pixels
    QVector<VStrokePrimitive> primitives() const;

    /// the stroke painted so far, before mergeOpacity() is applied
    KisPaintDeviceSP device() const;
    qreal mergeOpacity() const;

    /// the finished stroke as VStrokePainter::render() gives it
    KisPaintDeviceSP mergedDevice() const;

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif // VSTROKE_RECORDER_FACADE_H
