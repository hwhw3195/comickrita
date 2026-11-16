/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VCURVE_DATA_H
#define VCURVE_DATA_H

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSharedData>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "kritavstroke_export.h"

class QDomDocument;
class QDomElement;
class QTransform;
class VCurveData;

/**
 * One point of a VCurveData with everything stored for it, for algorithms
 * that build new curves out of old points (VCurveBuilder).
 */
struct KRITAVSTROKE_EXPORT VCurvePoint
{
    QPointF position;
    QPointF handleIn;      ///< control point of the segment arriving here; the position for lines
    QPointF handleOut;     ///< control point of the segment leaving here; the position for lines
    qreal radius = 0.0;
    quint8 segment = 0;    ///< VCurveData::Segment leaving the point
    quint8 flags = 0;
    QVector<qreal> attributes; ///< in the order of the builder's names

    /// \p a and \p b mixed linearly by \p t: position, radius and attributes; flags from the nearer one
    static VCurvePoint lerp(const VCurvePoint &a, const VCurvePoint &b, qreal t);

    /**
     * Splits the segment from \p a to \p b at \p t: a bezier with de
     * Casteljau, so the two halves trace the same curve, a line linearly.
     * Updates a.handleOut and b.handleIn and returns the new point.
     */
    static VCurvePoint split(VCurvePoint &a, VCurvePoint &b, qreal t);

    /// the position on the segment from \p a to \p b at \p t
    static QPointF segmentPoint(const VCurvePoint &a, const VCurvePoint &b, qreal t);
};

/**
 * The curves of one object on a freehand vector layer, stored as flat
 * arrays (struct of arrays, as Blender Grease Pencil stores its strokes).
 *
 * The points of all sub-paths are in one array; curveOffsets says where each
 * sub-path starts. Every point has a position and a radius (the half-width
 * of the line there). Other per-point values are named attributes; a missing
 * attribute means every point has its default.
 *
 * The segment leaving point i (to i + 1, or from the last point back to the
 * first of a cyclic sub-path) is a line, or a cubic bezier whose control
 * points are handlesOut[i] and handlesIn[i + 1]. The handle arrays are empty
 * while every segment is a line.
 *
 * The units of positions and radii are up to the shape holding the data.
 */
class KRITAVSTROKE_EXPORT VCurveData : public QSharedData
{
public:
    enum Segment : quint8 {
        LineSegment = 0,
        CurveSegment = 1
    };

    enum PointFlag : quint8 {
        /// brush strokes: a single dab at the point, after the segment arriving at it
        Dab = 1,
        /// Edit Shapes: the two handles stay on one line through the point (as KoPathPoint::IsSmooth)
        SmoothPoint = 2,
        /// Edit Shapes: the two handles stay on one line and equally long (as KoPathPoint::IsSymmetric)
        SymmetricPoint = 4
    };

    /// per sub-path: ends that a trim cut, drawn flat instead of with the shape's cap
    enum CapFlag : quint8 {
        StartFlat = 1,
        EndFlat = 2
    };

    QVector<int> curveOffsets;
    QVector<quint8> cyclic;           ///< per sub-path

    QVector<QPointF> positions;
    QVector<qreal> radii;
    QVector<quint8> segments;         ///< per point, the segment leaving it; empty: all lines
    QVector<QPointF> handlesOut;      ///< per point; empty: no curve segments
    QVector<QPointF> handlesIn;       ///< per point; empty: no curve segments
    QVector<quint8> flags;            ///< per point; empty: no flags
    QHash<QString, QVector<qreal>> attributes;
    QVector<quint8> caps;             ///< per sub-path, CapFlag; empty: every end has the shape's cap

    VCurveData() = default;
    VCurveData(const VCurveData &rhs) = default;
    /// copies the arrays (QSharedData itself has no assignment)
    VCurveData &operator=(const VCurveData &rhs);

    int curveCount() const { return curveOffsets.size(); }
    int pointCount() const { return positions.size(); }
    bool isEmpty() const { return positions.isEmpty(); }

    int curveBegin(int curve) const { return curveOffsets[curve]; }
    /// one past the last point of \p curve
    int curveEnd(int curve) const;
    bool isCyclic(int curve) const { return cyclic[curve]; }
    /// the curve point \p index belongs to
    int curveOf(int index) const;

    Segment segment(int index) const;
    bool hasCurveSegments() const;
    bool hasFlag(int index, PointFlag flag) const;
    qreal attribute(const QString &name, int index, qreal defaultValue) const;
    quint8 capFlags(int curve) const { return caps.isEmpty() ? 0 : caps[curve]; }
    /// the index after \p index on its sub-path, wrapping on cyclic ones; -1 past the end of an open one
    int nextIndex(int index) const;
    /// the index before \p index on its sub-path, wrapping on cyclic ones; -1 before the start of an open one
    int previousIndex(int index) const;

