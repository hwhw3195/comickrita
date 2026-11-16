/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QBuffer>
#include <QImage>
#include <QPainter>

#include <kistest.h>
#include <kis_pointer_utils.h>

#include <KoColorBackground.h>
#include <KoDocumentResourceManager.h>
#include <KoGradientBackground.h>
#include <KoPathShape.h>
#include <KoShapeContainer.h>
#include <KoShapeRegistry.h>
#include <KoShapeStroke.h>
#include <SvgParser.h>
#include <SvgWriter.h>

#include "VCurveConverter.h"
#include "VCurveOpacity.h"
#include "VGeomCurveShape.h"

/**
 * Stock paths converted to freehand vector curves: they look the same as the
 * stock path, keep the fill of a shape cut open, follow stroke width
 * changes and survive saving, also as stock paths for other readers.
 */
class VGeomCurveShapeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testLooksLikeStock_data();
    void testLooksLikeStock();
    void testOpenShapeKeepsFill();
    void testStrokeWidthScalesRadii();
    void testWhatConverts();
    // before the factory is registered
    void testStockReaderFallback();
    void testSvgRoundTrip();
    void testPerPointOpacity();
};

namespace {

KoPathShape *makePath(const QString &kind)
{
    QPainterPath p;
    if (kind == "box") {
        p.addRect(QRectF(30, 30, 140, 90));
    } else if (kind == "ellipse") {
        p.addEllipse(QRectF(30, 30, 160, 110));
    } else if (kind == "polyline") {
        p.moveTo(20, 150);
        p.lineTo(80, 40);
        p.lineTo(130, 160);
        p.lineTo(200, 50);
    } else if (kind == "line") {
        p.moveTo(20, 100);
        p.lineTo(220, 100);
    } else if (kind == "bezier") {
        p.moveTo(20, 160);
        p.cubicTo(60, 10, 160, 10, 200, 160);
    }
    KoPathShape *path = KoPathShape::createShapeFromPainterPath(p);
    path->normalize();
    // as the stock shape tools give them, some at an offset
    path->setTransformation(path->transformation() * QTransform::fromTranslate(5.5, 3.25));
    return path;
}

void setStyle(KoShape *shape, Qt::PenJoinStyle join, Qt::PenCapStyle cap, bool fill)
{
    KoShapeStroke *stroke = new KoShapeStroke(6.0, QColor(20, 40, 200));
    stroke->setJoinStyle(join);
    stroke->setCapStyle(cap);
    stroke->setMiterLimit(4.0);
    shape->setStroke(toQShared(stroke));
    if (fill) {
        shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(QColor(250, 200, 20))));
    }
}

QImage render(const KoShape *shape)
{
    QImage image(240, 200, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(shape->absoluteTransformation());
    shape->paint(painter);
    shape->paintStroke(painter);
    return image;
}

/// pixels whose color differs noticeably, and the pixels painted in \p b
QPair<int, int> difference(const QImage &a, const QImage &b)
{
    int diff = 0, painted = 0;
    for (int y = 0; y < a.height(); y++) {
        const QRgb *la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb *lb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); x++) {
            const int d = qAbs(qRed(la[x]) - qRed(lb[x])) + qAbs(qGreen(la[x]) - qGreen(lb[x])) +
                          qAbs(qBlue(la[x]) - qBlue(lb[x])) + qAbs(qAlpha(la[x]) - qAlpha(lb[x]));
            diff += d > 128;
            painted += qAlpha(lb[x]) > 0;
        }
    }
    return {diff, painted};
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

QByteArray save(KoShape *shape)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    SvgWriter writer({shape});
    writer.save(buffer, QSizeF(240, 200));
    return buffer.data();
}

QList<KoShape*> load(const QByteArray &svg)
{
    static KoDocumentResourceManager resources;
    QDomDocument doc = SvgParser::createDocumentFromSvg(svg);
    SvgParser parser(&resources);
    parser.setResolution(QRectF(0, 0, 240, 200), 72.0);
    QSizeF fragmentSize;
    return parser.parseSvg(doc.documentElement(), &fragmentSize);
}

}

void VGeomCurveShapeTest::testLooksLikeStock_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("join");
    QTest::addColumn<int>("cap");
    QTest::addColumn<bool>("fill");

    QTest::newRow("box-miter") << "box" << int(Qt::MiterJoin) << int(Qt::FlatCap) << true;
    QTest::newRow("box-bevel") << "box" << int(Qt::BevelJoin) << int(Qt::FlatCap) << false;
    QTest::newRow("box-round") << "box" << int(Qt::RoundJoin) << int(Qt::RoundCap) << true;
    QTest::newRow("ellipse") << "ellipse" << int(Qt::MiterJoin) << int(Qt::FlatCap) << true;
    QTest::newRow("polyline-miter") << "polyline" << int(Qt::MiterJoin) << int(Qt::SquareCap) << false;
    QTest::newRow("polyline-round") << "polyline" << int(Qt::RoundJoin) << int(Qt::RoundCap) << false;
    QTest::newRow("bezier") << "bezier" << int(Qt::MiterJoin) << int(Qt::FlatCap) << false;
}

