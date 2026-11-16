/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>
#include <QDomDocument>
#include <QImage>
#include <QPainter>
#include <QPainterPathStroker>
#include <QSharedDataPointer>
#include <QtMath>
#include <cmath>

#include <kistest.h>

#include "VCurveData.h"
#include "VCurveOutline.h"

/**
 * The per-point curve arrays of the freehand vector layer: building and
 * querying them, copy-on-write, the bit-exact XML round trip, and the
 * outline built from position and radius against Qt's stroker.
 */
class VCurveDataTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testBuildAndQuery();
    void testCopyOnWrite();
    void testXmlRoundTrip();
    void testXmlRejectsBrokenData();

    void testOutlineMatchesStroker_data();
    void testOutlineMatchesStroker();
    void testBevelPastMiterLimit();
    void testVariableRadius();
    void testFillAcrossGap();
};

namespace {

VCurveData polyline(const QVector<QPointF> &points, qreal radius, bool cyclic = false)
{
    VCurveData d;
    d.beginCurve(cyclic);
    for (const QPointF &pt : points) d.addPoint(pt, radius);
    return d;
}

QImage rasterize(const QPainterPath &path)
{
    QImage image(240, 240, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillPath(path, Qt::black);
    return image;
}

/// pixels covered by exactly one of the paths, and the pixels \p b covers
QPair<int, int> difference(const QPainterPath &a, const QPainterPath &b)
{
    const QImage ia = rasterize(a);
    const QImage ib = rasterize(b);
    int diff = 0, area = 0;
    for (int y = 0; y < ia.height(); y++) {
        const QRgb *la = reinterpret_cast<const QRgb*>(ia.constScanLine(y));
        const QRgb *lb = reinterpret_cast<const QRgb*>(ib.constScanLine(y));
        for (int x = 0; x < ia.width(); x++) {
            const bool inA = qAlpha(la[x]) > 127;
            const bool inB = qAlpha(lb[x]) > 127;
            diff += inA != inB;
            area += inB;
        }
    }
    return {diff, area};
}

}

void VCurveDataTest::testBuildAndQuery()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(0, 0), 1);
    d.addPoint(QPointF(10, 0), 2);
    d.addPoint(QPointF(10, 10), 3);
    d.beginCurve(true);
    d.addPoint(QPointF(50, 50), 4);
    d.addPoint(QPointF(60, 50), 5);
    d.addPoint(QPointF(60, 60), 6);

    QCOMPARE(d.curveCount(), 2);
    QCOMPARE(d.pointCount(), 6);
    QCOMPARE(d.curveBegin(1), 3);
    QCOMPARE(d.curveEnd(0), 3);
    QCOMPARE(d.curveEnd(1), 6);
    QCOMPARE(d.curveOf(2), 0);
    QCOMPARE(d.curveOf(3), 1);
    QVERIFY(!d.isCyclic(0));
    QVERIFY(d.isCyclic(1));

    QVERIFY(!d.hasCurveSegments());
    QCOMPARE(d.segment(0), VCurveData::LineSegment);

    // the closing segment of a cyclic sub-path ends at its first point
    d.setCurveSegment(5, QPointF(70, 70), QPointF(40, 60));
    QVERIFY(d.hasCurveSegments());
    QCOMPARE(d.segment(5), VCurveData::CurveSegment);
    QCOMPARE(d.handlesOut[5], QPointF(70, 70));
    QCOMPARE(d.handlesIn[3], QPointF(40, 60));
    QCOMPARE(d.controlBounds(), QRectF(0, 0, 70, 70));

    // missing attributes read as their default; set values create the array
    QCOMPARE(d.attribute("pressure", 1, 0.5), 0.5);
    d.setAttribute("pressure", 1, 0.5, 0.5);
    QVERIFY(!d.attributes.contains("pressure"));
    d.setAttribute("pressure", 1, 0.8, 0.5);
    QCOMPARE(d.attribute("pressure", 0, 0.5), 0.5);
    QCOMPARE(d.attribute("pressure", 1, 0.5), 0.8);

    QVERIFY(!d.hasFlag(2, VCurveData::Dab));
    d.setFlag(2, VCurveData::Dab);
    QVERIFY(d.hasFlag(2, VCurveData::Dab));
    QVERIFY(!d.hasFlag(1, VCurveData::Dab));

    // points added later get every per-point array
    const int i = d.addPoint(QPointF(65, 65), 7);
    QCOMPARE(i, 6);
    QCOMPARE(d.segments.size(), 7);
    QCOMPARE(d.handlesOut.size(), 7);
    QCOMPARE(d.flags.size(), 7);
    QCOMPARE(d.attributes["pressure"].size(), 7);
}

void VCurveDataTest::testCopyOnWrite()
{
    QSharedDataPointer<VCurveData> a(new VCurveData(polyline({QPointF(0, 0), QPointF(10, 0)}, 2)));
    QSharedDataPointer<VCurveData> b = a;
    QVERIFY(a.constData() == b.constData());

    b->positions[1] = QPointF(20, 0);
    QVERIFY(a.constData() != b.constData());
    QCOMPARE(a->positions[1], QPointF(10, 0));
    QCOMPARE(b->positions[1], QPointF(20, 0));
}

