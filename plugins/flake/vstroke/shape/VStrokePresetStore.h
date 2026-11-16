/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_PRESET_STORE_H
#define VSTROKE_PRESET_STORE_H

#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <QString>

#include <brushengine/kis_paintop_preset.h>

#include "kritavstroke_export.h"

class QDomElement;

/**
 * The brush settings a stroke was drawn with, shared by all strokes drawn
 * with the same settings. Never changes once created.
 */
class KRITAVSTROKE_EXPORT VStrokePresetDefinition
{
public:
    QString key;                ///< "vsp-<md5>" of the settings; the id in saved files
    QString xml;                ///< the <Preset> element (KisPaintOpPreset::toXML(), with
                                ///< the linked resources embedded), written to files as is
    KisPaintOpPresetSP preset;  ///< with a local resources snapshot; null when its paintop is missing
};

typedef QSharedPointer<const VStrokePresetDefinition> VStrokePresetDefinitionSP;

/**
 * The brush settings of all vector strokes alive in the process.
 *
 * Strokes hold their definition; the store only remembers which ones are
 * alive, so a definition goes away with the last stroke (or undo command
 * holding a stroke) that uses it. Saved files hold each definition used by
 * a layer once, in the defs section, and the strokes refer to it.
 *
 * Thread-safe.
 */
class KRITAVSTROKE_EXPORT VStrokePresetStore
{
public:
    VStrokePresetStore();
    ~VStrokePresetStore();

    static VStrokePresetStore *instance();

    /**
     * The definition for the settings of \p preset (a resources snapshot,
     * which must not change afterwards). Serializes the preset when the
     * settings are new, which reads the resource database: GUI thread only.
     */
    VStrokePresetDefinitionSP intern(KisPaintOpPresetSP preset);

    /**
     * The definition saved in \p element, a <krita:vstroke-preset> from the
     * defs of a file. When the paintop is missing, the definition has no
     * preset but keeps the XML, so saving again loses nothing. Returns null
     * when \p element holds no preset at all.
     */
    VStrokePresetDefinitionSP fromSaved(const QDomElement &element);

    /// definitions alive (for tests: catches leaks)
    int liveCount();

    static const char *ElementName;

private:
    static QString keyOf(KisPaintOpPresetSP preset);
    VStrokePresetDefinitionSP find(const QString &key);
    VStrokePresetDefinitionSP insert(VStrokePresetDefinition *definition);
    void prune();

private:
    QMutex m_mutex;
    QHash<QString, QWeakPointer<const VStrokePresetDefinition>> m_definitions;
};

#endif // VSTROKE_PRESET_STORE_H
