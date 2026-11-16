/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_RENDER_TEST_H
#define CSP_BRUSH_RENDER_TEST_H

#include <QObject>

/**
 * Paints the CSP reference strokes (data/paths.json, the same paths as
 * the CSP sessions) with converted presets and saves PNGs for review and
 * for comparison with CSP's own renders.
 *
 * Presets: data/*.kpp, plus every .kpp in $CSP_PRESET_DIR. The op is created
 * directly (no paintop registry), so only cspbrush presets are rendered.
 * Output: $CSP_RENDER_DIR, else ~/persistent/cspbrush_renders, else the temp dir.
 */
class CspBrushRenderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void render_data();
    void render();
};

#endif // CSP_BRUSH_RENDER_TEST_H
