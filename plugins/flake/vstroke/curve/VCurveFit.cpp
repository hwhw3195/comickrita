/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VCurveFit.h"

#include <QLineF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {

qreal dot(const QPointF &a, const QPointF &b)
{
    return a.x() * b.x() + a.y() * b.y();
}

qreal length(const QPointF &v)
{
    return std::sqrt(dot(v, v));
}

QPointF normalized(const QPointF &v)
{
    const qreal l = length(v);
    return l > 0 ? v / l : QPointF();
}

/// union-find with path halving
struct Components {
    QVector<int> parent;
    explicit Components(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        parent[a] = b;
        return true;
    }
};

struct Edge {
    int a, b;
    qreal length;
};

QPointF bezier(const VCurveFit::Cubic &c, qreal t)
{
    const qreal u = 1 - t;
    return u * u * u * c.p0 + 3 * u * u * t * c.c1 + 3 * u * t * t * c.c2 + t * t * t * c.p1;
}

QPointF bezierDerivative(const VCurveFit::Cubic &c, qreal t)
{
    const qreal u = 1 - t;
    return 3 * u * u * (c.c1 - c.p0) + 6 * u * t * (c.c2 - c.c1) + 3 * t * t * (c.p1 - c.c2);
}

QPointF bezierSecondDerivative(const VCurveFit::Cubic &c, qreal t)
{
    return 6 * (1 - t) * (c.c2 - 2 * c.c1 + c.p0) + 6 * t * (c.p1 - 2 * c.c2 + c.c1);
}

/// least squares handle lengths along the given tangents (Schneider's GenerateBezier)
VCurveFit::Cubic generate(const QVector<QPointF> &pts, int first, int last, const QVector<qreal> &u,
                          const QPointF &tangent1, const QPointF &tangent2)
{
    const QPointF p0 = pts[first];
    const QPointF p3 = pts[last];
    qreal c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
    for (int i = 0; i <= last - first; i++) {
        const qreal t = u[i];
        const qreal s = 1 - t;
        const qreal b0 = s * s * s, b1 = 3 * s * s * t, b2 = 3 * s * t * t, b3 = t * t * t;
        const QPointF a1 = tangent1 * b1;
        const QPointF a2 = tangent2 * b2;
        c00 += dot(a1, a1);
        c01 += dot(a1, a2);
        c11 += dot(a2, a2);
        const QPointF rest = pts[first + i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
        x0 += dot(a1, rest);
        x1 += dot(a2, rest);
    }
    const qreal det = c00 * c11 - c01 * c01;
    qreal alpha1 = 0, alpha2 = 0;
    if (std::abs(det) > 1e-12) {
        alpha1 = (x0 * c11 - x1 * c01) / det;
        alpha2 = (c00 * x1 - c01 * x0) / det;
    }
    const qreal segLength = length(p3 - p0);
    const qreal epsilon = 1e-6 * segLength;
    if (alpha1 < epsilon || alpha2 < epsilon) {
        // Wu/Barsky heuristic, as in the paper
        alpha1 = alpha2 = segLength / 3.0;
    }
    return {p0, p0 + tangent1 * alpha1, p3 + tangent2 * alpha2, p3};
}

qreal maxError(const QVector<QPointF> &pts, int first, int last, const VCurveFit::Cubic &c,
               const QVector<qreal> &u, int *splitPoint)
{
    qreal worst = 0;
    *splitPoint = (first + last) / 2;
    for (int i = first + 1; i < last; i++) {
        const qreal d = length(bezier(c, u[i - first]) - pts[i]);
        if (d > worst) {
            worst = d;
            *splitPoint = i;
        }
    }
    return worst;
}

void fitRange(const QVector<QPointF> &pts, int first, int last, const QPointF &tangent1, const QPointF &tangent2,
              qreal error, VCurveFit::FitResult &result)
{
    const int n = last - first + 1;
    if (n == 2) {
        const qreal d = length(pts[last] - pts[first]) / 3.0;
        result.cubics << VCurveFit::Cubic{pts[first], pts[first] + tangent1 * d, pts[last] + tangent2 * d, pts[last]};
        result.knots << last;
        return;
    }

    // chord length parameters
    QVector<qreal> u(n, 0.0);
    for (int i = 1; i < n; i++) u[i] = u[i - 1] + length(pts[first + i] - pts[first + i - 1]);
    if (u.last() <= 0) {
        result.cubics << VCurveFit::Cubic{pts[first], pts[first], pts[last], pts[last]};
        result.knots << last;
        return;
    }
    for (int i = 1; i < n; i++) u[i] /= u.last();

    VCurveFit::Cubic cubic = generate(pts, first, last, u, tangent1, tangent2);
    int split = 0;
    qreal err = maxError(pts, first, last, cubic, u, &split);
    if (err < error) {
        result.cubics << cubic;
        result.knots << last;
        return;
    }
    if (err < error * 4) {
        // Newton-Raphson reparameterization
        for (int iteration = 0; iteration < 4; iteration++) {
            for (int i = 0; i < n; i++) {
                const QPointF d = bezier(cubic, u[i]) - pts[first + i];
                const QPointF d1 = bezierDerivative(cubic, u[i]);
                const QPointF d2 = bezierSecondDerivative(cubic, u[i]);
                const qreal denominator = dot(d1, d1) + dot(d, d2);
                if (std::abs(denominator) > 1e-12) u[i] = qBound(0.0, u[i] - dot(d, d1) / denominator, 1.0);
            }
            cubic = generate(pts, first, last, u, tangent1, tangent2);
            err = maxError(pts, first, last, cubic, u, &split);
            if (err < error) {
                result.cubics << cubic;
                result.knots << last;
                return;
            }
        }
    }

    split = qBound(first + 1, split, last - 1);
    QPointF center = normalized(pts[split - 1] - pts[split + 1]);
    if (center.isNull()) center = normalized(QPointF(-(pts[split] - pts[split - 1]).y(), (pts[split] - pts[split - 1]).x()));
    fitRange(pts, first, split, tangent1, center, error, result);
    fitRange(pts, split, last, -center, tangent2, error, result);
}

}

