/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveData.h"

#include <QDomDocument>
#include <QDomElement>
#include <QTransform>
#include <QtEndian>

#include <kis_algebra_2d.h>

#include <algorithm>
#include <cmath>

const QString VCurveData::XmlElement = QStringLiteral("krita:curves");

namespace {

template <typename T>
QString encode(const QVector<T> &values)
{
    QByteArray bytes(values.size() * int(sizeof(T)), Qt::Uninitialized);
    for (int i = 0; i < values.size(); i++) {
        qToLittleEndian<T>(values[i], bytes.data() + i * sizeof(T));
    }
    return QString::fromLatin1(bytes.toBase64());
}

template <typename T>
bool decode(const QString &text, int expectedSize, QVector<T> *values)
{
    const QByteArray bytes = QByteArray::fromBase64(text.toLatin1());
    if (bytes.size() != expectedSize * int(sizeof(T))) return false;
    values->resize(expectedSize);
    for (int i = 0; i < expectedSize; i++) {
        (*values)[i] = qFromLittleEndian<T>(bytes.constData() + i * sizeof(T));
    }
    return true;
}

QVector<double> flatten(const QVector<QPointF> &points)
{
    QVector<double> values;
    values.reserve(points.size() * 2);
    for (const QPointF &pt : points) {
        values << pt.x() << pt.y();
    }
    return values;
}

QVector<QPointF> unflatten(const QVector<double> &values)
{
    QVector<QPointF> points;
    points.reserve(values.size() / 2);
    for (int i = 0; i + 1 < values.size(); i += 2) {
        points << QPointF(values[i], values[i + 1]);
    }
    return points;
}

void addArray(QDomDocument &doc, QDomElement &parent, const QString &name, const QString &type, const QString &data)
{
    QDomElement e = doc.createElement("krita:a");
    e.setAttribute("name", name);
    e.setAttribute("type", type);
    e.appendChild(doc.createTextNode(data));
    parent.appendChild(e);
}

}

VCurveData &VCurveData::operator=(const VCurveData &rhs)
{
    curveOffsets = rhs.curveOffsets;
    cyclic = rhs.cyclic;
    positions = rhs.positions;
    radii = rhs.radii;
    segments = rhs.segments;
    handlesOut = rhs.handlesOut;
    handlesIn = rhs.handlesIn;
    flags = rhs.flags;
    attributes = rhs.attributes;
    caps = rhs.caps;
    return *this;
}

int VCurveData::curveEnd(int curve) const
{
    return curve + 1 < curveOffsets.size() ? curveOffsets[curve + 1] : positions.size();
}

int VCurveData::curveOf(int index) const
{
    auto it = std::upper_bound(curveOffsets.constBegin(), curveOffsets.constEnd(), index);
    return int(it - curveOffsets.constBegin()) - 1;
}

VCurveData::Segment VCurveData::segment(int index) const
{
    return segments.isEmpty() ? LineSegment : Segment(segments[index]);
}

bool VCurveData::hasCurveSegments() const
{
    return !handlesOut.isEmpty();
}

bool VCurveData::hasFlag(int index, PointFlag flag) const
{
    return !flags.isEmpty() && (flags[index] & flag);
}

qreal VCurveData::attribute(const QString &name, int index, qreal defaultValue) const
{
    auto it = attributes.constFind(name);
    return it == attributes.constEnd() ? defaultValue : (*it)[index];
}

void VCurveData::beginCurve(bool isCyclic)
{
    curveOffsets << positions.size();
    cyclic << (isCyclic ? 1 : 0);
    if (!caps.isEmpty()) caps << 0;
}

int VCurveData::nextIndex(int index) const
{
    const int curve = curveOf(index);
    if (index + 1 < curveEnd(curve)) return index + 1;
    return isCyclic(curve) ? curveBegin(curve) : -1;
}

int VCurveData::previousIndex(int index) const
{
    const int curve = curveOf(index);
    if (index > curveBegin(curve)) return index - 1;
    return isCyclic(curve) ? curveEnd(curve) - 1 : -1;
}

QStringList VCurveData::attributeNames() const
{
    QStringList names = attributes.keys();
    names.sort();
    return names;
}

