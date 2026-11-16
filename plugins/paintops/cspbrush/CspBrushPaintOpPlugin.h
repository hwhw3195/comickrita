/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_PAINTOP_PLUGIN_H
#define CSP_BRUSH_PAINTOP_PLUGIN_H

#include <QObject>
#include <QVariant>

/**
 * Registers the "CSP Brush" engine, a brush engine modelled after
 * Clip Studio Paint's sub tool brushes (see sut2kpp --engine csp).
 */
class CspBrushPaintOpPlugin : public QObject
{
    Q_OBJECT
public:
    CspBrushPaintOpPlugin(QObject *parent, const QVariantList &);
    ~CspBrushPaintOpPlugin() override;
};

#endif // CSP_BRUSH_PAINTOP_PLUGIN_H