VCurveFit::PointGrid::PointGrid(const QVector<QPointF> &points, qreal cellSize)
    : m_points(points),
      m_cellSize(cellSize > 0 ? cellSize : 1.0)
{
    for (int i = 0; i < points.size(); i++) {
        m_cells[key(cellOf(points[i]))] << i;
    }
}

QPoint VCurveFit::PointGrid::cellOf(const QPointF &p) const
{
    return QPoint(int(std::floor(p.x() / m_cellSize)), int(std::floor(p.y() / m_cellSize)));
}

quint64 VCurveFit::PointGrid::key(const QPoint &cell)
{
    return (quint64(quint32(cell.x())) << 32) | quint32(cell.y());
}

QVector<int> VCurveFit::PointGrid::near(const QPointF &center, qreal radius) const
{
    QVector<int> result;
    const QPoint lo = cellOf(center - QPointF(radius, radius));
    const QPoint hi = cellOf(center + QPointF(radius, radius));
    const qreal radius2 = radius * radius;
    for (int x = lo.x(); x <= hi.x(); x++) {
        for (int y = lo.y(); y <= hi.y(); y++) {
            auto it = m_cells.constFind(key(QPoint(x, y)));
            if (it == m_cells.constEnd()) continue;
            for (int i : *it) {
                const QPointF v = m_points[i] - center;
                if (dot(v, v) <= radius2) result << i;
            }
        }
    }
    return result;
}

QVector<int> VCurveFit::PointGrid::nearest(int index, int k) const
{
    // grow the search ring until it holds k points, then one more ring for exactness
    const QPointF center = m_points[index];
    QVector<int> found;
    for (int ring = 1; ring < 64; ring *= 2) {
        found = near(center, ring * m_cellSize);
        if (found.size() > k || found.size() == m_points.size()) {
            found = near(center, (ring + 1) * m_cellSize);
            break;
        }
    }
    found.removeAll(index);
    std::sort(found.begin(), found.end(), [&] (int a, int b) {
        return dot(m_points[a] - center, m_points[a] - center) < dot(m_points[b] - center, m_points[b] - center);
    });
    if (found.size() > k) found.resize(k);
    return found;
}