qreal VCurveData::attributeDefault(const QString &name)
{
    // as KisPaintInformation (VStrokeCurves saves its values under these
    // names); opacity is VCurveOpacity's, density-scale VStrokeCurves'
    return name == QLatin1String("pressure") || name == QLatin1String("perspective") ||
           name == QLatin1String("opacity") || name == QLatin1String("density-scale") ? 1.0 : 0.0;
}

VCurvePoint VCurveData::point(int index, const QStringList &names) const
{
    VCurvePoint p;
    p.position = positions[index];
    p.radius = radii[index];
    p.segment = segment(index);
    p.flags = flags.isEmpty() ? 0 : flags[index];
    p.handleIn = handlesIn.isEmpty() ? p.position : handlesIn[index];
    p.handleOut = handlesOut.isEmpty() ? p.position : handlesOut[index];
    p.attributes.reserve(names.size());
    for (const QString &name : names) {
        p.attributes << attribute(name, index, attributeDefault(name));
    }
    return p;
}

void VCurveData::setCapFlags(int curve, quint8 capFlags)
{
    if (caps.isEmpty()) {
        if (!capFlags) return;
        caps.fill(0, curveOffsets.size());
    }
    caps[curve] = capFlags;
}

VCurveData VCurveData::curve(int c) const
{
    VCurveBuilder builder(attributeNames());
    builder.addCurve(*this, c);
    return builder.take();
}

void VCurveData::append(const VCurveData &other)
{
    if (other.isEmpty()) return;
    QStringList names = attributeNames();
    for (const QString &name : other.attributeNames()) {
        if (!names.contains(name)) names << name;
    }
    VCurveBuilder builder(names);
    for (int c = 0; c < curveCount(); c++) builder.addCurve(*this, c);
    for (int c = 0; c < other.curveCount(); c++) builder.addCurve(other, c);
    *this = builder.take();
}

namespace {
/// the position of the neighbour \p step (-1 or +1) from point \p k of \p pts, skipping
/// ones at the same place as \p from; mirrored through \p from past an open end
QPointF neighbourPosition(const QVector<VCurvePoint> &pts, int k, int step, bool cyclic, const QPointF &from,
                          const QPointF &mirrorOf)
{
    const int n = pts.size();
    int i = k + step;
    for (int guard = 0; guard < n; guard++, i += step) {
        if (cyclic) {
            i = (i % n + n) % n;
        } else if (i < 0 || i >= n) {
            break;
        }
        if (QLineF(pts[i].position, from).length() > 1e-9) return pts[i].position;
    }
    return 2 * from - mirrorOf;
}

/// the uniform Catmull-Rom spline through \p p1 and \p p2 at \p t
QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t)
{
    const qreal t2 = t * t;
    const qreal t3 = t2 * t;
    return 0.5 * ((2 * p1) + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3);
}
}

