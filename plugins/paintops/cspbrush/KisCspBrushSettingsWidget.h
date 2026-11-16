/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CSP_BRUSH_SETTINGS_WIDGET_H
#define KIS_CSP_BRUSH_SETTINGS_WIDGET_H

#include <kis_paintop_settings_widget.h>
#include <lager/state.hpp>
#include <KisResourcesInterface.h>
#include <KoCanvasResourcesInterface.h>

#include "CspBrushData.h"

class KRITACSPBRUSHPAINTOP_EXPORT KisCspBrushSettingsWidget : public KisPaintOpSettingsWidget
{
    Q_OBJECT
public:
    KisCspBrushSettingsWidget(QWidget *parent = nullptr, KisResourcesInterfaceSP resourcesInterface = nullptr,
                              KoCanvasResourcesInterfaceSP canvasResourcesInterface = nullptr);
    ~KisCspBrushSettingsWidget() override;

    KisPropertiesConfigurationSP configuration() const override;

    /// The brush size in pixels: Instant Preview is offered from its size threshold up
    lager::reader<qreal> effectiveBrushSize() const override;

private:
    /// every CSP setting of the preset, shared by all pages
    lager::state<CspBrushData, lager::automatic_tag> m_data;
};

#endif // KIS_CSP_BRUSH_SETTINGS_WIDGET_H
