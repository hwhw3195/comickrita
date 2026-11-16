/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspBrushRenderTest.h"

#include <QDir>
#include <QFile>
#include <QDomDocument>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTest>

#include <kistest.h>

#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_paint_device.h>
#include <kis_painter.h>

#include "KisCspBrushPaintOp.h"
#include "KisCspBrushSettings.h"

namespace {
QString dataPath(const QString &name)
{
    return QString(FILES_DATA_DIR) + name;
}

QString outputDir()
{
    QString dir = qEnvironmentVariable("CSP_RENDER_DIR");
    if (dir.isEmpty()) {
        const QString persistent = QDir::homePath() + "/persistent";
        dir = QDir(persistent).exists() ? persistent + "/cspbrush_renders" : QDir::tempPath() + "/cspbrush_renders";
    }
    QDir().mkpath(dir);
    return dir;
}

QVector<QPointF> toPoints(const QJsonArray &array)
{
    QVector<QPointF> points;
    for (const QJsonValue &v : array) {
        const QJsonArray xy = v.toArray();
        points << QPointF(xy[0].toDouble(), xy[1].toDouble());
    }
    return points;
}

}

void CspBrushRenderTest::render_data()
{
    QTest::addColumn<QString>("preset");
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("mixingMode"); ///< -1: as stored in the preset; 2: as stored, with watercolor edge

    QStringList files;
    for (const QString &name : QDir(FILES_DATA_DIR).entryList({"*.kpp"}, QDir::Files, QDir::Name)) {
        files << dataPath(name);
    }
    const QString extra = qEnvironmentVariable("CSP_PRESET_DIR");
    if (!extra.isEmpty()) {
        for (const QString &name : QDir(extra).entryList({"*.kpp"}, QDir::Files, QDir::Name)) {
            files << QDir(extra).filePath(name);
        }
    }
    for (const QString &file : files) {
        const QString name = QFileInfo(file).completeBaseName();
        QTest::newRow(name.toUtf8().constData()) << file << name << -1;
        // color mixing presets also in both mixing modes, for comparison
        if (name.startsWith("mix_")) {
            QTest::newRow((name + "_standard").toUtf8().constData()) << file << name + "_standard" << 0;
            QTest::newRow((name + "_perceptual").toUtf8().constData()) << file << name + "_perceptual" << 1;
            QTest::newRow((name + "_edge").toUtf8().constData()) << file << name + "_edge" << 2; // with watercolor edge
        }
    }
}

namespace {
/**
 * Settings from a .kpp without Krita's paintop registry (which does not
 * start in every headless test environment): the preset XML is the PNG's
 * "preset" text chunk.
 */
KisPaintOpSettingsSP loadSettings(const QString &file, QString *paintopId)
{
    QImageReader reader(file, "PNG");
    const QString xml = reader.text("preset");
    QDomDocument doc;
    if (xml.isEmpty() || !doc.setContent(xml)) return nullptr;
    const QDomElement root = doc.documentElement();
    *paintopId = root.attribute("paintopid");

    KisPaintOpSettingsSP settings = new KisCspBrushSettings(KisGlobalResourcesInterface::instance());
    for (QDomElement e = root.firstChildElement("param"); !e.isNull(); e = e.nextSiblingElement("param")) {
        settings->setProperty(e.attribute("name"), e.text());
    }
    return settings;
}

void paintPath(KisPaintOp *op, const QVector<QPointF> &points, qreal pressure)
{
    KisDistanceInformation dist;
    if (points.size() == 1) {
        KisPaintInformation pi(points[0], pressure);
        for (int i = 0; i < 60; i++) {
            pi.setCurrentTime(i * 33.0);
            KisPaintInformation::DistanceInformationRegistrar r = pi.registerDistanceInformation(&dist);
            op->paintAt(pi, &dist);
        }
        return;
    }
    for (int i = 1; i < points.size(); i++) {
        KisPaintInformation p1(points[i - 1], pressure);
        KisPaintInformation p2(points[i], pressure);
        if (i == 1) {
            KisPaintInformation::DistanceInformationRegistrar r = p1.registerDistanceInformation(&dist);
            op->paintAt(p1, &dist);
        }
        op->paintLine(p1, p2, &dist);
    }
}
}

void CspBrushRenderTest::render()
{
    QFETCH(QString, preset);
    QFETCH(QString, name);
    QFETCH(int, mixingMode);

    QString paintopId;
    KisPaintOpSettingsSP settings = loadSettings(preset, &paintopId);
    QVERIFY2(settings, qPrintable(preset));
    if (mixingMode == 2) {
        settings->setProperty("CSP/BrushUseWaterEdge", 1);
        settings->setProperty("CSP/BrushWaterEdgeRadius", 6.0);
        settings->setProperty("CSP/BrushWaterEdgeRadiusUnit", 0);
        settings->setProperty("CSP/BrushWaterEdgeAlphaPower", 60);
        settings->setProperty("CSP/BrushWaterEdgeValuePower", 30);
    } else if (mixingMode >= 0) {
        settings->setProperty("CSP/BrushColorMixingMode", mixingMode);
    }
    QCOMPARE(paintopId, QString("cspbrush"));

    QFile pathsFile(dataPath("paths.json"));
    QVERIFY(pathsFile.open(QIODevice::ReadOnly));
    const QJsonObject paths = QJsonDocument::fromJson(pathsFile.readAll()).object();
    const QJsonArray canvasSize = paths["canvas"].toArray();
    const QRect canvasRect(0, 0, canvasSize[0].toInt(), canvasSize[1].toInt());

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP canvas = new KisPaintDevice(cs);

    const bool mixing = settings->getBool("CSP/BrushUseWaterColor", false);
    const QString canvasKind = mixing ? "chart" : "white";
    if (mixing) {
        QImage chart(dataPath("chart.png"));
        QVERIFY(!chart.isNull());
        canvas->convertFromQImage(chart, nullptr);
    }

    // like Krita's indirect painting: dabs go to a temporary device with
    // alpha darken, merged into the layer afterwards; color mixing paints
    // the layer itself
    KisPaintDeviceSP target = mixing ? canvas : KisPaintDeviceSP(new KisPaintDevice(cs));
    KisPainter gc(target);
    gc.setPaintColor(KoColor(QColor(255, 140, 0), cs)); // the CSP sessions' main color
    gc.setBackgroundColor(KoColor(QColor(255, 255, 255), cs));
    if (!mixing) gc.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);

    // one op per stroke, as Krita creates one per stroke
    const QJsonObject strokes = paths[canvasKind].toObject();
    for (auto it = strokes.constBegin(); it != strokes.constEnd(); ++it) {
        KisCspBrushPaintOp op(settings, &gc, nullptr, nullptr);
        paintPath(&op, toPoints(it.value().toArray()), 1.0);
    }

    if (!mixing) {
        KisPainter merge(canvas);
        merge.bitBlt(canvasRect.topLeft(), target, canvasRect);
    }

    QImage result = canvas->convertToQImage(nullptr, canvasRect);
    QImage onWhite(result.size(), QImage::Format_RGB32);
    onWhite.fill(Qt::white);
    {
        QPainter painter(&onWhite);
        painter.drawImage(0, 0, result);
    }
    const QString out = outputDir() + "/" + name + ".png";
    QVERIFY(onWhite.save(out));
    QVERIFY(!canvas->exactBounds().isEmpty());
}

KISTEST_MAIN(CspBrushRenderTest)
