/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPERSTROKERANDOMSOURCE_H
#define KISPERSTROKERANDOMSOURCE_H

#include <QScopedPointer>
#include "kis_shared.h"
#include "kis_shared_ptr.h"

#include "kritaimage_export.h"


class KRITAIMAGE_EXPORT KisPerStrokeRandomSource : public KisShared
{
public:
    KisPerStrokeRandomSource();
    KisPerStrokeRandomSource(const KisPerStrokeRandomSource &rhs);

    /**
     * Creates a source with a fixed \p seed. Two sources with the same seed
     * generate the same value for the same key, which lets a stroke be
     * re-rendered exactly.
     */
    explicit KisPerStrokeRandomSource(int seed);

    ~KisPerStrokeRandomSource();

    /**
     * Generates a random number in a range from \p min to \p max
     */
    int generate(const QString &key, int min, int max) const;

    /**
     * Generates a random number in a closed range [0; 1.0]
     */
    qreal generateNormalized(const QString &key) const;

    /**
     * The seed this source was created with
     */
    int seed() const;

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

class KisPerStrokeRandomSource;
typedef KisSharedPtr<KisPerStrokeRandomSource> KisPerStrokeRandomSourceSP;
typedef KisWeakSharedPtr<KisPerStrokeRandomSource> KisPerStrokeRandomSourceWSP;

#endif // KISPERSTROKERANDOMSOURCE_H
