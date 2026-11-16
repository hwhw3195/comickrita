/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_TEST_UTILS_H
#define VSTROKE_TEST_UTILS_H

#include <QDomDocument>
#include <QImageReader>

#include <KisGlobalResourcesInterface.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_auto_brush.h>
#include <kis_circle_mask_generator.h>
#include <kis_simple_paintop_factory.h>

#include <KisScatterOptionData.h>

#include "KisBrushOpSettings.h"
#include "kis_brushop.h"
#include "kis_brushop_settings_widget.h"

#include "KisCspBrushPaintOp.h"
#include "KisCspBrushSettings.h"
#include "KisCspBrushSettingsWidget.h"

#include "VStrokePrimitive.h"

namespace VStrokeTestUtils {

/**
 * Registers the two paintops when the plugin loader did not find them
 * (the build tree has no installed plugins).
 */
inline void ensurePaintOps()
{
    KisPaintOpRegistry *r = KisPaintOpRegistry::instance();
    if (!r->value("paintbrush")) {
        r->add(new KisSimplePaintOpFactory<KisBrushOp, KisBrushOpSettings, KisBrushOpSettingsWidget>(
            "paintbrush", "Pixel", KisPaintOpFactory::categoryStable(), "krita-paintbrush.png", QString(), QStringList(), 1));
    }
    if (!r->value("cspbrush")) {
        r->add(new KisSimplePaintOpFactory<KisCspBrushPaintOp, KisCspBrushSettings, KisCspBrushSettingsWidget>(
            "cspbrush", "CSP Brush", KisPaintOpFactory::categoryStable(), "krita-paintbrush.png", QString(), QStringList(), 6));
    }
}

inline KisPaintOpPresetSP makePreset(KisPaintOpSettingsSP settings, const QString &name)
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setSettings(settings);
    preset->setName(name);
    return preset;
}

/// pixel brush: soft round auto brush with full scatter, so every dab uses the random source
inline KisPaintOpPresetSP pixelBrushPreset()
{
    KisPaintOpSettingsSP settings = new KisBrushOpSettings(KisGlobalResourcesInterface::instance());
    settings->setProperty("paintop", "paintbrush");

    KisAutoBrush brush(new KisCircleMaskGenerator(20, 1.0, 0.5, 0.5, 2, true), 0.0, 0.0);
    brush.setSpacing(0.15);
    QDomDocument doc;
    QDomElement e = doc.createElement("Brush");
    brush.toXML(doc, e);
    doc.appendChild(e);
    settings->setProperty("brush_definition", doc.toString());

    KisScatterOptionData scatter;
    scatter.isChecked = true;
    scatter.strengthValue = 2.0;
    scatter.write(settings.data());

    return makePreset(settings, "vstroke_test_pixel_scatter");
}

/// cspbrush: a converted CSP pen with spray turned on
inline KisPaintOpPresetSP cspBrushPreset()
{
    QImageReader reader(QString(CSPBRUSH_DATA_DIR) + "mypen.kpp", "PNG");
    QDomDocument doc;
    if (!doc.setContent(reader.text("preset"))) return KisPaintOpPresetSP();

    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    const QDomElement root = doc.documentElement();
    for (QDomElement p = root.firstChildElement("param"); !p.isNull(); p = p.nextSiblingElement("param")) {
        settings->setProperty(p.attribute("name"), p.text());
    }
    settings->setProperty("paintop", "cspbrush");
    settings->setProperty("CSP/BrushUseSpray", 1);
    settings->setProperty("CSP/BrushSprayDensity", 6);
    settings->setProperty("CSP/BrushSpraySize", 3.0);

    return makePreset(settings, "vstroke_test_csp_spray");
}

/// cspbrush: a converted CSP ribbon brush
inline KisPaintOpPresetSP ribbonBrushPreset()
{
    QImageReader reader(QString(CSPBRUSH_DATA_DIR) + "ribbon_wave.kpp", "PNG");
    QDomDocument doc;
    if (!doc.setContent(reader.text("preset"))) return KisPaintOpPresetSP();

    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    const QDomElement root = doc.documentElement();
    for (QDomElement p = root.firstChildElement("param"); !p.isNull(); p = p.nextSiblingElement("param")) {
        settings->setProperty(p.attribute("name"), p.text());
    }
    settings->setProperty("paintop", "cspbrush");

    return makePreset(settings, "vstroke_test_csp_ribbon");
}

inline KisPaintInformation info(const QPointF &pos, qreal pressure, qreal time)
{
    KisPaintInformation pi(pos, pressure);
    pi.setCurrentTime(time);
    return pi;
}

/// a stroke with every primitive type, as the freehand helper sends them
inline QVector<VStrokePrimitive> testStroke()
{
    QVector<VStrokePrimitive> stroke;
    stroke << VStrokePrimitive::point(info(QPointF(40, 120), 0.2, 0));

    KisPaintInformation prev = info(QPointF(40, 120), 0.2, 0);
    for (int i = 1; i <= 10; i++) {
        KisPaintInformation next = info(QPointF(40 + i * 20, 120 + (i % 2) * 6), 0.2 + 0.08 * i, i * 16);
        stroke << VStrokePrimitive::line(prev, next);
        prev = next;
    }

    KisPaintInformation end = info(QPointF(380, 60), 0.6, 200);
    stroke << VStrokePrimitive::curve(prev, QPointF(300, 160), QPointF(360, 160), end);
    stroke << VStrokePrimitive::point(end);
    return stroke;
}

}

#endif // VSTROKE_TEST_UTILS_H
