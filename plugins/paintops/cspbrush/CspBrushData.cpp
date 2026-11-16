/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushData.h"

#include <kis_properties_configuration.h>

const QString CspBrushData::prefix = QStringLiteral("CSP/");

qreal CspBrushData::real(const QString &key, qreal defaultValue) const
{
    bool ok = false;
    const qreal value = values.value(key).toDouble(&ok);
    return ok ? value : defaultValue;
}

int CspBrushData::integer(const QString &key, int defaultValue) const
{
    bool ok = false;
    const QVariant v = values.value(key);
    // stored as text in .kpp files; "1.0" is a valid integer for us
    const int value = qRound(v.toDouble(&ok));
    return ok ? value : defaultValue;
}

bool CspBrushData::boolean(const QString &key, bool defaultValue) const
{
    const QVariant v = values.value(key);
    if (!v.isValid()) return defaultValue;
    const QString s = v.toString().toLower();
    if (s == "true") return true;
    if (s == "false") return false;
    return integer(key, defaultValue ? 1 : 0) != 0;
}

QString CspBrushData::string(const QString &key, const QString &defaultValue) const
{
    return values.contains(key) ? values.value(key).toString() : defaultValue;
}

bool CspBrushData::read(const KisPropertiesConfiguration *setting)
{
    values.clear();
    // getPropertiesKeys() and getProperty() are virtual: the editor passes a
    // KisLockedPropertiesProxy, whose getProperties() is its own empty map
    for (const QString &key : setting->getPropertiesKeys()) {
        if (key.startsWith(prefix)) {
            values.insert(key.mid(prefix.size()), setting->getProperty(key));
        }
    }
    if (values.isEmpty()) {
        *this = defaultBrush();
    }
    return true;
}

void CspBrushData::write(KisPropertiesConfiguration *setting) const
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        setting->setProperty(prefix + it.key(), it.value());
    }
}

CspBrushData CspBrushData::defaultBrush()
{
    CspBrushData d;
    d.set("BrushSize", 20.0);
    d.set("BrushSizeUnit", 0);
    d.set("Opacity", 100);
    d.set("BrushFlow", 100);
    d.set("BrushHardness", 100);
    d.set("AntiAlias", 2);
    d.set("BrushThickness", 100);
    d.set("BrushAutoIntervalType", 2);
    d.set("BrushInterval", 10.0);
    d.set("BrushUsePatternImage", 0);
    d.set("BrushSizeEffector", "on=p;p=0");
    return d;
}
