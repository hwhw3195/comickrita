/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_TEXTURE_STAGE_H
#define CSP_TEXTURE_STAGE_H

#include <QPoint>
#include <QTransform>
#include <QVector>

#include <KisResourcesInterface.h>
#include <kis_types.h>

#include "CspBrushParams.h"

class KisPropertiesConfiguration;
class KisPaintOpSettings;

/**
 * CSP paper texture: modulates the coverage of each dab by a texture
 * fixed to the canvas. The image is a pattern of Krita's library
 * (Texture/Pattern/..., see loadPattern), the settings are CSP's.
 *
 * Per pixel: a = dab coverage, t = texture strength at that canvas pixel
 * (0 = no paper effect, 1 = full; dark texture pixels are strong unless
 * "Reverse density"), k = texture density with dynamics. The CSP texture
 * modes are functions a' = f(a, t, k), kept in one table (modeFunction)
 * so values fitted to CSP reference strokes replace them in one place.
 *
 * Krita paints a CSP stroke with alpha darken (overlaps keep the larger
 * coverage), and f is non-decreasing in a, so applying the canvas-fixed
 * texture to every dab gives the same result as texturing the finished
 * stroke ("Apply by each plot" off) when the density is 100%.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspTextureStage
{
public:
    CspTextureStage(const CspBrushParams &params, const QImage &texture);

    /**
     * The texture image of a preset: Krita's pattern (Texture/Pattern/...), looked up in the
     * library by md5 and name, else the copy embedded in the preset. Empty when the texture is off.
     */
    static QImage loadPattern(const KisPropertiesConfiguration *settings, KisResourcesInterfaceSP resourcesInterface);

    /// Krita's "Auto invert for eraser": on, and the stroke erases (the canvas' effective composite op)
    static bool invertsForEraser(const KisPaintOpSettings *settings);

    bool isEnabled() const { return !m_strength.isEmpty(); }

    /// Texture strength t (0..1) at device pixel (x, y); @p lodScale maps a preview pixel to the canvas
    qreal strengthAt(int x, int y, qreal lodScale = 1.0) const;

    /// Texture mode functions (see the class comment)
    static qreal modeFunction(int mode, qreal a, qreal t, qreal k);

    /// Applies the texture to @p dab placed at @p topLeft, with density @p k
    void apply(KisFixedPaintDeviceSP dab, const QPoint &topLeft, qreal k, qreal lodScale = 1.0) const;

private:
    CspBrushParams m_params;
    QVector<float> m_strength; ///< t per texture pixel
    int m_width {0};
    int m_height {0};
    QTransform m_canvasToTexture;
};

#endif // CSP_TEXTURE_STAGE_H
