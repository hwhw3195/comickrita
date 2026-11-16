/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CSP_BRUSH_PAGES_H
#define KIS_CSP_BRUSH_PAGES_H

#include <QList>
#include <KisResourcesInterface.h>
#include <lager/cursor.hpp>

#include "CspBrushData.h"

class KisCspBrushOptionPage;

/// The editor pages in their order: General (reads and writes the preset), Brush tip, Color mixing, Color jitter,
/// Spraying effect, Texture, Watercolor edge, CSP Velocity Calibration. Krita's Blending Mode page goes after
/// Brush tip (KisCspBrushSettingsWidget).
/// @p resourcesInterface finds library resources (texture patterns, brush tips)
KRITACSPBRUSHPAINTOP_EXPORT QList<KisCspBrushOptionPage *>
createCspBrushPages(lager::cursor<CspBrushData> data, KisResourcesInterfaceSP resourcesInterface = nullptr);

#endif // KIS_CSP_BRUSH_PAGES_H
