/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_BRUSH_DATA_H
#define CSP_BRUSH_DATA_H

#include <QMap>
#include <QString>
#include <QVariant>
#include <boost/operators.hpp>

#include "kritacspbrushpaintop_export.h"

class KisPropertiesConfiguration;

/**
 * All "CSP/..." properties of a preset, kept verbatim.
 *
 * The keys are the column names of the Clip Studio Paint sub tool
 * "Variant" table (written by sut2kpp --engine csp), so that every
 * setting survives a round trip through the settings widget even
 * while the UI only exposes a few of them. Engine-only keys use an
 * "x-" prefix (e.g. "x-SwapMainSub").
 */
struct KRITACSPBRUSHPAINTOP_EXPORT CspBrushData : boost::equality_comparable<CspBrushData>
{
    static const QString prefix;

    inline friend bool operator==(const CspBrushData &lhs, const CspBrushData &rhs) {
        return lhs.values == rhs.values;
    }

    QMap<QString, QVariant> values; ///< keys without the "CSP/" prefix

    bool contains(const QString &key) const { return values.contains(key); }
    qreal real(const QString &key, qreal defaultValue = 0.0) const;
    int integer(const QString &key, int defaultValue = 0) const;
    bool boolean(const QString &key, bool defaultValue = false) const;
    QString string(const QString &key, const QString &defaultValue = QString()) const;
    void set(const QString &key, const QVariant &value) { values[key] = value; }

    bool read(const KisPropertiesConfiguration *setting);
    void write(KisPropertiesConfiguration *setting) const;

    /// A round brush that paints out of the box (no preset loaded)
    static CspBrushData defaultBrush();
};

#endif // CSP_BRUSH_DATA_H
