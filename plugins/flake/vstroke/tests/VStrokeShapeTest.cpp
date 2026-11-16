/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VStrokeShapeTest.h"

#include <QBuffer>
#include <QRegularExpression>
#include <QTest>

#include <kistest.h>

#include <KoColorBackground.h>
#include <KoDocumentResourceManager.h>
#include <KoPathShape.h>
#include <KoShapeContainer.h>
#include <KoShapeRegistry.h>
#include <SvgParser.h>
#include <SvgWriter.h>

#include "VStrokeCurves.h"
#include "VStrokeShape.h"
#include "VStrokeShapeFactory.h"

namespace {

KisPaintInformation info(const QPointF &pos, qreal pressure, qreal time)
{
    KisPaintInformation pi(pos, pressure);
    pi.setCurrentTime(time);
    return pi;
}

/// in image pixels
QVector<VStrokePrimitive> testStroke()
{
    QVector<VStrokePrimitive> stroke;
    KisPaintInformation prev = info(QPointF(40, 120), 0.2, 0);
    stroke << VStrokePrimitive::point(prev);
    for (int i = 1; i <= 6; i++) {
        KisPaintInformation next = info(QPointF(40 + i * 30, 120 + (i % 2) * 10), 0.2 + 0.1 * i, i * 16);
        stroke << VStrokePrimitive::line(prev, next);
        prev = next;
    }
    stroke << VStrokePrimitive::curve(prev, QPointF(260, 200), QPointF(320, 200), info(QPointF(360, 80), 0.5, 120));
    return stroke;
}

const qreal PixelsPerPoint = 300.0 / 72.0;

VStrokeShape *testShape()
{
    VStrokeData settings;
    settings.presetName = "test preset";
    settings.presetMd5 = "0123456789abcdef";
    settings.brushSize = 12.0;
    settings.opacity = 0.75;
    settings.seed = 42;
    settings.perStrokeSeed = -7;
    settings.startAngle = 0.25;

    VStrokeShape *shape = VStrokeShape::fromImagePixels(testStroke(), PixelsPerPoint, settings);
    shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(200, 30, 10))));
    return shape;
}

QByteArray save(KoShape *shape)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    SvgWriter writer({shape});
    writer.save(buffer, QSizeF(100, 60));
    return buffer.data();
}

QList<KoShape*> load(const QByteArray &svg, KoDocumentResourceManager *resources)
{
    QDomDocument doc = SvgParser::createDocumentFromSvg(svg);
    SvgParser parser(resources);
    parser.setResolution(QRectF(0, 0, 400, 240), 72.0 * PixelsPerPoint);
    QSizeF fragmentSize;
    return parser.parseSvg(doc.documentElement(), &fragmentSize);
}

template <class T>
T *findShape(const QList<KoShape*> &shapes)
{
    Q_FOREACH (KoShape *shape, shapes) {
        if (T *t = dynamic_cast<T*>(shape)) return t;
        if (KoShapeContainer *c = dynamic_cast<KoShapeContainer*>(shape)) {
            if (T *t = findShape<T>(c->shapes())) return t;
        }
    }
    return nullptr;
}

void ensureFactory()
{
    if (!KoShapeRegistry::instance()->value(VStrokeShapeId)) {
        // addFactory() also maps the XML element names, add() alone does not
        KoShapeRegistry::instance()->addFactory(new VStrokeShapeFactory());
    }
}

KoDocumentResourceManager *resources()
{
    static KoDocumentResourceManager manager;
    manager.setResource(KoDocumentResourceManager::DocumentResolution, 72.0 * PixelsPerPoint);
    return &manager;
}

void comparePositions(const QVector<VStrokePrimitive> &actual, const QVector<VStrokePrimitive> &expected, qreal tolerance)
{
    QCOMPARE(actual.size(), expected.size());
    for (int i = 0; i < actual.size(); i++) {
        QCOMPARE(actual[i].type, expected[i].type);
        const QVector<QPointF> a = actual[i].positions();
        const QVector<QPointF> e = expected[i].positions();
        for (int j = 0; j < a.size(); j++) {
            QVERIFY2(QLineF(a[j], e[j]).length() < tolerance,
                     qPrintable(QString("primitive %1 point %2: (%3, %4) instead of (%5, %6)")
                                .arg(i).arg(j).arg(a[j].x()).arg(a[j].y()).arg(e[j].x()).arg(e[j].y())));
        }
    }
}

}