void VGeomCurveShapeTest::testLooksLikeStock()
{
    QFETCH(QString, kind);
    QFETCH(int, join);
    QFETCH(int, cap);
    QFETCH(bool, fill);

    QScopedPointer<KoPathShape> path(makePath(kind));
    setStyle(path.data(), Qt::PenJoinStyle(join), Qt::PenCapStyle(cap), fill);

    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(path.data()));
    QVERIFY(curve);
    QCOMPARE(curve->absoluteTransformation(), path->absoluteTransformation());
    QCOMPARE(curve->curves().curveCount(), path->subpathCount());
    QCOMPARE(curve->curves().radii.first(), 3.0);

    const QImage expected = render(path.data());
    const QImage actual = render(curve.data());
    const QPair<int, int> diff = difference(actual, expected);
    QVERIFY(diff.second > 500);
    // the edges agree to within half a pixel: only antialiasing differs
    QVERIFY2(diff.first < diff.second / 100 + 10,
             qPrintable(QString("%1 pixels differ of %2").arg(diff.first).arg(diff.second)));

    // a click inside a filled shape selects it, and only then
    const QPointF inside = path->outlineRect().center();
    QCOMPARE(curve->outline().contains(inside), fill);
}

void VGeomCurveShapeTest::testOpenShapeKeepsFill()
{
    // a box with one side missing, as a trim would leave it
    KoPathShape path;
    path.moveTo(QPointF(40, 40));
    path.lineTo(QPointF(160, 40));
    path.lineTo(QPointF(160, 140));
    path.lineTo(QPointF(40, 140));
    setStyle(&path, Qt::MiterJoin, Qt::FlatCap, true);

    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(&path));
    QVERIFY(curve);

    const QImage image = render(curve.data());
    // filled inside, and no line across the open side
    QCOMPARE(QColor(image.pixel(100, 90)).rgb(), QColor(250, 200, 20).rgb());
    QCOMPARE(QColor(image.pixel(42, 90)).rgb(), QColor(250, 200, 20).rgb());
    QCOMPARE(qAlpha(image.pixel(37, 90)), 0);
    QCOMPARE(QColor(image.pixel(160, 90)).rgb(), QColor(20, 40, 200).rgb());
}

void VGeomCurveShapeTest::testStrokeWidthScalesRadii()
{
    QScopedPointer<KoPathShape> path(makePath("polyline"));
    setStyle(path.data(), Qt::MiterJoin, Qt::FlatCap, false);
    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(path.data()));

    // one point made thicker, then the whole line through the stroke width
    VCurveData d = curve->curves();
    d.radii[1] = 9.0;
    curve->setCurves(d);

    KoShapeStroke *wider = new KoShapeStroke(*qSharedPointerDynamicCast<KoShapeStroke>(curve->stroke()));
    wider->setLineWidth(12.0);
    curve->setStroke(toQShared(wider));
    QCOMPARE(curve->curves().radii[0], 6.0);
    QCOMPARE(curve->curves().radii[1], 18.0);

    // the stroke's color and join are the line's
    KoShapeStroke *red = new KoShapeStroke(*wider);
    red->setColor(Qt::red);
    red->setJoinStyle(Qt::RoundJoin);
    curve->setStroke(toQShared(red));
    QCOMPARE(curve->lineColor(), QColor(Qt::red));
    QCOMPARE(curve->curves().radii[1], 18.0);
}

void VGeomCurveShapeTest::testWhatConverts()
{
    QScopedPointer<KoPathShape> plain(makePath("box"));
    QVERIFY(VCurveConverter::canConvert(plain.data()));
    setStyle(plain.data(), Qt::MiterJoin, Qt::FlatCap, true);
    QVERIFY(VCurveConverter::canConvert(plain.data()));

    QScopedPointer<KoPathShape> dashed(makePath("box"));
    KoShapeStroke *dashes = new KoShapeStroke(2.0, Qt::black);
    dashes->setLineStyle(Qt::DashLine, QVector<qreal>());
    dashed->setStroke(toQShared(dashes));
    QVERIFY(!VCurveConverter::canConvert(dashed.data()));
    QVERIFY(!VCurveConverter::fromPath(dashed.data()));

    QScopedPointer<KoPathShape> gradient(makePath("box"));
    QLinearGradient g(0, 0, 100, 0);
    g.setColorAt(0, Qt::red);
    g.setColorAt(1, Qt::blue);
    gradient->setBackground(QSharedPointer<KoGradientBackground>(new KoGradientBackground(g)));
    QVERIFY(!VCurveConverter::canConvert(gradient.data()));

    // the others in a list stay as they are
    QList<KoShape*> shapes = {makePath("polyline"), dashed.take()};
    QVERIFY(VCurveConverter::convertInPlace(shapes));
    QVERIFY(dynamic_cast<VGeomCurveShape*>(shapes[0]));
    QVERIFY(!dynamic_cast<VGeomCurveShape*>(shapes[1]));
    qDeleteAll(shapes);
}