void VCurveDataTest::testXmlRoundTrip()
{
    VCurveData d;
    d.beginCurve();
    for (int i = 0; i < 20; i++) {
        d.addPoint(QPointF(i * 3.1234567890123, qSin(i) * 17.000000000001), 0.1 + i / 7.0);
        d.setAttribute("pressure", i, i / 19.0, 1.0);
        d.setAttribute("time", i, i * 16.25, 0.0);
    }
    d.setCurveSegment(4, QPointF(1.0 / 3, 2.0 / 3), QPointF(M_PI, M_E));
    d.setFlag(0, VCurveData::Dab);
    d.beginCurve(true);
    d.addPoint(QPointF(100, 100), 1);
    d.addPoint(QPointF(110, 100), 1);
    d.addPoint(QPointF(110, 110), 1);

    QDomDocument doc;
    QDomElement e = doc.createElement(VCurveData::XmlElement);
    d.toXML(doc, e, QTransform());
    doc.appendChild(e);

    // through text, as in a file
    QDomDocument reread;
    QVERIFY(reread.setContent(doc.toString()));

    VCurveData loaded;
    QVERIFY(VCurveData::fromXML(reread.documentElement(), &loaded));
    QVERIFY(loaded == d);

    // positions and handles are saved mapped
    QDomDocument doc2;
    QDomElement e2 = doc2.createElement(VCurveData::XmlElement);
    d.toXML(doc2, e2, QTransform::fromTranslate(5, 0));
    VCurveData moved;
    QVERIFY(VCurveData::fromXML(e2, &moved));
    QCOMPARE(moved.positions[1], d.positions[1] + QPointF(5, 0));
    QCOMPARE(moved.handlesOut[4], d.handlesOut[4] + QPointF(5, 0));
    QCOMPARE(moved.radii, d.radii);
}

void VCurveDataTest::testXmlRejectsBrokenData()
{
    VCurveData d = polyline({QPointF(0, 0), QPointF(10, 0), QPointF(10, 10)}, 2);

    QDomDocument doc;
    QDomElement e = doc.createElement(VCurveData::XmlElement);
    d.toXML(doc, e, QTransform());

    VCurveData out;
    QVERIFY(VCurveData::fromXML(e, &out));

    QDomElement wrongCount = e.cloneNode().toElement();
    wrongCount.setAttribute("points", 4);
    QVERIFY(!VCurveData::fromXML(wrongCount, &out));

    QDomElement noVersion = e.cloneNode().toElement();
    noVersion.removeAttribute("version");
    QVERIFY(!VCurveData::fromXML(noVersion, &out));

    QDomElement noRadius = e.cloneNode().toElement();
    for (QDomElement a = noRadius.firstChildElement(); !a.isNull(); a = a.nextSiblingElement()) {
        if (a.attribute("name") == "radius") {
            noRadius.removeChild(a);
            break;
        }
    }
    QVERIFY(!VCurveData::fromXML(noRadius, &out));
}

void VCurveDataTest::testOutlineMatchesStroker_data()
{
    QTest::addColumn<int>("join");
    QTest::addColumn<int>("cap");
    QTest::addColumn<bool>("cyclic");
    QTest::addColumn<bool>("bezier");

    QTest::newRow("round-round") << int(Qt::RoundJoin) << int(Qt::RoundCap) << false << false;
    QTest::newRow("miter-flat") << int(Qt::MiterJoin) << int(Qt::FlatCap) << false << false;
    QTest::newRow("miter-square") << int(Qt::MiterJoin) << int(Qt::SquareCap) << false << false;
    QTest::newRow("bevel-flat") << int(Qt::BevelJoin) << int(Qt::FlatCap) << false << false;
    QTest::newRow("miter-closed") << int(Qt::MiterJoin) << int(Qt::FlatCap) << true << false;
    QTest::newRow("round-closed") << int(Qt::RoundJoin) << int(Qt::FlatCap) << true << false;
    QTest::newRow("bezier-round") << int(Qt::RoundJoin) << int(Qt::RoundCap) << false << true;
    QTest::newRow("bezier-miter") << int(Qt::MiterJoin) << int(Qt::FlatCap) << false << true;
}

