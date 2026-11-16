/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokePresetStore.h"

#include <QCryptographicHash>
#include <QDomDocument>
#include <QGlobalStatic>
#include <QTextStream>

#include <KisGlobalResourcesInterface.h>
#include <KisRequiredResourcesOperators.h>
#include <KoEmbeddedResource.h>
#include <KoLocalStrokeCanvasResources.h>
#include <KoResourceLoadResult.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_debug.h>

Q_GLOBAL_STATIC(VStrokePresetStore, s_instance)

const char *VStrokePresetStore::ElementName = "krita:vstroke-preset";

namespace {

QString elementText(const QDomElement &e)
{
    QString text;
    QTextStream stream(&text);
    e.save(stream, -1);
    return text;
}

QString md5Key(const QByteArray &bytes)
{
    return "vsp-" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
}

/// the copy of a linked resource that toXML() embedded in \p presetElement
KoResourceLoadResult embeddedCopy(const QDomElement &presetElement, const KoResourceSignature &sig)
{
    const QDomElement resources = presetElement.firstChildElement("resources");
    QDomElement byName;
    for (QDomElement e = resources.firstChildElement("resource"); !e.isNull(); e = e.nextSiblingElement("resource")) {
        if (e.attribute("type") != sig.type) continue;
        if (!sig.md5sum.isEmpty() && e.attribute("md5sum") == sig.md5sum) {
            byName = e;
            break;
        }
        if (byName.isNull() && (e.attribute("filename") == sig.filename || e.attribute("name") == sig.name)) {
            byName = e;
        }
    }
    if (byName.isNull()) {
        return KoResourceLoadResult(sig);
    }
    const KoResourceSignature embeddedSig(byName.attribute("type"), byName.attribute("md5sum"),
                                          byName.attribute("filename"), byName.attribute("name"));
    return KoResourceLoadResult(KoEmbeddedResource(embeddedSig, QByteArray::fromBase64(byName.text().toLatin1())));
}

}

VStrokePresetStore::VStrokePresetStore()
{
}

VStrokePresetStore::~VStrokePresetStore()
{
    // every document is closed by now: strokes left over are leaks
    prune();
    if (!m_definitions.isEmpty()) {
        qDebug() << "vstroke:" << m_definitions.size() << "brush preset definitions still alive at exit";
    }
}

VStrokePresetStore *VStrokePresetStore::instance()
{
    return s_instance;
}

QString VStrokePresetStore::keyOf(KisPaintOpPresetSP preset)
{
    QDomDocument doc;
    QDomElement e = doc.createElement("Preset");
    preset->settings()->toXML(doc, e);

    // the values as saved, without the types: a loaded preset has them all
    // as strings, and KisPaintOpPreset::fromXML() drops texture settings
    // when the texture is off
    const bool texture = preset->settings()->getBool("Texture/Pattern/Enabled");
    QMap<QString, QString> params;
    for (QDomElement p = e.firstChildElement("param"); !p.isNull(); p = p.nextSiblingElement("param")) {
        const QString name = p.attribute("name");
        if (!texture && name.startsWith("Texture") && name != "Texture/Pattern/Enabled") continue;
        params.insert(name, p.text());
    }

    QByteArray bytes = preset->paintOp().id().toUtf8() + '\n' + preset->name().toUtf8() + '\n';
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        bytes += it.key().toUtf8() + '=' + it.value().toUtf8() + '\n';
    }
    // the linked resources by content: a brush tip changed under the same name paints differently
    Q_FOREACH (const KoResourceLoadResult &r, preset->linkedResources(preset->resourcesInterface())) {
        const KoResourceSignature sig = r.signature();
        bytes += sig.type.toUtf8() + ' ' + sig.md5sum.toUtf8() + ' ' + sig.filename.toUtf8() + '\n';
    }
    return md5Key(bytes);
}