void VGeomCurveShapeTest::testSvgRoundTrip()
{
    if (!KoShapeRegistry::instance()->value(VGeomCurveShapeId)) {
        KoShapeRegistry::instance()->addFactory(new VGeomCurveShapeFactory());
    }

    QScopedPointer<KoPathShape> path(makePath("ellipse"));
    setStyle(path.data(), Qt::MiterJoin, Qt::FlatCap, true);
    path->setFillRule(Qt::OddEvenFill);
    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(path.data()));
    VCurveData edited = curve->curves();
    edited.radii[2] = 7.5;
    curve->setCurves(edited);

    const QByteArray svg = save(curve.data());
    const QList<KoShape*> shapes = load(svg);
    VGeomCurveShape *loaded = findShape<VGeomCurveShape>(shapes);
    QVERIFY2(loaded, svg.constData());

    QVERIFY(loaded->curves() == curve->curves());
    QCOMPARE(loaded->fillRule(), Qt::OddEvenFill);
    QCOMPARE(loaded->lineColor(), curve->lineColor());
    QVERIFY(loaded->background());
    QCOMPARE(qSharedPointerDynamicCast<KoShapeStroke>(loaded->stroke())->joinStyle(), Qt::MiterJoin);

    const QPair<int, int> diff = difference(render(loaded), render(curve.data()));
    QVERIFY2(diff.first < 10, qPrintable(QString("%1 pixels differ").arg(diff.first)));
    qDeleteAll(shapes);
}

void VGeomCurveShapeTest::testPerPointOpacity()
{
    // a line fading out from start to end, and kept by saving
    QScopedPointer<KoPathShape> path(makePath("line"));
    setStyle(path.data(), Qt::MiterJoin, Qt::FlatCap, false);
    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(path.data()));
    const QImage opaque = render(curve.data());
    VCurveData d = curve->curves();
    QCOMPARE(d.pointCount(), 2);
    QVERIFY(!VCurveOpacity::isVarying(d));
    d.setAttribute(VCurveOpacity::Attribute, 1, 0.0, 1.0);
    QCOMPARE(d.attribute(VCurveOpacity::Attribute, 0, -1), 1.0);
    QVERIFY(VCurveOpacity::isVarying(d));
    curve->setCurves(d);
    const QImage faded = render(curve.data());

    const QPointF a = curve->absoluteTransformation().map(d.positions[0]);
    const QPointF b = curve->absoluteTransformation().map(d.positions[1]);
    for (qreal f : {0.1, 0.5, 0.9}) {
        const QPoint p = (a + f * (b - a)).toPoint();
        QCOMPARE(qAlpha(opaque.pixel(p)), 255);
        QVERIFY2(qAbs(qAlpha(faded.pixel(p)) - 255 * (1 - f)) < 12,
                 qPrintable(QString("at %1: %2").arg(f).arg(qAlpha(faded.pixel(p)))));
        // the same color, only fainter
        QVERIFY(qAbs(qRed(qUnpremultiply(faded.pixel(p))) - 20) <= 3);
    }

    const QList<KoShape*> shapes = load(save(curve.data()));
    VGeomCurveShape *loaded = findShape<VGeomCurveShape>(shapes);
    QVERIFY(loaded);
    QVERIFY(loaded->curves() == curve->curves());
    qDeleteAll(shapes);
}

void VGeomCurveShapeTest::testStockReaderFallback()
{
    if (KoShapeRegistry::instance()->value(VGeomCurveShapeId)) {
        QSKIP("the factory is registered (installed plugin or an earlier test), cannot read like stock Krita");
    }

    QScopedPointer<KoPathShape> path(makePath("box"));
    setStyle(path.data(), Qt::MiterJoin, Qt::FlatCap, true);
    QScopedPointer<VGeomCurveShape> curve(VCurveConverter::fromPath(path.data()));

    const QList<KoShape*> shapes = load(save(curve.data()));
    KoPathShape *stock = findShape<KoPathShape>(shapes);
    QVERIFY(stock);
    QVERIFY(!findShape<VGeomCurveShape>(shapes));

    // stock Krita draws the original box
    const QPair<int, int> diff = difference(render(stock), render(path.data()));
    QVERIFY2(diff.first < diff.second / 100 + 10, qPrintable(QString("%1 pixels differ").arg(diff.first)));
    qDeleteAll(shapes);
}

KISTEST_MAIN(VGeomCurveShapeTest)

#include "VGeomCurveShapeTest.moc"