void VCurveDataTest::testOutlineMatchesStroker()
{
    QFETCH(int, join);
    QFETCH(int, cap);
    QFETCH(bool, cyclic);
    QFETCH(bool, bezier);

    const qreal radius = 6;
    // right angles and a 120 degree turn: no miter reaches the limit
    const QVector<QPointF> points = {QPointF(40, 40), QPointF(180, 40), QPointF(180, 180), QPointF(110, 140)};
    VCurveData d = polyline(points, radius, cyclic);

    QPainterPath centerline;
    centerline.moveTo(points[0]);
    if (bezier) {
        d.setCurveSegment(1, QPointF(220, 80), QPointF(220, 140));
        centerline.lineTo(points[1]);
        centerline.cubicTo(QPointF(220, 80), QPointF(220, 140), points[2]);
        centerline.lineTo(points[3]);
    } else {
        for (int i = 1; i < points.size(); i++) centerline.lineTo(points[i]);
    }
    if (cyclic) centerline.closeSubpath();

    VCurveStrokeStyle style;
    style.join = Qt::PenJoinStyle(join);
    style.cap = Qt::PenCapStyle(cap);
    style.miterLimit = 4;

    QPainterPathStroker stroker;
    stroker.setWidth(2 * radius);
    stroker.setJoinStyle(style.join);
    stroker.setCapStyle(style.cap);
    stroker.setMiterLimit(50); // Qt measures it differently; none of these corners reach either limit
    const QPainterPath expected = stroker.createStroke(centerline);

    const QPair<int, int> diff = difference(VCurveOutline::line(d, style, 1.0, 0.0, 0.05), expected);
    QVERIFY(diff.second > 1000);
    // polygonal disks and flattening against Qt's curves
    QVERIFY2(diff.first < diff.second / 50 + 30,
             qPrintable(QString("%1 differing pixels of %2").arg(diff.first).arg(diff.second)));
}

void VCurveDataTest::testBevelPastMiterLimit()
{
    // a 20 degree corner: the miter is 1 / sin(10 deg) = 5.76 line widths long
    const QPointF corner(200, 120);
    const QPointF dir = QPointF(qCos(qDegreesToRadians(20.0)), qSin(qDegreesToRadians(20.0)));
    VCurveData d = polyline({QPointF(40, 120), corner, corner - dir * 160}, 5);

    VCurveStrokeStyle style;
    style.join = Qt::MiterJoin;
    style.cap = Qt::FlatCap;

    // a point just inside the tip of the miter
    const QPointF tipDirection = QPointF(1, 0) + dir;
    const QPointF nearTip = corner + tipDirection / std::hypot(tipDirection.x(), tipDirection.y()) * 5 * 5.0;

    style.miterLimit = 10;
    QVERIFY(VCurveOutline::line(d, style).contains(nearTip));

    style.miterLimit = 4;
    const QPainterPath beveled = VCurveOutline::line(d, style);
    QVERIFY(!beveled.contains(nearTip));
    QVERIFY(beveled.contains(corner + QPointF(0.3, 0.1)));
    QVERIFY(beveled.contains(corner - QPointF(2, 0)));
}

void VCurveDataTest::testVariableRadius()
{
    VCurveData d;
    d.beginCurve();
    d.addPoint(QPointF(20, 100), 2);
    d.addPoint(QPointF(220, 100), 20);

    const QPainterPath outline = VCurveOutline::line(d, VCurveStrokeStyle());
    QVERIFY(outline.contains(QPointF(210, 100 + 15)));
    QVERIFY(!outline.contains(QPointF(30, 100 + 5)));
    QVERIFY(outline.contains(QPointF(30, 100 + 1.5)));

    // radius scale and minimum
    QVERIFY(VCurveOutline::line(d, VCurveStrokeStyle(), 0.5).contains(QPointF(210, 100 + 8)));
    QVERIFY(!VCurveOutline::line(d, VCurveStrokeStyle(), 0.5).contains(QPointF(210, 100 + 12)));
    QVERIFY(VCurveOutline::line(d, VCurveStrokeStyle(), 0.0, 4.0).contains(QPointF(100, 100 + 3.5)));
}

void VCurveDataTest::testFillAcrossGap()
{
    // three sides of a square, as a closed square cut open by a trim
    VCurveData open = polyline({QPointF(40, 40), QPointF(140, 40), QPointF(140, 140), QPointF(40, 140)}, 2);
    QVERIFY(VCurveOutline::fill(open, Qt::WindingFill).contains(QPointF(90, 90)));
    // the line itself stays open
    QVERIFY(!VCurveOutline::line(open, VCurveStrokeStyle()).contains(QPointF(40, 90)));

    // two pieces of one object fill as one area
    VCurveData pieces = polyline({QPointF(40, 40), QPointF(140, 40)}, 2);
    pieces.beginCurve();
    pieces.addPoint(QPointF(140, 140), 2);
    pieces.addPoint(QPointF(40, 140), 2);
    QVERIFY(VCurveOutline::fill(pieces, Qt::WindingFill).contains(QPointF(90, 90)));

    // closed sub-paths keep their holes
    VCurveData ring = polyline({QPointF(20, 20), QPointF(200, 20), QPointF(200, 200), QPointF(20, 200)}, 2, true);
    ring.beginCurve(true);
    for (const QPointF &pt : {QPointF(80, 80), QPointF(140, 80), QPointF(140, 140), QPointF(80, 140)}) {
        ring.addPoint(pt, 2);
    }
    const QPainterPath ringFill = VCurveOutline::fill(ring, Qt::OddEvenFill);
    QVERIFY(ringFill.contains(QPointF(40, 40)));
    QVERIFY(!ringFill.contains(QPointF(110, 110)));
}

KISTEST_MAIN(VCurveDataTest)

#include "VCurveDataTest.moc"