VStrokePresetDefinitionSP VStrokePresetStore::intern(KisPaintOpPresetSP preset)
{
    if (!preset) return VStrokePresetDefinitionSP();

    const QString key = keyOf(preset);
    if (VStrokePresetDefinitionSP existing = find(key)) {
        return existing;
    }

    VStrokePresetDefinition *definition = new VStrokePresetDefinition();
    definition->key = key;
    // our own copy: the caller's snapshot is not guaranteed to stay unchanged
    definition->preset = preset->clone().dynamicCast<KisPaintOpPreset>();

    QDomDocument doc;
    QDomElement root = doc.createElement("Preset");
    definition->preset->toXML(doc, root);
    doc.appendChild(root);
    definition->xml = doc.toString(-1);

    return insert(definition);
}

VStrokePresetDefinitionSP VStrokePresetStore::fromSaved(const QDomElement &element)
{
    const QDomElement presetElement = element.firstChildElement("Preset");
    if (presetElement.isNull()) return VStrokePresetDefinitionSP();

    KisResourcesInterfaceSP globalResources = KisGlobalResourcesInterface::instance();
    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setValid(true);
    preset->fromXML(presetElement, globalResources); // invalid when the paintop is missing

    if (!preset->valid()) {
        // the paintop is missing: keep the settings for saving, draw the outline
        VStrokePresetDefinition *definition = new VStrokePresetDefinition();
        definition->xml = elementText(presetElement);
        definition->key = md5Key(definition->xml.toUtf8());
        if (VStrokePresetDefinitionSP existing = find(definition->key)) {
            delete definition;
            return existing;
        }
        warnKrita << "vstroke: cannot load brush preset" << presetElement.attribute("name")
                  << "- its brush engine" << presetElement.attribute("paintopid") << "is missing";
        return insert(definition);
    }
    preset->setValid(preset->settings()->isValid());

    // as KisRequiredResourcesOperators::createLocalResourcesSnapshot(), but
    // a resource missing here comes from the copy saved with the preset
    QList<KoResourceSP> resources;
    Q_FOREACH (KoResourceLoadResult r, preset->requiredResources(globalResources)) {
        if (r.type() == KoResourceLoadResult::FailedLink) {
            r = embeddedCopy(presetElement, r.signature());
        }
        KisRequiredResourcesOperators::detail::addResourceOrWarnIfNotLoaded(r, &resources, globalResources);
    }
    preset->setResourcesInterface(KisRequiredResourcesOperators::detail::createLocalResourcesStorage(resources));
    if (!preset->requiredCanvasResources().isEmpty()) {
        // the canvas resources at drawing time are not saved; better unset than a crash
        preset->setCanvasResourcesInterface(KoLocalStrokeCanvasResourcesSP(new KoLocalStrokeCanvasResources()));
    }

    const QString key = keyOf(preset);
    if (VStrokePresetDefinitionSP existing = find(key)) {
        return existing;
    }

    VStrokePresetDefinition *definition = new VStrokePresetDefinition();
    definition->key = key;
    definition->preset = preset;
    definition->xml = elementText(presetElement);
    return insert(definition);
}

int VStrokePresetStore::liveCount()
{
    QMutexLocker l(&m_mutex);
    prune();
    return m_definitions.size();
}

VStrokePresetDefinitionSP VStrokePresetStore::find(const QString &key)
{
    QMutexLocker l(&m_mutex);
    return m_definitions.value(key).toStrongRef();
}

VStrokePresetDefinitionSP VStrokePresetStore::insert(VStrokePresetDefinition *definition)
{
    VStrokePresetDefinitionSP result(definition);

    QMutexLocker l(&m_mutex);
    // another thread may have made the same one meanwhile
    if (VStrokePresetDefinitionSP existing = m_definitions.value(definition->key).toStrongRef()) {
        return existing;
    }
    prune();
    m_definitions.insert(definition->key, result);
    return result;
}

void VStrokePresetStore::prune()
{
    for (auto it = m_definitions.begin(); it != m_definitions.end();) {
        if (it.value().isNull()) {
            it = m_definitions.erase(it);
        } else {
            ++it;
        }
    }
}