void VStrokeShapeTest::testStockReaderFallback()
{
    if (KoShapeRegistry::instance()->value(VStrokeShapeId)) {
        QSKIP("the vstroke plugin is installed and was loaded, cannot read like stock Krita");
    }

    QScopedPointer<VStrokeShape> shape(testShape());
    const QList<KoShape*> shapes = load(save(shape.data()), resources());

    KoPathShape *path = findShape<KoPathShape>(shapes);
    QVERIFY(path);
    QVERIFY(!findShape<VStrokeShape>(shapes));
    QVERIFY(!path->outline().isEmpty());

    QSharedPointer<KoColorBackground> bg = qSharedPointerDynamicCast<KoColorBackground>(path->background());
    QVERIFY(bg);
    QCOMPARE(bg->color().rgb(), QColor(200, 30, 10).rgb());

    // the path is the stroke's outline, in the same place
    const QRectF expected = shape->absoluteOutlineRect();
    const QRectF actual = path->absoluteOutlineRect();
    QVERIFY2(qAbs(actual.left() - expected.left()) < 0.5 && qAbs(actual.top() - expected.top()) < 0.5 &&
             qAbs(actual.right() - expected.right()) < 0.5 && qAbs(actual.bottom() - expected.bottom()) < 0.5,
             qPrintable(QString("outline at %1,%2 %3x%4 instead of %5,%6 %7x%8")
                        .arg(actual.x()).arg(actual.y()).arg(actual.width()).arg(actual.height())
                        .arg(expected.x()).arg(expected.y()).arg(expected.width()).arg(expected.height())));
    qDeleteAll(shapes);
}

void VStrokeShapeTest::testSvgRoundTrip()
{
    ensureFactory();

    QScopedPointer<VStrokeShape> shape(testShape());
    comparePositions(shape->imagePixelPrimitives(), testStroke(), 1e-9);

    const QByteArray svg = save(shape.data());
    const QList<KoShape*> shapes = load(svg, resources());

    VStrokeShape *loaded = findShape<VStrokeShape>(shapes);
    QVERIFY2(loaded, svg.constData());

    QCOMPARE(loaded->resolution(), PixelsPerPoint);
    comparePositions(loaded->imagePixelPrimitives(), testStroke(), 1e-6);

    const VStrokeData &d = loaded->data();
    QCOMPARE(d.presetName, QString("test preset"));
    QCOMPARE(d.presetMd5, QString("0123456789abcdef"));
    QCOMPARE(d.brushSize, 12.0);
    QCOMPARE(d.opacity, 0.75);
    QCOMPARE(d.seed, 42);
    QCOMPARE(d.perStrokeSeed, -7);
    QCOMPARE(d.startAngle, 0.25);
    QCOMPARE(d.compositeOp, COMPOSITE_OVER);
    // the samples read back bit-exact
    QCOMPARE(loaded->curves().attributes, shape->curves().attributes);
    QCOMPARE(loaded->curves().radii, shape->curves().radii);
    QCOMPARE(loaded->curves().flags, shape->curves().flags);
    QCOMPARE(loaded->curves().segments, shape->curves().segments);

    QSharedPointer<KoColorBackground> bg = qSharedPointerDynamicCast<KoColorBackground>(loaded->background());
    QVERIFY(bg);
    QCOMPARE(bg->color().rgb(), QColor(200, 30, 10).rgb());

    qDeleteAll(shapes);
}

void VStrokeShapeTest::testCompositeOpRoundTrip()
{
    ensureFactory();

    // a mode the layer draws as Normal keeps its id too
    for (const QString &op : {COMPOSITE_MULT, COMPOSITE_ERASE, COMPOSITE_HUE}) {
        QScopedPointer<VStrokeShape> shape(testShape());
        VStrokeData data = shape->data();
        data.compositeOp = op;
        QScopedPointer<VStrokeShape> blended(VStrokeShape::fromImagePixels(testStroke(), PixelsPerPoint, data));
        blended->setBackground(shape->background());

        const QList<KoShape*> shapes = load(save(blended.data()), resources());
        VStrokeShape *loaded = findShape<VStrokeShape>(shapes);
        QVERIFY(loaded);
        QCOMPARE(loaded->data().compositeOp, op);
        qDeleteAll(shapes);
    }
}

void VStrokeShapeTest::testOldEraseAttributeLoads_data()
{
    QTest::addColumn<QString>("erase");
    QTest::addColumn<QString>("expected");
    QTest::newRow("erase") << "1" << COMPOSITE_ERASE;
    QTest::newRow("paint") << "0" << COMPOSITE_OVER;
}

void VStrokeShapeTest::testOldEraseAttributeLoads()
{
    QFETCH(QString, erase);
    QFETCH(QString, expected);
    ensureFactory();

    QScopedPointer<VStrokeShape> shape(testShape());
    QString svg = QString::fromUtf8(save(shape.data()));
    QVERIFY(svg.contains("krita:composite-op=\""));
    svg.remove(QRegularExpression("krita:composite-op=\"[^\"]*\""));
    svg.replace("krita:erase=\"0\"", QString("krita:erase=\"%1\"").arg(erase));

    const QList<KoShape*> shapes = load(svg.toUtf8(), resources());
    VStrokeShape *loaded = findShape<VStrokeShape>(shapes);
    QVERIFY(loaded);
    QCOMPARE(loaded->data().compositeOp, expected);
    qDeleteAll(shapes);
}

