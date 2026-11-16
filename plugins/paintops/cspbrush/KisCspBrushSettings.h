/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CSP_BRUSH_SETTINGS_H
#define KIS_CSP_BRUSH_SETTINGS_H

#include <kis_outline_generation_policy.h>
#include <kis_paintop_settings.h>

#include "kritacspbrushpaintop_export.h"

class KRITACSPBRUSHPAINTOP_EXPORT KisCspBrushSettings : public KisOutlineGenerationPolicy<KisPaintOpSettings>
{
public:
    KisCspBrushSettings(KisResourcesInterfaceSP resourcesInterface);
    ~KisCspBrushSettings() override;

    bool paintIncremental() override;

    /// The effective composite op when the texture inverts for the eraser (Krita's Auto invert for eraser)
    QList<int> requiredCanvasResources() const override;

    // no Masked Brush: MaskingBrush/... keys of older presets are dropped, so Krita's core doesn't mask the stroke
    void setProperty(const QString &name, const QVariant &value) override;
    void fromXML(const QDomElement &element) override;

    qreal paintOpSize() const override;
    void setPaintOpSize(qreal value) override;
    qreal paintOpAngle() const override;
    void setPaintOpAngle(qreal value) override;
    qreal paintOpOpacity() override;
    void setPaintOpOpacity(qreal value) override;

    bool isAirbrushing() const override;
    qreal airbrushInterval() const override;

    KisOptimizedBrushOutline brushOutline(const KisPaintInformation &info, const OutlineMode &mode, qreal alignForZoom) override;
};

#endif // KIS_CSP_BRUSH_SETTINGS_H