    /// the names of the attributes, sorted
    QStringList attributeNames() const;
    /// the value a missing attribute has (that of KisPaintInformation)
    static qreal attributeDefault(const QString &name);
    /// point \p index, its attributes in the order of \p names
    VCurvePoint point(int index, const QStringList &names) const;

    /// starts a sub-path; points added next belong to it
    void beginCurve(bool isCyclic = false);
    /// appends a point to the last sub-path and returns its index
    int addPoint(const QPointF &position, qreal radius);
    /// makes the segment leaving \p index a bezier
    void setCurveSegment(int index, const QPointF &control1, const QPointF &control2);
    void setFlag(int index, PointFlag flag);
    void clearFlag(int index, PointFlag flag);
    /// makes the segment leaving \p index a line; its handles go back to the positions
    void setLineSegment(int index);
    /// sets a per-point value; the array is created with \p defaultValue on first use
    void setAttribute(const QString &name, int index, qreal value, qreal defaultValue);
    void setCapFlags(int curve, quint8 flags);

    /// sub-path \p curve on its own
    VCurveData curve(int curve) const;
    /// appends the sub-paths of \p other (in the same units)
    void append(const VCurveData &other);

    /**
     * Splits each segment longer than \p maxSpacing (measured between
     * positions mapped by \p toDocument) into equal parts; beziers stay
     * exact. With \p mask (one per point, for the segment leaving it), only
     * the segments whose flag is set.
     */
    VCurveData subdivided(qreal maxSpacing, const QTransform &toDocument, const QVector<bool> &mask = QVector<bool>(),
                          bool smooth = false) const;

    /// the most steps subdividedSteps() takes: each one doubles the segments
    static constexpr int MaxSubdivideSteps = 5;
    /**
     * Splits each segment in half, \p steps times (at most
     * MaxSubdivideSteps); with \p mask, only the flagged segments and the
     * pieces they become.
     *
     * With \p smooth, in both, the new points of line segments go on the
     * Catmull-Rom spline through the points around them (the four-point
     * scheme), so the line rounds out as it gains points; the old points
     * stay where they are. Bezier segments are split exactly either way.
     */
    VCurveData subdividedSteps(int steps, const QVector<bool> &mask = QVector<bool>(), bool smooth = false) const;

    /**
     * The core of both: splits each flagged segment into \p parts equal
     * parts. \p resultMask gets the flags of the new points' segments.
     */
    VCurveData subdividedBy(const std::function<int(const VCurvePoint &, const VCurvePoint &)> &parts, bool smooth,
                            const QVector<bool> &mask, QVector<bool> *resultMask = nullptr) const;

    /// maps positions and handles (not radii)
    void transform(const QTransform &t);

    /**
     * The smallest radius a shape keeps (in its radius units: image pixels
     * for brush strokes): no tool or transform can make a stroke thinner,
     * so there are no zero-width strokes. Nothing hides a stroke by its
     * radius ("no line" is a flag), and a pressure-0 tip renders the same.
     */
    static constexpr qreal MinRadius = 0.001;
    /// raises every radius below MinRadius to it
    void clampRadii();
    /// the bounds of positions and handles
    QRectF controlBounds() const;

    /// exact equality of all arrays
    bool operator==(const VCurveData &rhs) const;
    bool operator!=(const VCurveData &rhs) const { return !(*this == rhs); }

    /**
     * Writes the arrays as children of \p element: each one base64 of its
     * bytes in little endian, so values read back bit-exact. Positions and
     * handles are mapped by \p t first.
     */
    void toXML(QDomDocument &doc, QDomElement &element, const QTransform &t) const;
    /// false when the element holds no valid curves
    static bool fromXML(const QDomElement &element, VCurveData *data);

    static const QString XmlElement;
};

/**
 * Builds a VCurveData point by point from VCurvePoints. The arrays that
 * stay at their defaults (segments, handles, flags, caps) are left empty.
 */
class KRITAVSTROKE_EXPORT VCurveBuilder
{
public:
    explicit VCurveBuilder(const QStringList &attributeNames);

    void beginCurve(bool cyclic = false, quint8 capFlags = 0);
    void add(const VCurvePoint &point);
    /// appends sub-path \p curve of \p data
    void addCurve(const VCurveData &data, int curve);
    VCurveData take();

    const QStringList &names() const { return m_names; }

private:
    QStringList m_names;
    VCurveData m_data;
    QVector<QVector<qreal>> m_attributes;
    bool m_anyCurve = false;
    bool m_anyFlag = false;
    bool m_anyCap = false;
};

/**
 * One open or closed sub-path taken out of a shape for edits that work
 * across shapes (joining, merging, trimming): positions in document
 * coordinates, radii in document units.
 */
struct KRITAVSTROKE_EXPORT VDocStroke
{
    VCurveData curve;     ///< one sub-path
    int owner = -1;       ///< the shape it came from or goes to, for the caller
    bool changed = false; ///< edited: the owner needs its curves rewritten
    int source = -1;      ///< the sub-path of the owner it is, while unchanged
};

#endif // VCURVE_DATA_H
