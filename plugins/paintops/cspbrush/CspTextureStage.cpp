/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspTextureStage.h"

#include <QBuffer>
#include <QtMath>
#include <cmath>

#include <KisTextureOptionData.h>
#include <KoCanvasResourcesIds.h>
#include <KoCanvasResourcesInterface.h>
#include <KoCompositeOpRegistry.h>
#include <kis_paintop_settings.h>
#include <KoColorSpace.h>
#include <KoResourceLoadResult.h>
#include <kis_debug.h>
#include <kis_fixed_paint_device.h>
#include <resources/KoPattern.h>

QImage CspTextureStage::loadPattern(const KisPropertiesConfiguration *settings,
                                    KisResourcesInterfaceSP resourcesInterface)
{
    KisTextureOptionData data;
    data.read(settings);
    if (!data.isEnabled || data.textureData.isNull()) return QImage();

    const KoResourceLoadResult result = data.textureData.loadLinkedPattern(resourcesInterface);
    KoPatternSP pattern = result.resource<KoPattern>();
    if (!pattern && result.type() == KoResourceLoadResult::EmbeddedResource) {
        // Krita imports embedded patterns when it loads a preset; without the resource system
        // (tests, the render test) the pattern is read here
        QByteArray bytes = result.embeddedResource().data();
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        pattern.reset(new KoPattern(QString()));
        if (!pattern->loadFromDevice(&buffer, resourcesInterface)) pattern.reset();
    }
    if (!pattern) {
        warnKrita << "CSP Brush: couldn't load the texture pattern";
        return QImage();
    }
    return pattern->pattern().convertToFormat(QImage::Format_ARGB32);
}

bool CspTextureStage::invertsForEraser(const KisPaintOpSettings *settings)
{
    KisTextureOptionData data;
    data.read(settings);
    if (!data.isEnabled || !data.autoInvertOnErase || !settings->canvasResourcesInterface()) return false;
    // KisTextureOption: the same resource, requested by KisCspBrushSettings::requiredCanvasResources()
    return settings->canvasResourcesInterface()->resource(KoCanvasResource::CurrentEffectiveCompositeOp).toString()
        == COMPOSITE_ERASE;
}

CspTextureStage::CspTextureStage(const CspBrushParams &params, const QImage &texture)
    : m_params(params)
{
    const QImage image = texture.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return;

    m_width = image.width();
    m_height = image.height();
    m_strength.resize(m_width * m_height);

    const qreal contrast = 1.0 + params.textureContrast;
    for (int y = 0; y < m_height; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < m_width; x++) {
            const QRgb c = line[x];
            // flatten onto white paper: transparent texture pixels have no effect
            const qreal alpha = qAlpha(c) / 255.0;
            const qreal gray = (qGray(c) / 255.0) * alpha + (1.0 - alpha);
            qreal g = (gray - 0.5) * contrast + 0.5 + params.textureBrightness;
            qreal t = 1.0 - qBound(0.0, g, 1.0);
            if (params.textureInvert) t = 1.0 - t;
            if (params.textureEmphasize) t = qBound(0.0, (t - 0.5) * 2.0 + 0.5, 1.0);
            m_strength[y * m_width + x] = float(t);
        }
    }

    QTransform textureToCanvas;
    textureToCanvas.rotate(-params.textureRotation);
    textureToCanvas.scale(params.textureScale, params.textureScale);
    m_canvasToTexture = textureToCanvas.inverted();
}

qreal CspTextureStage::strengthAt(int x, int y, qreal lodScale) const
{
    if (m_strength.isEmpty()) return 0.0;
    const QPointF p = m_canvasToTexture.map(QPointF((x + 0.5) / lodScale, (y + 0.5) / lodScale));
    // bilinear, tiled
    const qreal fx = p.x() - 0.5;
    const qreal fy = p.y() - 0.5;
    const int x0 = int(std::floor(fx));
    const int y0 = int(std::floor(fy));
    const qreal wx = fx - x0;
    const qreal wy = fy - y0;
    auto at = [&](int tx, int ty) {
        tx = ((tx % m_width) + m_width) % m_width;
        ty = ((ty % m_height) + m_height) % m_height;
        return qreal(m_strength[ty * m_width + tx]);
    };
    return (at(x0, y0) * (1 - wx) + at(x0 + 1, y0) * wx) * (1 - wy)
         + (at(x0, y0 + 1) * (1 - wx) + at(x0 + 1, y0 + 1) * wx) * wy;
}

qreal CspTextureStage::modeFunction(int mode, qreal a, qreal t, qreal k)
{
    // Starting hypotheses from CSP's manual; to be replaced by functions
    // fitted to the Session 1 reference strokes (plan, section 5).
    const qreal kt = qBound(0.0, k * t, 1.0);
    qreal r = a;
    switch (mode) {
    case CspBrushParams::TexNormal:
        r = a * (1.0 - kt);
        break;
    case CspBrushParams::TexMultiply:
        r = a * (1.0 - kt);
        break;
    case CspBrushParams::TexSubtract:
        r = a - kt;
        break;
    case CspBrushParams::TexCompare:
        r = qMin(a, 1.0 - kt);
        break;
    case CspBrushParams::TexOutline: {
        // like Subtract, but the texture cuts the coverage through a soft
        // ramp instead of a hard threshold: softer texture and transitions.
        // The ramp narrows with kt, so no texture density leaves a alone.
        const qreal w = qMin(0.25, kt);
        r = w <= 0.0 ? a : a * qBound(0.0, (a - kt + w) / (2.0 * w), 1.0);
        break;
    }
    case CspBrushParams::TexOverlay: {
        const qreal s = 1.0 - t; // paper lightness
        const qreal o = a < 0.5 ? 2.0 * a * s : 1.0 - 2.0 * (1.0 - a) * (1.0 - s);
        r = a + (o - a) * k;
        break;
    }
    case CspBrushParams::TexColorDodge: {
        const qreal s = 1.0 - t;
        const qreal o = s >= 1.0 ? 1.0 : qMin(1.0, a / (1.0 - s));
        r = a + (o - a) * k;
        break;
    }
    case CspBrushParams::TexHardMix: {
        const qreal s = 1.0 - t;
        const qreal o = a + s >= 1.0 ? 1.0 : 0.0;
        r = a + (o - a) * k;
        break;
    }
    case CspBrushParams::TexHeight:
        // the texture eats into the stroke from its edges inwards
        r = kt >= 1.0 ? 0.0 : (a - kt) / (1.0 - kt);
        break;
    default:
        break;
    }
    return qBound(0.0, r, 1.0);
}

void CspTextureStage::apply(KisFixedPaintDeviceSP dab, const QPoint &topLeft, qreal k, qreal lodScale) const
{
    if (!isEnabled() || k <= 0.0) return;

    const KoColorSpace *cs = dab->colorSpace();
    const int pixelSize = cs->pixelSize();
    const QRect bounds = dab->bounds();
    quint8 *pixel = dab->data();
    const int mode = m_params.textureMode;

    for (int y = 0; y < bounds.height(); y++) {
        for (int x = 0; x < bounds.width(); x++, pixel += pixelSize) {
            const qreal a = cs->opacityF(pixel);
            if (a <= 0.0) continue;
            const qreal t = strengthAt(topLeft.x() + x, topLeft.y() + y, lodScale);
            cs->setOpacity(pixel, modeFunction(mode, a, t, k), 1);
        }
    }
}
