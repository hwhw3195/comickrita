/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QBuffer>
#include <QTest>

#include <testbrush.h>

#include <KisResourceCacheDb.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTag.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <kis_brush.h>

#include "CspTipImport.h"

/**
 * "+Create New" also puts the tip into Krita's brush tip library, under the
 * tags typed for it. Runs with a resource database (testbrush.h), unlike
 * CspBrushWidgetTest.
 */
class CspTipLibraryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testAddToLibrary();
};

namespace {
CspTipImport::Tip twoColorTip(const QString &name)
{
    // gray with alpha, as "Black and white" makes them: black, white and a clear corner
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(qRgba(0, 0, 0, 255));
    for (int x = 4; x < 8; x++) {
        for (int y = 0; y < 8; y++) image.setPixel(x, y, qRgba(255, 255, 255, 200));
    }
    image.setPixel(0, 0, qRgba(0, 0, 0, 0));
    return CspTipImport::Tip{name, image};
}

/// the names of the library's brush tips that have the tag @p tagName
QStringList namesTagged(const QString &tagName)
{
    KisTagModel tagModel(ResourceType::Brushes);
    KisTagSP tag;
    for (int row = 0; row < tagModel.rowCount(); row++) {
        KisTagSP candidate = tagModel.tagForIndex(tagModel.index(row, 0));
        if (candidate && candidate->id() >= 0 && candidate->name() == tagName) tag = candidate;
    }
    QStringList names;
    if (!tag) return names;
    KisTagFilterResourceProxyModel proxy(ResourceType::Brushes);
    proxy.setTagFilter(tag);
    for (int row = 0; row < proxy.rowCount(); row++) {
        names << proxy.index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
    }
    return names;
}

int tagCount(const QString &tagName)
{
    KisTagModel tagModel(ResourceType::Brushes);
    int count = 0;
    for (int row = 0; row < tagModel.rowCount(); row++) {
        KisTagSP tag = tagModel.tagForIndex(tagModel.index(row, 0));
        if (tag && tag->id() >= 0 && tag->name().compare(tagName, Qt::CaseInsensitive) == 0) count++;
    }
    return count;
}
}

void CspTipLibraryTest::testAddToLibrary()
{
    QVERIFY(KisResourceCacheDb::isValid());
    // a name of this run, so that tips left by an earlier run don't count
    const QString name = QString("csp tip %1").arg(QDateTime::currentMSecsSinceEpoch());
    const QString tagA = name + " tag a";
    const QString tagB = name + " tag b";
    const CspTipImport::Tip tip = twoColorTip(name);

    KisResourceModel model(ResourceType::Brushes);
    QVERIFY(model.resourcesForName(name).isEmpty());

    // the tip is in the library, findable by its name, and tagged; the tags did not exist
    KoResourceSP added = CspTipImport::addToLibrary(tip, {tagA, tagB});
    QVERIFY(added);
    QVERIFY(added->resourceId() >= 0);
    QCOMPARE(added->name(), name);
    QVERIFY(added->filename().endsWith(".png"));
    QCOMPARE(model.resourcesForName(name).size(), 1);
    QCOMPARE(namesTagged(tagA), QStringList{name});
    QCOMPARE(namesTagged(tagB), QStringList{name});

    // "Add images" takes it back from the library as it was made: the white part and the alpha too
    KisBrushSP brush = model.resourcesForName(name).first().dynamicCast<KisBrush>();
    QVERIFY(brush);
    QByteArray raw;
    QBuffer buffer(&raw);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(model.exportResource(brush, &buffer));
    QVERIFY(CspTipImport::isCspTipPng(raw));
    const QVector<CspTipImport::Tip> back = CspTipImport::fromBrush(brush, raw);
    QCOMPARE(back.size(), 1);
    QCOMPARE(back[0].name, name);
    QCOMPARE(back[0].image, tip.image);

    // a second tip of the same name gets a file of its own; a tag is found whatever its case, not made twice
    KoResourceSP second = CspTipImport::addToLibrary(tip, {tagA.toUpper()});
    QVERIFY(second);
    QVERIFY(second->filename() != added->filename());
    QCOMPARE(model.resourcesForName(name).size(), 2);
    QCOMPARE(tagCount(tagA), 1);
    QCOMPARE(namesTagged(tagA).size(), 2);
    QCOMPARE(namesTagged(tagB).size(), 1);

    // no tags, an empty image
    QVERIFY(CspTipImport::addToLibrary(twoColorTip(name + " plain")));
    QVERIFY(!CspTipImport::addToLibrary(CspTipImport::Tip{name, QImage()}));

    // a name that cannot be a file name
    KoResourceSP odd = CspTipImport::addToLibrary(twoColorTip(name + " a/b:c*?"));
    QVERIFY(odd);
    QCOMPARE(odd->name(), name + " a/b:c*?");
    QVERIFY(!odd->filename().contains('/') && !odd->filename().contains(':') && !odd->filename().contains('*'));
}

KISTEST_MAIN(CspTipLibraryTest)

#include "CspTipLibraryTest.moc"