void VStrokeShapeTest::testLoadVersion2()
{
    ensureFactory();

    // as saved before strokes were stored as curves: one primitive per element
    const QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:krita=\"http://krita.org/namespaces/svg/krita\""
        " width=\"100pt\" height=\"60pt\" viewBox=\"0 0 100 60\">"
        "<path id=\"s1\" transform=\"translate(10, 20)\" fill=\"#c81e0a\" d=\"M0,0 L10,0 L10,10 Z\""
        " krita:type=\"vstroke\" krita:vstroke-version=\"2\" krita:preset-name=\"\" krita:brush-size=\"12\""
        " krita:opacity=\"0.75\" krita:seed=\"42\" krita:per-stroke-seed=\"-7\" krita:start-angle=\"0\" krita:erase=\"0\">"
        "<krita:vstroke-data>"
        "<krita:p type=\"point\"><pi1 pointX=\"0\" pointY=\"0\" pressure=\"0.5\" time=\"0\"/></krita:p>"
        "<krita:p type=\"line\"><pi1 pointX=\"0\" pointY=\"0\" pressure=\"0.5\" time=\"0\"/>"
        "<pi2 pointX=\"20\" pointY=\"0\" pressure=\"0.75\" time=\"16\"/></krita:p>"
        "<krita:p type=\"curve\" c1X=\"30\" c1Y=\"5\" c2X=\"30\" c2Y=\"15\">"
        "<pi1 pointX=\"20\" pointY=\"0\" pressure=\"0.75\" time=\"16\"/>"
        "<pi2 pointX=\"20\" pointY=\"20\" pressure=\"1\" time=\"32\"/></krita:p>"
        "</krita:vstroke-data></path></svg>";
    const QList<KoShape*> shapes = load(svg, resources());

    VStrokeShape *loaded = findShape<VStrokeShape>(shapes);
    QVERIFY(loaded);
    QCOMPARE(loaded->data().brushSize, 12.0);
    QCOMPARE(loaded->data().seed, 42);

    // one sub-path of three samples: the dab, the line and the curve
    const VCurveData &c = loaded->curves();
    QCOMPARE(c.curveCount(), 1);
    QCOMPARE(c.pointCount(), 3);
    QVERIFY(c.hasFlag(0, VCurveData::Dab));
    QCOMPARE(c.segment(0), VCurveData::LineSegment);
    QCOMPARE(c.segment(1), VCurveData::CurveSegment);
    QCOMPARE(c.radii[1], 0.5 * 12.0 * 0.75);

    const QVector<VStrokePrimitive> primitives = VStrokeCurves::toPrimitives(c, 12.0);
    QCOMPARE(primitives.size(), 3);
    QCOMPARE(primitives[0].type, VStrokePrimitive::Point);
    QCOMPARE(primitives[1].type, VStrokePrimitive::Line);
    QCOMPARE(primitives[2].type, VStrokePrimitive::Curve);
    QCOMPARE(primitives[2].pi2.pressure(), 1.0);
    QCOMPARE(primitives[2].pi2.currentTime(), 32.0);

    qDeleteAll(shapes);
}

void VStrokeShapeTest::testPlainPathStillLoadsAsPath()
{
    ensureFactory();

    const QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100pt\" height=\"60pt\" viewBox=\"0 0 100 60\">"
        "<path d=\"M10,10 L50,10 L50,40 Z\" fill=\"#ff0000\"/>"
        "</svg>";
    const QList<KoShape*> shapes = load(svg, resources());

    QVERIFY(!findShape<VStrokeShape>(shapes));
    KoPathShape *path = findShape<KoPathShape>(shapes);
    QVERIFY(path);
    QCOMPARE(path->pointCount(), 3);

    qDeleteAll(shapes);
}

void VStrokeShapeTest::testCloneSharesData()
{
    QScopedPointer<VStrokeShape> shape(testShape());
    QScopedPointer<KoShape> clone(shape->cloneShape());

    VStrokeShape *strokeClone = dynamic_cast<VStrokeShape*>(clone.data());
    QVERIFY(strokeClone);
    // copy-on-write: cloning does not copy the settings or the curves
    QCOMPARE(&strokeClone->data(), &shape->data());
    QCOMPARE(&strokeClone->curves(), &shape->curves());
    QCOMPARE(strokeClone->outline(), shape->outline());
}

void VStrokeShapeTest::testTransformKeepsImagePixels()
{
    QScopedPointer<VStrokeShape> shape(testShape());

    // moving the shape by 10pt moves the stroke by 10pt worth of pixels
    shape->setTransformation(shape->transformation() * QTransform::fromTranslate(10, 0));
    const QVector<VStrokePrimitive> moved =
        VStrokePrimitive::mapped(testStroke(), QTransform::fromTranslate(10 * PixelsPerPoint, 0));
    comparePositions(shape->imagePixelPrimitives(), moved, 1e-9);

    // scaling changes positions only, the brush size stays in pixels
    const QRectF before = shape->outlineRect();
    shape->setSize(shape->size() * 2);
    QCOMPARE(shape->data().brushSize, 12.0);
    QVERIFY(shape->outlineRect().width() > before.width() * 1.5);
}

KISTEST_MAIN(VStrokeShapeTest)
