/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCspBrushOptionPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <vector>

#include <KisAngleSelector.h>
#include <klocalizedstring.h>
#include <kis_slider_spin_box.h>
#include <lager/lenses.hpp>
#include <lager/state.hpp>

#include "CspEffectorEditor.h"

namespace {
lager::cursor<bool> checkCursor(lager::cursor<CspBrushData> data, const QString &key)
{
    return data.zoom(lager::lenses::getset(
        [key](const CspBrushData &d) { return d.boolean(key); },
        [key](CspBrushData d, bool on) {
            d.set(key, on ? 1 : 0);
            return d;
        }));
}

qreal lengthPx(const CspBrushData &d, const QString &key, qreal defaultValue)
{
    const qreal raw = d.real(key, defaultValue);
    return d.integer(key + "Unit", 0) ? raw * 100.0 : raw;
}
}

struct KisCspBrushOptionPage::Private
{
    Private(lager::cursor<CspBrushData> d) : data(d) {}

    lager::cursor<CspBrushData> data;
    bool owner {false};
    QWidget *page {nullptr};
    QFormLayout *form {nullptr};     ///< where controls go: the page's, or a fold section's
    QFormLayout *pageForm {nullptr};
    QVector<std::function<void(const CspBrushData &)>> refreshers;
    bool refreshing {false};
    QVector<std::function<void(const KisPropertiesConfiguration *)>> externalReads;
    QVector<std::function<void(KisPropertiesConfiguration *)>> externalWrites;
};

KisCspBrushOptionPage::KisCspBrushOptionPage(const QString &label, PaintopCategory category,
                                             lager::cursor<CspBrushData> data, const QString &checkKey, bool owner)
    : KisPaintOpOption(label, category, checkKey.isEmpty() ? lager::cursor<bool>(lager::make_state(true, lager::automatic_tag{}))
                                                           : checkCursor(data, checkKey))
    , m_d(new Private(data))
{
    setObjectName(QStringLiteral("KisCspBrushOptionPage ") + label);
    m_checkable = !checkKey.isEmpty();
    m_d->owner = owner;

    QScrollArea *scroll = new QScrollArea();
    scroll->setObjectName(QStringLiteral("CspPage ") + label);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_d->page = new QWidget();
    m_d->form = new QFormLayout(m_d->page);
    m_d->form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_d->pageForm = m_d->form;
    scroll->setWidget(m_d->page);

    m_d->data.bind([this](const CspBrushData &d) { refreshAll(d); });
    setConfigurationPage(scroll);
}

KisCspBrushOptionPage::~KisCspBrushOptionPage()
{
}

void KisCspBrushOptionPage::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    if (m_d->owner) m_d->data->write(setting.data());
    for (const auto &write : m_d->externalWrites) write(setting.data());
}

void KisCspBrushOptionPage::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    for (const auto &read : m_d->externalReads) read(setting.data());
    if (!m_d->owner) return;
    CspBrushData data;
    data.read(setting.data());
    m_d->data.set(data);
}

const CspBrushData &KisCspBrushOptionPage::data() const
{
    return *m_d->data;
}

void KisCspBrushOptionPage::edit(const std::function<void(CspBrushData &)> &change)
{
    if (m_d->refreshing) return;
    m_d->data.update([&](CspBrushData d) {
        change(d);
        return d;
    });
    emitSettingChanged();
}

void KisCspBrushOptionPage::set(const QString &key, const QVariant &value)
{
    edit([&](CspBrushData &d) { d.set(key, value); });
}

QWidget *KisCspBrushOptionPage::pageWidget() const
{
    return m_d->page;
}

QFormLayout *KisCspBrushOptionPage::form() const
{
    return m_d->form;
}

void KisCspBrushOptionPage::onUpdate(const std::function<void(const CspBrushData &)> &refresh)
{
    m_d->refreshers << refresh;
    m_d->refreshing = true;
    refresh(*m_d->data);
    m_d->refreshing = false;
}

void KisCspBrushOptionPage::addExternalSettings(const std::function<void(const KisPropertiesConfiguration *)> &read,
                                                const std::function<void(KisPropertiesConfiguration *)> &write)
{
    m_d->externalReads << read;
    m_d->externalWrites << write;
}

void KisCspBrushOptionPage::notifyExternalChange()
{
    emitSettingChanged();
}