VCurveData VCurveData::subdividedBy(const std::function<int(const VCurvePoint &, const VCurvePoint &)> &parts, bool smooth,
                                    const QVector<bool> &mask, QVector<bool> *resultMask) const
{
    VCurveBuilder builder(attributeNames());
    if (resultMask) resultMask->clear();
    for (int c = 0; c < curveCount(); c++) {
        const int begin = curveBegin(c);
        const int end = curveEnd(c);
        QVector<VCurvePoint> out;
        for (int i = begin; i < end; i++) out << point(i, builder.names());

        QVector<VCurvePoint> result;
        QVector<bool> masks;
        const int n = out.size();
        const bool cyclic = isCyclic(c) && n > 1;
        const int segmentCount = cyclic ? n : n - 1;
        VCurvePoint current = out[0];
        for (int k = 0; k < segmentCount; k++) {
            const int next = (k + 1) % n;
            VCurvePoint b = next == 0 && !result.isEmpty() ? result[0] : out[next];
            const bool masked = mask.isEmpty() || (begin + k < mask.size() && mask[begin + k]);
            if (masked) {
                const int count = parts(current, b);
                // smoothing places the new points of a line on the Catmull-Rom spline
                // through the points around it; beziers are already smooth and split exactly
                // (a zero-length segment has no direction to bend along, and stays put)
                const bool curveIt = smooth && count > 1 && current.segment != CurveSegment &&
                                     QLineF(out[k].position, out[next].position).length() > 1e-9;
                QPointF p0, p1, p2, p3;
                if (curveIt) {
                    p1 = out[k].position;
                    p2 = out[next].position;
                    p0 = neighbourPosition(out, k, -1, cyclic, p1, p2);
                    p3 = neighbourPosition(out, next, +1, cyclic, p2, p1);
                }
                for (int m = count; m > 1; m--) {
                    // the first of m equal parts of what is left
                    VCurvePoint mid = VCurvePoint::split(current, b, 1.0 / m);
                    if (curveIt) {
                        mid.position = catmullRom(p0, p1, p2, p3, qreal(count - m + 1) / count);
                        mid.handleIn = mid.handleOut = mid.position;
                    }
                    result << current;
                    masks << masked;
                    current = mid;
                }
            }
            result << current;
            masks << masked;
            if (next == 0) {
                // the closing segment changed the first point's incoming handle
                result[0].handleIn = b.handleIn;
            } else {
                current = b;
            }
        }
        if (segmentCount < n) {
            result << current;
            masks << false;
        }

        builder.beginCurve(isCyclic(c), capFlags(c));
        for (const VCurvePoint &p : result) builder.add(p);
        if (resultMask) *resultMask += masks;
    }
    return builder.take();
}

VCurveData VCurveData::subdivided(qreal maxSpacing, const QTransform &toDocument, const QVector<bool> &mask,
                                  bool smooth) const
{
    if (maxSpacing <= 0) return *this;
    return subdividedBy([&] (const VCurvePoint &a, const VCurvePoint &b) {
        // the length of the segment, from its control polygon for beziers (an upper bound)
        const QPointF p0 = toDocument.map(a.position);
        const QPointF p1 = toDocument.map(b.position);
        qreal length = QLineF(p0, p1).length();
        if (a.segment == CurveSegment) {
            const QPointF c1 = toDocument.map(a.handleOut);
            const QPointF c2 = toDocument.map(b.handleIn);
            length = (QLineF(p0, c1).length() + QLineF(c1, c2).length() + QLineF(c2, p1).length() + length) / 2;
        }
        return int(std::ceil(length / maxSpacing - 1e-9));
    }, smooth, mask);
}

VCurveData VCurveData::subdividedSteps(int steps, const QVector<bool> &mask, bool smooth) const
{
    steps = qBound(0, steps, MaxSubdivideSteps);
    VCurveData data = *this;
    QVector<bool> currentMask = mask;
    for (int i = 0; i < steps; i++) {
        QVector<bool> nextMask;
        data = data.subdividedBy([] (const VCurvePoint &, const VCurvePoint &) { return 2; }, smooth, currentMask,
                                 mask.isEmpty() ? nullptr : &nextMask);
        currentMask = nextMask;
    }
    return data;
}

int VCurveData::addPoint(const QPointF &position, qreal radius)
{
    if (curveOffsets.isEmpty()) {
        beginCurve();
    }
    positions << position;
    radii << radius;
    if (!segments.isEmpty()) segments << LineSegment;
    if (!handlesOut.isEmpty()) {
        handlesOut << position;
        handlesIn << position;
    }
    if (!flags.isEmpty()) flags << 0;
    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        // the previous point's value until set: callers set every attribute they use
        it->append(it->isEmpty() ? 0.0 : it->last());
    }
    return positions.size() - 1;
}

void VCurveData::setCurveSegment(int index, const QPointF &control1, const QPointF &control2)
{
    if (segments.isEmpty()) {
        segments.fill(LineSegment, positions.size());
    }
    if (handlesOut.isEmpty()) {
        handlesOut = positions;
        handlesIn = positions;
    }
    const int curve = curveOf(index);
    const int next = index + 1 < curveEnd(curve) ? index + 1 : curveBegin(curve);
    segments[index] = CurveSegment;
    handlesOut[index] = control1;
    handlesIn[next] = control2;
}

