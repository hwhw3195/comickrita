/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCspBrushSettings.h"

#include <KisOptimizedBrushOutline.h>
#include <KoCanvasResourcesIds.h>
#include <QPainterPath>
#include <kis_texture_option.h>

#include "CspBrushData.h"
#include "CspBrushParams.h"

namespace {
// literals, not CspBrushData::prefix + ...: namespace-scope statics of
// other files may not be initialized yet when these are
const QString sizeKey = QStringLiteral("CSP/BrushSize");
const QString sizeUnitKey = QStringLiteral("CSP/BrushSizeUnit");
const QString angleKey = QStringLiteral("CSP/BrushRotation");
const QString opacityKey = QStringLiteral("CSP/Opacity");
}

KisCspBrushSettings::KisCspBrushSettings(KisResourcesInterfaceSP resourcesInterface)
    : KisOutlineGenerationPolicy<KisPaintOpSettings>(KisCurrentOutlineFetcher::MIRROR_OPTION, resourcesInterface)
{
}

KisCspBrushSettings::~KisCspBrushSettings()
{
}

namespace {
bool isMaskingKey(const QString &name)
{
    return name.startsWith(QLatin1String("MaskingBrush/"));
}
}

void KisCspBrushSettings::setProperty(const QString &name, const QVariant &value)
{
    if (isMaskingKey(name)) return;
    KisOutlineGenerationPolicy<KisPaintOpSettings>::setProperty(name, value);
}

void KisCspBrushSettings::fromXML(const QDomElement &element)
{
    KisOutlineGenerationPolicy<KisPaintOpSettings>::fromXML(element);
    for (const QString &key : getPropertiesKeys()) {
        if (isMaskingKey(key)) removeProperty(key);
    }
}

QList<int> KisCspBrushSettings::requiredCanvasResources() const
{
    QList<int> result = KisOutlineGenerationPolicy<KisPaintOpSettings>::requiredCanvasResources();
    if (KisTextureOption::requiresEffectiveCompositeOp(this)) result << KoCanvasResource::CurrentEffectiveCompositeOp;
    return result;
}

bool KisCspBrushSettings::paintIncremental()
{
    // Opacity is a per-stroke cap in CSP, which Krita's indirect painting
    // ("wash" mode) does. Color mixing reads the layer it paints on, so it
    // paints into the layer directly (plan, Mode B). A ribbon doesn't mix.
    // Keep in sync with CspBrushParams::mixingActive(); this runs often, so it doesn't decode the tips.
    const QString &p = CspBrushData::prefix;
    const bool ribbon = getBool(p + "BrushRibbon", false) && getBool(p + "BrushUsePatternImage", false)
        && getInt(p + "Tips/Count", 0) > 0 && !getBool(p + "BrushUseSpray", false);
    return getBool(p + "BrushUseWaterColor", false) && !ribbon;
}

qreal KisCspBrushSettings::paintOpSize() const
{
    const qreal raw = getDouble(sizeKey, 20.0);
    return getInt(sizeUnitKey, 0) ? raw * 100.0 : raw;
}

void KisCspBrushSettings::setPaintOpSize(qreal value)
{
    setProperty(sizeKey, value);
    setProperty(sizeUnitKey, 0);
}

qreal KisCspBrushSettings::paintOpAngle() const
{
    return getDouble(angleKey, 0.0);
}

void KisCspBrushSettings::setPaintOpAngle(qreal value)
{
    setProperty(angleKey, value);
}

qreal KisCspBrushSettings::paintOpOpacity()
{
    return getDouble(opacityKey, 100.0) / 100.0;
}

void KisCspBrushSettings::setPaintOpOpacity(qreal value)
{
    setProperty(opacityKey, qRound(value * 100.0));
}

bool KisCspBrushSettings::isAirbrushing() const
{
    return getBool(CspBrushData::prefix + "BrushContinuousPlot", false);
}

qreal KisCspBrushSettings::airbrushInterval() const
{
    return 1000.0 / 60.0;
}

KisOptimizedBrushOutline KisCspBrushSettings::brushOutline(const KisPaintInformation &info, const OutlineMode &mode, qreal alignForZoom)
{
    KisOptimizedBrushOutline path;
    if (mode.isVisible) {
        const qreal radius = 0.5 * paintOpSize();
        QPainterPath realOutline;
        realOutline.addEllipse(QPointF(), radius, radius);
        path = outlineFetcher()->fetchOutline(info, this, realOutline, mode, alignForZoom);

        if (mode.showTiltDecoration) {
            const QPainterPath tiltLine = makeTiltIndicator(info, QPointF(), radius, 3.0);
            path.addPath(outlineFetcher()->fetchOutline(info, this, tiltLine, mode, alignForZoom, 1.0, 0.0, true, 0, 0));
        }
    }
    return path;
}