void KisCspBrushOptionPage::refreshAll(const CspBrushData &data)
{
    // block every control of the page: refreshing must not look like an edit
    const QList<QWidget *> widgets = m_d->page->findChildren<QWidget *>();
    std::vector<QSignalBlocker> blockers;
    blockers.reserve(widgets.size());
    for (QWidget *w : widgets) blockers.emplace_back(w);

    m_d->refreshing = true;
    for (const auto &refresh : m_d->refreshers) refresh(data);
    m_d->refreshing = false;
}

QWidget *KisCspBrushOptionPage::withEffector(QWidget *control, const QString &label, const QString &effectorKey)
{
    if (effectorKey.isEmpty()) return control;

    QWidget *row = new QWidget(m_d->page);
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    control->setParent(row);
    layout->addWidget(control, 1);
    CspEffectorButton *button = new CspEffectorButton(QString(label).remove(':'), row);
    button->setObjectName(effectorKey);
    // hue, saturation and value jitter shift both ways: random down to -100%; everything else 0..100% as in CSP
    static const QStringList signedRandom = {QStringLiteral("BrushHueChangeEffector"),
                                             QStringLiteral("BrushSaturationChangeEffector"),
                                             QStringLiteral("BrushValueChangeEffector")};
    button->setSignedRandom(signedRandom.contains(effectorKey));
    layout->addWidget(button);

    connect(button, &CspEffectorButton::effectorChanged, this, [this, effectorKey](const QString &text) {
        set(effectorKey, text);
    });
    onUpdate([button, effectorKey](const CspBrushData &d) { button->setEffector(d.string(effectorKey)); });
    return row;
}

KisSliderSpinBox *KisCspBrushOptionPage::addInt(const QString &key, const QString &label, int min, int max,
                                                int defaultValue, const QString &suffix, const QString &effectorKey)
{
    KisSliderSpinBox *slider = new KisSliderSpinBox(m_d->page);
    slider->setObjectName(key);
    slider->setRange(min, max);
    slider->setSuffix(suffix);
    m_d->form->addRow(label, withEffector(slider, label, effectorKey));

    connect(slider, qOverload<int>(&KisSliderSpinBox::valueChanged), this, [this, key](int v) { set(key, v); });
    onUpdate([slider, key, defaultValue](const CspBrushData &d) { slider->setValue(d.integer(key, defaultValue)); });
    return slider;
}

KisDoubleSliderSpinBox *KisCspBrushOptionPage::addReal(const QString &key, const QString &label, qreal min,
                                                       qreal max, int decimals, qreal defaultValue,
                                                       const QString &suffix, const QString &effectorKey,
                                                       qreal exponent)
{
    KisDoubleSliderSpinBox *slider = new KisDoubleSliderSpinBox(m_d->page);
    slider->setObjectName(key);
    slider->setRange(min, max, decimals);
    slider->setExponentRatio(exponent);
    slider->setSuffix(suffix);
    m_d->form->addRow(label, withEffector(slider, label, effectorKey));

    connect(slider, qOverload<qreal>(&KisDoubleSliderSpinBox::valueChanged), this, [this, key](qreal v) { set(key, v); });
    onUpdate([slider, key, defaultValue](const CspBrushData &d) { slider->setValue(d.real(key, defaultValue)); });
    return slider;
}

KisDoubleSliderSpinBox *KisCspBrushOptionPage::addLength(const QString &key, const QString &label, qreal min,
                                                         qreal max, qreal defaultValue, const QString &effectorKey)
{
    KisDoubleSliderSpinBox *slider = new KisDoubleSliderSpinBox(m_d->page);
    slider->setObjectName(key);
    slider->setRange(min, max, 2);
    slider->setExponentRatio(3.0);
    slider->setSuffix(i18n(" px"));
    m_d->form->addRow(label, withEffector(slider, label, effectorKey));

    connect(slider, qOverload<qreal>(&KisDoubleSliderSpinBox::valueChanged), this, [this, key](qreal v) {
        edit([&](CspBrushData &d) {
            d.set(key, v);
            d.set(key + "Unit", 0);
        });
    });
    onUpdate([slider, key, defaultValue](const CspBrushData &d) { slider->setValue(lengthPx(d, key, defaultValue)); });
    return slider;
}