void VCurveData::setFlag(int index, PointFlag flag)
{
    if (flags.isEmpty()) {
        flags.fill(0, positions.size());
    }
    flags[index] |= flag;
}

void VCurveData::clearFlag(int index, PointFlag flag)
{
    if (flags.isEmpty()) return;
    flags[index] &= quint8(~flag);
    if (std::all_of(flags.begin(), flags.end(), [] (quint8 f) { return f == 0; })) flags.clear();
}

void VCurveData::setLineSegment(int index)
{
    if (segments.isEmpty()) return;
    const int next = nextIndex(index);
    segments[index] = LineSegment;
    if (!handlesOut.isEmpty()) {
        handlesOut[index] = positions[index];
        if (next >= 0) handlesIn[next] = positions[next];
    }
    // as VCurveBuilder: no arrays while every segment is a line
    if (std::find(segments.begin(), segments.end(), quint8(CurveSegment)) == segments.end()) {
        segments.clear();
        handlesOut.clear();
        handlesIn.clear();
    }
}

void VCurveData::setAttribute(const QString &name, int index, qreal value, qreal defaultValue)
{
    auto it = attributes.find(name);
    if (it == attributes.end()) {
        if (value == defaultValue) return;
        it = attributes.insert(name, QVector<qreal>(positions.size(), defaultValue));
    }
    (*it)[index] = value;
}

void VCurveData::transform(const QTransform &t)
{
    for (QPointF &pt : positions) pt = t.map(pt);
    for (QPointF &pt : handlesOut) pt = t.map(pt);
    for (QPointF &pt : handlesIn) pt = t.map(pt);
}

void VCurveData::clampRadii()
{
    for (qreal &r : radii) {
        // also catches NaN, which compares false
        if (!(r >= MinRadius)) r = MinRadius;
    }
}

QRectF VCurveData::controlBounds() const
{
    if (positions.isEmpty()) return QRectF();
    QRectF bounds(positions.first(), QSizeF());
    for (const QPointF &pt : positions) KisAlgebra2D::accumulateBounds(pt, &bounds);
    for (int i = 0; i < handlesOut.size(); i++) {
        if (segment(i) != CurveSegment) continue;
        const int curve = curveOf(i);
        const int next = i + 1 < curveEnd(curve) ? i + 1 : curveBegin(curve);
        KisAlgebra2D::accumulateBounds(handlesOut[i], &bounds);
        KisAlgebra2D::accumulateBounds(handlesIn[next], &bounds);
    }
    return bounds;
}

bool VCurveData::operator==(const VCurveData &rhs) const
{
    return curveOffsets == rhs.curveOffsets && cyclic == rhs.cyclic &&
           positions == rhs.positions && radii == rhs.radii &&
           segments == rhs.segments && handlesOut == rhs.handlesOut && handlesIn == rhs.handlesIn &&
           flags == rhs.flags && attributes == rhs.attributes && caps == rhs.caps;
}

void VCurveData::toXML(QDomDocument &doc, QDomElement &element, const QTransform &t) const
{
    VCurveData mapped(*this);
    mapped.transform(t);

    element.setAttribute("version", 1);
    element.setAttribute("points", positions.size());
    element.setAttribute("curves", curveOffsets.size());

    addArray(doc, element, "offsets", "i32", encode(QVector<qint32>(curveOffsets.begin(), curveOffsets.end())));
    addArray(doc, element, "cyclic", "u8", encode(cyclic));
    addArray(doc, element, "position", "f64x2", encode(flatten(mapped.positions)));
    addArray(doc, element, "radius", "f64", encode(QVector<double>(radii.begin(), radii.end())));
    if (!segments.isEmpty()) addArray(doc, element, "segment", "u8", encode(segments));
    if (!handlesOut.isEmpty()) {
        addArray(doc, element, "handle-out", "f64x2", encode(flatten(mapped.handlesOut)));
        addArray(doc, element, "handle-in", "f64x2", encode(flatten(mapped.handlesIn)));
    }
    if (!flags.isEmpty()) addArray(doc, element, "flags", "u8", encode(flags));
    if (!caps.isEmpty()) addArray(doc, element, "caps", "u8", encode(caps));

    QStringList names = attributes.keys();
    names.sort(); // stable files
    for (const QString &name : names) {
        const QVector<qreal> &values = attributes[name];
        addArray(doc, element, "attr:" + name, "f64", encode(QVector<double>(values.begin(), values.end())));
    }
}

