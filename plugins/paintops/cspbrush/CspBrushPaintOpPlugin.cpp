/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushPaintOpPlugin.h"

#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include <brushengine/kis_paintop_registry.h>
#include <kis_simple_paintop_factory.h>

#include "KisCspBrushPaintOp.h"
#include "KisCspBrushSettings.h"
#include "KisCspBrushSettingsWidget.h"

K_PLUGIN_FACTORY_WITH_JSON(CspBrushPaintOpPluginFactory, "kritacspbrushpaintop.json", registerPlugin<CspBrushPaintOpPlugin>();)

CspBrushPaintOpPlugin::CspBrushPaintOpPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KisPaintOpRegistry::instance()->add(
        new KisSimplePaintOpFactory<KisCspBrushPaintOp, KisCspBrushSettings, KisCspBrushSettingsWidget>(
            "cspbrush", i18n("CSP Brush"), KisPaintOpFactory::categoryStable(),
            "krita-paintbrush.png", QString(), QStringList(), 6));
}

CspBrushPaintOpPlugin::~CspBrushPaintOpPlugin()
{
}

#include "CspBrushPaintOpPlugin.moc"