QVector<QVector<int>> VCurveFit::spanningTree(const QVector<QPointF> &points, int k)
{
    const int n = points.size();
    QVector<QVector<int>> tree(n);
    if (n < 2) return tree;

    // cells holding a few points each on average
    QRectF bounds(points.first(), QSizeF());
    for (const QPointF &p : points) bounds |= QRectF(p, QSizeF(1e-9, 1e-9));
    const qreal cell = qMax(1e-6, std::sqrt(bounds.width() * bounds.height() / n) * 2);
    const PointGrid grid(points, cell);

    QVector<Edge> edges;
    edges.reserve(n * k);
    for (int i = 0; i < n; i++) {
        for (int j : grid.nearest(i, k)) {
            // pairs found from both ends come twice, which Kruskal skips
            edges << Edge{i, j, length(points[i] - points[j])};
        }
    }
    std::sort(edges.begin(), edges.end(), [] (const Edge &a, const Edge &b) { return a.length < b.length; });

    Components components(n);
    int joined = 0;
    auto take = [&] (const QVector<Edge> &list) {
        for (const Edge &e : list) {
            if (components.unite(e.a, e.b)) {
                tree[e.a] << e.b;
                tree[e.b] << e.a;
                joined++;
            }
        }
    };
    take(edges);

    if (joined < n - 1) {
        // link what the neighbour graph left apart by the nearest pairs
        QHash<int, QVector<int>> parts;
        for (int i = 0; i < n; i++) parts[components.find(i)] << i;
        const QList<int> roots = parts.keys();
        QVector<Edge> links;
        for (int a = 0; a < roots.size(); a++) {
            for (int b = a + 1; b < roots.size(); b++) {
                Edge best{-1, -1, std::numeric_limits<qreal>::max()};
                for (int i : parts[roots[a]]) {
                    for (int j : parts[roots[b]]) {
                        const qreal d = length(points[i] - points[j]);
                        if (d < best.length) best = Edge{i, j, d};
                    }
                }
                links << best;
            }
        }
        std::sort(links.begin(), links.end(), [] (const Edge &a, const Edge &b) { return a.length < b.length; });
        take(links);
    }
    return tree;
}

QVector<int> VCurveFit::longestPath(const QVector<QVector<int>> &tree, const QVector<QPointF> &points)
{
    const int n = tree.size();
    if (n == 0) return QVector<int>();

    // the farthest point from a start, through the tree; two passes give a diameter
    auto farthest = [&] (int start, QVector<int> *parent) {
        QVector<qreal> distance(n, -1.0);
        parent->fill(-1, n);
        QVector<int> stack{start};
        distance[start] = 0;
        int best = start;
        while (!stack.isEmpty()) {
            const int i = stack.takeLast();
            if (distance[i] > distance[best]) best = i;
            for (int j : tree[i]) {
                if (distance[j] >= 0) continue;
                distance[j] = distance[i] + length(points[i] - points[j]);
                (*parent)[j] = i;
                stack << j;
            }
        }
        return best;
    };

    QVector<int> parent;
    const int a = farthest(0, &parent);
    const int b = farthest(a, &parent);
    QVector<int> path;
    for (int i = b; i >= 0; i = parent[i]) path << i;
    return path;
}

VCurveFit::FitResult VCurveFit::fitCubics(const QVector<QPointF> &input, qreal error)
{
    FitResult result;
    // repeated points have no tangent
    QVector<QPointF> pts;
    QVector<int> source;
    for (int i = 0; i < input.size(); i++) {
        if (!pts.isEmpty() && length(input[i] - pts.last()) < 1e-9) continue;
        pts << input[i];
        source << i;
    }
    if (pts.isEmpty()) return result;
    result.knots << 0;
    if (pts.size() == 1) return result;

    const QPointF tangent1 = normalized(pts[1] - pts[0]);
    const QPointF tangent2 = normalized(pts[pts.size() - 2] - pts.last());
    fitRange(pts, 0, pts.size() - 1, tangent1, tangent2, qMax(error, 1e-6), result);
    for (int &knot : result.knots) knot = source[knot];
    return result;
}