bool VCurveData::fromXML(const QDomElement &element, VCurveData *data)
{
    const int points = element.attribute("points").toInt();
    const int curves = element.attribute("curves").toInt();
    if (element.attribute("version").toInt() != 1 || points <= 0 || curves <= 0) return false;

    VCurveData d;
    QVector<double> doubles;
    QVector<qint32> offsets;
    bool haveOffsets = false, havePositions = false, haveRadii = false, haveCyclic = false;

    for (QDomElement e = element.firstChildElement("krita:a"); !e.isNull(); e = e.nextSiblingElement("krita:a")) {
        const QString name = e.attribute("name");
        const QString text = e.text();
        if (name == "offsets") {
            haveOffsets = decode(text, curves, &offsets);
        } else if (name == "cyclic") {
            haveCyclic = decode(text, curves, &d.cyclic);
        } else if (name == "position") {
            havePositions = decode(text, points * 2, &doubles);
            d.positions = unflatten(doubles);
        } else if (name == "radius") {
            haveRadii = decode(text, points, &doubles);
            d.radii = QVector<qreal>(doubles.begin(), doubles.end());
        } else if (name == "segment") {
            if (!decode(text, points, &d.segments)) return false;
        } else if (name == "handle-out") {
            if (!decode(text, points * 2, &doubles)) return false;
            d.handlesOut = unflatten(doubles);
        } else if (name == "handle-in") {
            if (!decode(text, points * 2, &doubles)) return false;
            d.handlesIn = unflatten(doubles);
        } else if (name == "flags") {
            if (!decode(text, points, &d.flags)) return false;
        } else if (name == "caps") {
            if (!decode(text, curves, &d.caps)) return false;
        } else if (name.startsWith("attr:")) {
            if (!decode(text, points, &doubles)) return false;
            d.attributes.insert(name.mid(5), QVector<qreal>(doubles.begin(), doubles.end()));
        }
    }

    if (!haveOffsets || !havePositions || !haveRadii || !haveCyclic) return false;
    if (d.handlesOut.size() != d.handlesIn.size()) return false;
    d.curveOffsets = QVector<int>(offsets.begin(), offsets.end());
    for (int i = 0; i < d.curveOffsets.size(); i++) {
        const bool valid = i == 0 ? d.curveOffsets[i] == 0
                                  : d.curveOffsets[i] > d.curveOffsets[i - 1] && d.curveOffsets[i] < points;
        if (!valid) return false;
    }
    if (d.segments.contains(CurveSegment) && d.handlesOut.isEmpty()) return false;

    *data = d;
    return true;
}

VCurvePoint VCurvePoint::lerp(const VCurvePoint &a, const VCurvePoint &b, qreal t)
{
    VCurvePoint p;
    p.position = a.position + t * (b.position - a.position);
    p.handleIn = p.handleOut = p.position;
    p.radius = a.radius + t * (b.radius - a.radius);
    p.segment = a.segment;
    p.flags = t < 0.5 ? a.flags : b.flags;
    p.attributes.resize(a.attributes.size());
    for (int k = 0; k < a.attributes.size() && k < b.attributes.size(); k++) {
        p.attributes[k] = a.attributes[k] + t * (b.attributes[k] - a.attributes[k]);
    }
    return p;
}

QPointF VCurvePoint::segmentPoint(const VCurvePoint &a, const VCurvePoint &b, qreal t)
{
    if (a.segment != VCurveData::CurveSegment) return a.position + t * (b.position - a.position);
    const qreal u = 1.0 - t;
    return u * u * u * a.position + 3 * u * u * t * a.handleOut + 3 * u * t * t * b.handleIn + t * t * t * b.position;
}

