/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_CSP_BRUSH_OPTION_PAGE_H
#define KIS_CSP_BRUSH_OPTION_PAGE_H

#include <functional>

#include <QStringList>
#include <QVector>
#include <kis_paintop_option.h>
#include <lager/cursor.hpp>

#include "CspBrushData.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class KisPropertiesConfiguration;
class KisAngleSelector;
class KisDoubleSliderSpinBox;
class KisSliderSpinBox;

/**
 * One category page of the CSP Brush editor (CSP's sub tool detail
 * categories: Brush size, Ink, Brush tip, ...).
 *
 * All pages share one CspBrushData, the whole CSP/ setting map, so every
 * value of a converted preset survives editing even without a control.
 * Only the owner page reads and writes the preset; the others edit the
 * shared data. Controls are bound to a setting key (without "CSP/") and
 * refresh whenever the data changes, so pages stay in sync.
 */
class KRITACSPBRUSHPAINTOP_EXPORT KisCspBrushOptionPage : public KisPaintOpOption
{
    Q_OBJECT
public:
    /// @p checkKey: a CSP on/off setting shown as the page's checkbox (empty: always on)
    KisCspBrushOptionPage(const QString &label, PaintopCategory category, lager::cursor<CspBrushData> data,
                          const QString &checkKey = QString(), bool owner = false);
    ~KisCspBrushOptionPage() override;

    void writeOptionSetting(KisPropertiesConfigurationSP setting) const override;
    void readOptionSetting(const KisPropertiesConfigurationSP setting) override;

    const CspBrushData &data() const;
    /// A user edit: changes the shared data and tells Krita the preset changed
    void edit(const std::function<void(CspBrushData &)> &change);
    void set(const QString &key, const QVariant &value);

    QWidget *pageWidget() const;
    QFormLayout *form() const;

    // Controls bound to a setting. @p effectorKey ("BrushSizeEffector", ...) adds the button of CSP's
    // effect source popup next to it.
    KisSliderSpinBox *addInt(const QString &key, const QString &label, int min, int max, int defaultValue,
                             const QString &suffix = QString(), const QString &effectorKey = QString());
    KisDoubleSliderSpinBox *addReal(const QString &key, const QString &label, qreal min, qreal max, int decimals,
                                    qreal defaultValue, const QString &suffix = QString(),
                                    const QString &effectorKey = QString(), qreal exponent = 1.0);
    /// A length in px; CSP may store it in physical units (key + "Unit"), edits store px
    KisDoubleSliderSpinBox *addLength(const QString &key, const QString &label, qreal min, qreal max,
                                      qreal defaultValue, const QString &effectorKey = QString());
    QCheckBox *addCheck(const QString &key, const QString &text, bool defaultValue = false);
    /// A checkbox for one bit of an integer setting
    QCheckBox *addBit(const QString &key, const QString &text, int bit);
    /// A choice among the bits of @p mask of an integer setting: sets the chosen value's bits, keeps the bits
    /// outside @p mask (a value with several bits of @p mask shows as the first item that matches)
    QComboBox *addComboBits(const QString &key, const QString &label, const QStringList &items, int mask,
                            const QVector<int> &values);
    /// Items are stored as their index unless @p values is given
    QComboBox *addCombo(const QString &key, const QString &label, const QStringList &items, int defaultValue = 0,
                        const QVector<int> &values = QVector<int>());
    KisAngleSelector *addAngle(const QString &key, const QString &label);
    void addSection(const QString &title);
    /// Following controls go into a section that folds under a header button, until endFoldSection()
    void beginFoldSection(const QString &title, bool expanded = false);
    void endFoldSection();
    void addNote(const QString &text);

    /// Calls @p refresh with the data now and on every change (signals of the page's widgets are blocked)
    void onUpdate(const std::function<void(const CspBrushData &)> &refresh);

    /// Settings kept outside the CSP data (Krita's Texture/Pattern/...): @p read gets the preset when it
    /// loads, @p write the configuration being saved
    void addExternalSettings(const std::function<void(const KisPropertiesConfiguration *)> &read,
                             const std::function<void(KisPropertiesConfiguration *)> &write);
    /// A user edit of external settings: tells Krita the preset changed
    void notifyExternalChange();

private:
    /// @p control with the effector button of @p effectorKey after it (just @p control without a key)
    QWidget *withEffector(QWidget *control, const QString &label, const QString &effectorKey);
    void refreshAll(const CspBrushData &data);

    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif // KIS_CSP_BRUSH_OPTION_PAGE_H