QCheckBox *KisCspBrushOptionPage::addCheck(const QString &key, const QString &text, bool defaultValue)
{
    QCheckBox *check = new QCheckBox(text, m_d->page);
    check->setObjectName(key);
    m_d->form->addRow(QString(), check);
    connect(check, &QCheckBox::toggled, this, [this, key](bool on) { set(key, on ? 1 : 0); });
    onUpdate([check, key, defaultValue](const CspBrushData &d) { check->setChecked(d.boolean(key, defaultValue)); });
    return check;
}

QCheckBox *KisCspBrushOptionPage::addBit(const QString &key, const QString &text, int bit)
{
    QCheckBox *check = new QCheckBox(text, m_d->page);
    check->setObjectName(QString("%1#%2").arg(key).arg(bit));
    m_d->form->addRow(QString(), check);
    connect(check, &QCheckBox::toggled, this, [this, key, bit](bool on) {
        edit([&](CspBrushData &d) {
            const int value = d.integer(key);
            d.set(key, on ? (value | bit) : (value & ~bit));
        });
    });
    onUpdate([check, key, bit](const CspBrushData &d) { check->setChecked(d.integer(key) & bit); });
    return check;
}

QComboBox *KisCspBrushOptionPage::addCombo(const QString &key, const QString &label, const QStringList &items,
                                           int defaultValue, const QVector<int> &values)
{
    QComboBox *combo = new QComboBox(m_d->page);
    combo->setObjectName(key);
    combo->addItems(items);
    m_d->form->addRow(label, combo);

    auto valueOf = [values](int index) { return values.isEmpty() ? index : values.value(index); };
    auto indexOf = [values](int value) { return values.isEmpty() ? value : values.indexOf(value); };
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, key, valueOf](int index) {
        set(key, valueOf(index));
    });
    onUpdate([combo, key, defaultValue, indexOf](const CspBrushData &d) {
        combo->setCurrentIndex(indexOf(d.integer(key, defaultValue)));
    });
    return combo;
}

QComboBox *KisCspBrushOptionPage::addComboBits(const QString &key, const QString &label, const QStringList &items,
                                               int mask, const QVector<int> &values)
{
    QComboBox *combo = new QComboBox(m_d->page);
    combo->setObjectName(key + "#combo");
    combo->addItems(items);
    m_d->form->addRow(label, combo);

    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, key, mask, values](int index) {
        edit([&](CspBrushData &d) { d.set(key, (d.integer(key) & ~mask) | values.value(index)); });
    });
    onUpdate([combo, key, mask, values](const CspBrushData &d) {
        const int bits = d.integer(key) & mask;
        int index = values.indexOf(bits);
        for (int i = 0; index < 0 && i < values.size(); i++) {
            if (values[i] && (bits & values[i])) index = i;
        }
        combo->setCurrentIndex(qMax(0, index));
    });
    return combo;
}

KisAngleSelector *KisCspBrushOptionPage::addAngle(const QString &key, const QString &label)
{
    KisAngleSelector *angle = new KisAngleSelector(m_d->page);
    angle->setObjectName(key);
    angle->setRange(-180, 360);
    m_d->form->addRow(label, angle);
    connect(angle, &KisAngleSelector::angleChanged, this, [this, key](qreal v) { set(key, v); });
    onUpdate([angle, key](const CspBrushData &d) { angle->setAngle(d.real(key)); });
    return angle;
}

void KisCspBrushOptionPage::addSection(const QString &title)
{
    QLabel *label = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), m_d->page);
    m_d->form->addRow(label);
}

void KisCspBrushOptionPage::beginFoldSection(const QString &title, bool expanded)
{
    endFoldSection();
    QToolButton *header = new QToolButton(m_d->page);
    header->setObjectName("Fold " + title);
    header->setText(title);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setAutoRaise(true);
    header->setCheckable(true);
    header->setChecked(expanded);
    header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);

    QWidget *content = new QWidget(m_d->page);
    content->setObjectName("FoldContent " + title);
    QFormLayout *form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(16, 0, 0, 0);
    content->setVisible(expanded);
    connect(header, &QToolButton::toggled, content, [header, content](bool on) {
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        content->setVisible(on);
    });

    m_d->pageForm->addRow(header);
    m_d->pageForm->addRow(content);
    m_d->form = form;
}

void KisCspBrushOptionPage::endFoldSection()
{
    m_d->form = m_d->pageForm;
}

void KisCspBrushOptionPage::addNote(const QString &text)
{
    QLabel *label = new QLabel(text, m_d->page);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setItalic(true);
    label->setFont(font);
    m_d->form->addRow(label);
}