VCurvePoint VCurvePoint::split(VCurvePoint &a, VCurvePoint &b, qreal t)
{
    VCurvePoint mid = lerp(a, b, t);
    // a dab belongs to its own point, never to a new one
    mid.flags = 0;
    if (a.segment == VCurveData::CurveSegment) {
        auto mix = [t] (const QPointF &p, const QPointF &q) { return p + t * (q - p); };
        const QPointF p01 = mix(a.position, a.handleOut);
        const QPointF p12 = mix(a.handleOut, b.handleIn);
        const QPointF p23 = mix(b.handleIn, b.position);
        const QPointF p012 = mix(p01, p12);
        const QPointF p123 = mix(p12, p23);
        mid.position = mix(p012, p123);
        mid.handleIn = p012;
        mid.handleOut = p123;
        mid.segment = VCurveData::CurveSegment;
        a.handleOut = p01;
        b.handleIn = p23;
    } else {
        mid.segment = VCurveData::LineSegment;
    }
    return mid;
}

VCurveBuilder::VCurveBuilder(const QStringList &attributeNames)
    : m_names(attributeNames),
      m_attributes(attributeNames.size())
{
}

void VCurveBuilder::beginCurve(bool cyclic, quint8 capFlags)
{
    m_data.curveOffsets << m_data.positions.size();
    m_data.cyclic << (cyclic ? 1 : 0);
    m_data.caps << capFlags;
    m_anyCap |= capFlags != 0;
}

void VCurveBuilder::add(const VCurvePoint &p)
{
    if (m_data.curveOffsets.isEmpty()) beginCurve();
    m_data.positions << p.position;
    m_data.radii << p.radius;
    m_data.segments << p.segment;
    m_data.handlesIn << p.handleIn;
    m_data.handlesOut << p.handleOut;
    m_data.flags << p.flags;
    m_anyCurve |= p.segment == VCurveData::CurveSegment;
    m_anyFlag |= p.flags != 0;
    for (int k = 0; k < m_names.size(); k++) {
        m_attributes[k] << (k < p.attributes.size() ? p.attributes[k] : VCurveData::attributeDefault(m_names[k]));
    }
}

void VCurveBuilder::addCurve(const VCurveData &data, int curve)
{
    beginCurve(data.isCyclic(curve), data.capFlags(curve));
    for (int i = data.curveBegin(curve); i < data.curveEnd(curve); i++) {
        add(data.point(i, m_names));
    }
}

VCurveData VCurveBuilder::take()
{
    VCurveData d = m_data;
    // a sub-path with no points would break the offsets
    for (int c = d.curveOffsets.size() - 1; c >= 0; c--) {
        const int end = c + 1 < d.curveOffsets.size() ? d.curveOffsets[c + 1] : d.positions.size();
        if (end == d.curveOffsets[c]) {
            d.curveOffsets.remove(c);
            d.cyclic.remove(c);
            d.caps.remove(c);
        }
    }
    if (!m_anyCurve) {
        d.segments.clear();
        d.handlesIn.clear();
        d.handlesOut.clear();
    } else {
        // the handles of line segments sit on their points, as addPoint() leaves them
        for (int i = 0; i < d.positions.size(); i++) {
            if (d.segments[i] != VCurveData::CurveSegment) d.handlesOut[i] = d.positions[i];
        }
        for (int i = 0; i < d.positions.size(); i++) {
            const int c = d.curveOf(i);
            const int begin = d.curveOffsets[c];
            int previous = i - 1;
            if (i == begin) previous = d.isCyclic(c) ? d.curveEnd(c) - 1 : -1;
            if (previous < 0 || d.segments[previous] != VCurveData::CurveSegment) d.handlesIn[i] = d.positions[i];
        }
    }
    if (!m_anyFlag) d.flags.clear();
    if (!m_anyCap) d.caps.clear();
    for (int k = 0; k < m_names.size(); k++) {
        const qreal def = VCurveData::attributeDefault(m_names[k]);
        bool allDefault = true;
        for (qreal v : m_attributes[k]) {
            if (v != def) {
                allDefault = false;
                break;
            }
        }
        if (!allDefault) d.attributes.insert(m_names[k], m_attributes[k]);
    }
    m_data = VCurveData();
    m_attributes = QVector<QVector<qreal>>(m_names.size());
    m_anyCurve = m_anyFlag = m_anyCap = false;
    return d;
}
