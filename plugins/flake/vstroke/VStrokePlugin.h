/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PLUGIN_H
#define VSTROKE_PLUGIN_H

#include <QObject>
#include <QVariantList>

class VStrokePlugin : public QObject
{
    Q_OBJECT
public:
    VStrokePlugin(QObject *parent, const QVariantList &);
};

#endif // VSTROKE_PLUGIN_H
