/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCspBrushSettingsWidget.h"

#include <KisCompositeOpOptionWidget.h>
#include <KisGlobalResourcesInterface.h>
#include <KisPaintOpOptionWidgetUtils.h>
#include <klocalizedstring.h>
#include <kis_properties_configuration.h>

#include "KisCspBrushOptionPage.h"
#include "KisCspBrushPages.h"
#include "KisCspBrushSettings.h"

KisCspBrushSettingsWidget::KisCspBrushSettingsWidget(QWidget *parent, KisResourcesInterfaceSP resourcesInterface,
                                                     KoCanvasResourcesInterfaceSP canvasResourcesInterface)
    : KisPaintOpSettingsWidget(parent)
    , m_data(lager::make_state(CspBrushData::defaultBrush(), lager::automatic_tag{}))
{
    namespace kpowu = KisPaintOpOptionWidgetUtils;
    Q_UNUSED(canvasResourcesInterface);
    setObjectName("cspbrush option widget");

    if (!resourcesInterface) resourcesInterface = KisGlobalResourcesInterface::instance();
    // all pages in the General category: Krita groups pages by category, this keeps their order
    const QList<KisCspBrushOptionPage *> pages = createCspBrushPages(m_data, resourcesInterface);
    for (int i = 0; i < pages.size(); i++) {
        addPaintOpOption(pages[i]);
        if (i == 1) addPaintOpOption(kpowu::createOptionWidget<KisCompositeOpOptionWidget>()); // after Brush tip
    }
}

KisCspBrushSettingsWidget::~KisCspBrushSettingsWidget()
{
}

lager::reader<qreal> KisCspBrushSettingsWidget::effectiveBrushSize() const
{
    // as KisCspBrushSettings::paintOpSize(): a physical unit is stored at 10 px per mm (raw x 100)
    return m_data.map([](const CspBrushData &d) {
        const qreal raw = d.real("BrushSize", 20.0);
        return d.integer("BrushSizeUnit", 0) ? raw * 100.0 : raw;
    });
}

KisPropertiesConfigurationSP KisCspBrushSettingsWidget::configuration() const
{
    // a shared pointer from the start: writeConfiguration() takes one and would delete a bare pointer
    KisPaintOpSettingsSP config = new KisCspBrushSettings(resourcesInterface());
    config->setProperty("paintop", "cspbrush");
    writeConfiguration(config);
    return config;
}
