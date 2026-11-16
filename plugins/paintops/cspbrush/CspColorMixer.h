/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_COLOR_MIXER_H
#define CSP_COLOR_MIXER_H

#include <QRect>
#include <QVector>

#include <KoColor.h>
#include <kis_types.h>

#include "CspBrushParams.h"

class KisPainter;
class KoColorSpace;

/// Time spent in the stages of color mixing, for the benchmark. Off unless enabled (one bool test per dab).
struct KRITACSPBRUSHPAINTOP_EXPORT CspMixProfile {
    static bool enabled;
    static qint64 prepNs, sampleNs, layDownNs, flushNs;
    static qint64 dabNs; ///< making the dab (tip scaled, rotated and colored), with or without mixing
    static int dabs;
    static void reset();
};

/**
 * CSP "Color mixing": the brush picks up and lays down canvas color.
 *
 * The brush carries one color along the stroke, in all three modes:
 *
 *  - It starts as the canvas color under the first dab with some of the
 *    drawing color mixed in (more with a higher "Amount of paint").
 *  - With every dab it moves towards a target: the canvas color under the
 *    dab mixed with the drawing color by "Amount of paint". How fast is
 *    set by "Color stretch", as a distance in brush diameters over which
 *    half of the difference goes: a high stretch drags the start color far.
 *  - The dab is painted with the carried color.
 *
 * The canvas color is the layer's as it was when the stroke started, so
 * the stroke does not pick up its own paint (the paintop samples a copy).
 * Colors count by their alpha: over an empty layer only the drawing color
 * changes what the brush carries, and with "Amount of paint" 0 the color
 * picked up at the start is dragged to the end of the stroke.
 *
 * The modes differ in their constants and in the alpha:
 *
 *  - Blend: "Density of paint" sets the paint's alpha between the canvas
 *    alpha (0) and opaque (1); the alpha is carried like the color.
 *  - Running color: takes very little canvas color while moving, and at
 *    "Color stretch" 100 never gives up the start color. The canvas color
 *    is sampled from a blurred neighbourhood ("Blur").
 *  - Smear: gives its color up more slowly; it paints opaque, "Density of
 *    paint" is not used.
 *
 * The paint is written into the layer with lerp(canvas, paint, mask)
 * including alpha (premultiplied). All color blends use the brush's mixing
 * mode (Standard / Perceptual).
 *
 * Erasing: the drawing color and "Amount of paint" play no part. Blend and
 * Smear lay down the canvas color they carry and Running color the canvas
 * color, and the alpha is the canvas alpha times (1 - density).
 *
 * The constants are fitted to strokes drawn in Clip Studio Paint (vault:
 * "2026-11-15 CSP color mixing rework plan"); the test
 * CspBrushStrokeTest::testMatchesCspReference holds the measurements.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspColorMixer
{
public:
    explicit CspColorMixer(const CspBrushParams &params, bool erasing = false);

    /**
     * Paint color for one dab. @p canvas is the canvas color under it, @p distance how far the brush
     * moved since the previous dab, in brush diameters (not used for the first dab).
     */
    KoColor paintColor(const KoColor &canvas, const KoColor &brush, qreal amount, qreal density,
                       qreal distance = 1.0);

    /// Most pixels sampleCanvas() looks at; a larger rect is sampled on a regular grid
    static constexpr int MaxSamples = 4096;

    /**
     * Canvas color under @p rect, weighted by @p weights (one 0..255 per pixel, or empty = uniform).
     * Above @p maxSamples pixels (0 = never) a regular grid of at most about that many pixels is used instead.
     */
    static KoColor sampleCanvas(KisPaintDeviceSP device, const QRect &rect, const QVector<qint16> &weights,
                                CspColorMath::Mode mode = CspColorMath::Mode::Standard, int maxSamples = MaxSamples);
    /// The same, with the weights as the bytes of a mask (@p mask null = uniform), converted only where sampled
    static KoColor sampleCanvas(KisPaintDeviceSP device, const QRect &rect, const quint8 *mask,
                                CspColorMath::Mode mode = CspColorMath::Mode::Standard, int maxSamples = MaxSamples);

    /**
     * Writes @p paint through @p painter inside @p rect: per pixel
     * lerp(canvas, paint, mask / 255 * strength), with the painter's
     * selection, mirroring and channel flags. @p mask is Alpha8 with @p rect as bounds.
     * Perceptual mixes each pixel with its own canvas color.
     */
    static void layDown(KisPainter *painter, const QRect &rect, KisFixedPaintDeviceSP mask,
                        const KoColor &paint, qreal strength,
                        CspColorMath::Mode mode = CspColorMath::Mode::Standard);

    /// A copy of a dab under the painter's mirror tools: where it goes and how the dab is flipped
    struct MirrorCopy {
        QRect rect;
        bool flipHorizontally {false};
        bool flipVertically {false};
    };
    /// The dab at @p rect and its mirrored copies (the first is the dab itself)
    static QVector<MirrorCopy> mirrorCopies(KisPainter *painter, const QRect &rect);

    /// "Color stretch" as the distance, in brush diameters, over which the carried color covers half of
    /// the way to its target. Infinite for Running color at 100
    static qreal halfDistance(qreal colorStretch, int mixingType);
    /// Share of the canvas color in the color a stroke starts with
    static qreal startCanvasShare(qreal amount, int mixingType);
    /// Share of the canvas color in the color the brush settles at on an opaque canvas
    static qreal canvasShare(qreal amount, int mixingType);
    /// "Density of paint" as the alpha the paint settles at over an empty layer (Smear: not used)
    static qreal densityAlpha(qreal density, int mixingType);

private:
    CspBrushParams m_params;
    // the color the brush carries, kept in float: a step smaller than one 8-bit level must add up
    CspColorMath::Rgb m_carried {0.0f, 0.0f, 0.0f}; ///< gamma-encoded sRGB, 0..1
    float m_carriedWeight {0.0f};                   ///< how much color it holds: 0 = none yet
    float m_carriedAlpha {0.0f};                    ///< the alpha it paints with (Blend, Running color)
    CspColorMath::Spectrum m_carriedLog {};         ///< Perceptual: the carried color as a log-spectrum
    bool m_carriedLogValid {false};
    const KoColorSpace *m_space {nullptr};          ///< the color space of the paint it returns
    bool m_erasing {false};
    bool m_started {false};
};

#endif // CSP_COLOR_MIXER_H
