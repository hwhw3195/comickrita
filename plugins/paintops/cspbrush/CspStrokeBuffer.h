/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_STROKE_BUFFER_H
#define CSP_STROKE_BUFFER_H

#include <QRect>
#include <QString>
#include <QVector>
#include <memory>

#include <kis_types.h>

#include "CspBrushParams.h"

class KisPainter;
class KoColorSpace;

/**
 * Accumulates the dabs of one stroke in a private device (alpha darken,
 * like Krita's indirect painting) and writes the post-processed stroke to
 * the painter's device for every changed area. Effects that depend on the
 * whole stroke, like CSP's watercolor edge, are recomputed from the
 * buffer each time, so they never compound.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspStrokeBuffer
{
public:
    CspStrokeBuffer(const CspBrushParams &params, const KoColorSpace *cs);
    ~CspStrokeBuffer();

    /// Margin a changed area grows by when it is written out, for edge radius @p radius (device px)
    int margin(qreal radius) const;

    /// Painter that draws into the stroke buffer
    KisPainter *painter() { return m_painter.get(); }

    /// Writes the processed stroke around @p changed to @p target (non-incremental, mirrored)
    void flush(const QRect &changed, KisPainter *target);

    /**
     * Watercolor edge for alpha values @p alpha (w x h, row-major): returns
     * the edge strength per pixel, 0 inside, rising towards the border.
     */
    static QVector<float> edgeStrength(const QVector<float> &alpha, int w, int h, int radius);

private:
    CspBrushParams m_params;
    const KoColorSpace *m_cs;
    KisPaintDeviceSP m_stroke;
    std::unique_ptr<KisPainter> m_painter;
};

/**
 * Color mixing with a watercolor edge or a blend mode. Mixing reads the
 * layer it paints on, so it paints into a working copy of the layer; the
 * layer as it was at the stroke start is kept aside, and the stroke's
 * coverage is tracked. Every changed area is rewritten as the mixed color
 * over the original, weighted by the coverage, in the painter's composite
 * op, with the edge's Opacity and Darkness effects when the edge is on.
 * Recomputed from the original, so one stroke never compounds.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspMixedStrokeBuffer
{
public:
    /// @p target paints on the layer (direct painting): its selection, mirroring and channel flags apply
    CspMixedStrokeBuffer(const CspBrushParams &params, KisPainter *target);
    ~CspMixedStrokeBuffer();

    /// The working copy: mixing samples and lays down here (mirrored, without selection)
    KisPaintDeviceSP device() const { return m_working; }
    KisPainter *painter() { return m_painter.get(); }
    /// The layer as it was when the stroke started
    KisPaintDeviceSP original() const { return m_original; }

    /// Adds the dab's coverage (@p mask, Alpha8 with @p rect as bounds, up to @p opacity at the rate of @p flow), mirrored
    void addCoverage(const QRect &rect, KisFixedPaintDeviceSP mask, qreal opacity, qreal flow = 1.0);

    /// Rewrites the layer around @p changed and its mirrored copies
    void flush(const QRect &changed);

private:
    void flushRect(const QRect &out, int radius);

    CspBrushParams m_params;
    KisPainter *m_target;
    QString m_compositeOpId;
    KisPaintDeviceSP m_original;
    KisPaintDeviceSP m_working;
    KisPaintDeviceSP m_coverage;
    std::unique_ptr<KisPainter> m_painter;
};

#endif // CSP_STROKE_BUFFER_H
