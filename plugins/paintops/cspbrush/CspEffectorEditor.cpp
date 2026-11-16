/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CspEffectorEditor.h"

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QVBoxLayout>

#include <kis_curve_widget.h>
#include <kis_icon_utils.h>
#include <kis_signals_blocker.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>

namespace {
QString lengthSuffix(int input)
{
    switch (input) {
    case CspEffector::Fade: return i18n(" dabs");
    case CspEffector::Distance: return i18n(" px");
    default: return i18n(" ms");
    }
}

// the curve tabs: every input but random
const CspEffector::Input curveInputs[] = {CspEffector::Pressure, CspEffector::Tilt, CspEffector::Velocity,
                                          CspEffector::Fade, CspEffector::Distance, CspEffector::Time};
}

QString CspEffectorEditor::inputName(int input)
{
    switch (input) {
    case CspEffector::Pressure: return i18n("Pressure");
    case CspEffector::Tilt: return i18n("Tilt");
    case CspEffector::Velocity: return i18n("Velocity");
    case CspEffector::Random: return i18n("Random");
    case CspEffector::Fade: return i18n("Fade");
    case CspEffector::Distance: return i18n("Distance");
    default: return i18n("Time");
    }
}

CspEffectorEditor::CspEffectorEditor(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    QGridLayout *grid = new QGridLayout();
    grid->addWidget(new QLabel(i18n("Minimum value")), 0, 1);

    for (int i = 0; i < CspEffector::InputCount; i++) {
        m_enabled[i] = new QCheckBox(inputName(i), this);
        m_enabled[i]->setObjectName(QString("input%1").arg(i));
        m_minimum[i] = new KisSliderSpinBox(this);
        m_minimum[i]->setObjectName(QString("minimum%1").arg(i));
        m_minimum[i]->setRange(0, 100);
        m_minimum[i]->setSuffix(i18n("%"));
        grid->addWidget(m_enabled[i], i + 1, 0);
        grid->addWidget(m_minimum[i], i + 1, 1);

        connect(m_enabled[i], &QCheckBox::toggled, this, [this, i](bool on) {
            m_effector.setEnabled(CspEffector::Input(i), on);
            updateWidgets();
            commit();
        });
        connect(m_minimum[i], qOverload<int>(&KisSliderSpinBox::valueChanged), this, [this, i](int value) {
            m_effector.setMinimum(CspEffector::Input(i), value / 100.0);
            commit();
        });
    }
    layout->addLayout(grid);

    m_curveTabs = new QTabBar(this);
    for (CspEffector::Input input : curveInputs) m_curveTabs->addTab(inputName(input));
    m_curveTabs->setExpanding(false);
    layout->addWidget(m_curveTabs);

    m_curve = new KisCurveWidget(this);
    m_curve->setObjectName("curve");
    m_curve->setMinimumSize(200, 200);
    m_curve->setToolTip(i18n("Tilt: the left end is upright, the right end flat.\n"
                             "Velocity: the left end is fast, the right end slow (as in CSP)."));
    layout->addWidget(m_curve, 1);

    m_lengthRow = new QWidget(this);
    QHBoxLayout *lengthLayout = new QHBoxLayout(m_lengthRow);
    lengthLayout->setContentsMargins(0, 0, 0, 0);
    m_length = new KisSliderSpinBox(m_lengthRow);
    m_length->setObjectName("length");
    m_length->setRange(1, 100000);
    m_length->setSoftRange(1, 5000);
    m_length->setPrefix(i18n("Length: "));
    m_periodic = new QCheckBox(i18n("Repeat"), m_lengthRow);
    m_periodic->setObjectName("periodic");
    lengthLayout->addWidget(m_length, 1);
    lengthLayout->addWidget(m_periodic);
    layout->addWidget(m_lengthRow);

    QPushButton *reset = new QPushButton(i18n("Linear curve"), this);
    layout->addWidget(reset, 0, Qt::AlignRight);

    connect(m_curveTabs, &QTabBar::currentChanged, this, &CspEffectorEditor::showCurve);
    connect(m_curve, &KisCurveWidget::curveChanged, this, [this](const KisCubicCurve &curve) {
        if (m_updating) return;
        m_effector.setCurve(currentInput(), curve.isIdentity() ? QList<KisCubicCurvePoint>() : curve.curvePoints());
        commit();
    });
    connect(m_length, qOverload<int>(&KisSliderSpinBox::valueChanged), this, [this](int value) {
        if (m_updating) return;
        m_effector.setLength(currentInput(), value);
        commit();
    });
    connect(m_periodic, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating) return;
        m_effector.setPeriodic(currentInput(), on);
        commit();
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        m_effector.setCurve(currentInput(), QList<KisCubicCurvePoint>());
        showCurve();
        commit();
    });

    updateWidgets();
}

void CspEffectorEditor::setSignedRandom(bool value)
{
    // hue, saturation and value jitter: a random factor in -1..1 shifts both ways
    KisSignalsBlocker b(m_minimum[CspEffector::Random]);
    m_minimum[CspEffector::Random]->setRange(value ? -100 : 0, 100);
    updateWidgets();
}

CspEffector::Input CspEffectorEditor::currentInput() const
{
    return curveInputs[qMax(0, m_curveTabs->currentIndex())];
}

void CspEffectorEditor::setEffector(const QString &text)
{
    if (text == m_effector.toString()) return;
    m_effector = CspEffector::fromString(text);
    updateWidgets();
}

void CspEffectorEditor::updateWidgets()
{
    for (int i = 0; i < CspEffector::InputCount; i++) {
        KisSignalsBlocker b(m_enabled[i], m_minimum[i]);
        const bool on = m_effector.isEnabled(CspEffector::Input(i));
        m_enabled[i]->setChecked(on);
        m_minimum[i]->setValue(qRound(m_effector.minimum(CspEffector::Input(i)) * 100));
        m_minimum[i]->setEnabled(on);
    }
    showCurve();
}

void CspEffectorEditor::showCurve()
{
    const CspEffector::Input input = currentInput();
    m_updating = true;
    const QList<KisCubicCurvePoint> &points = m_effector.curve(input);
    m_curve->setCurve(points.isEmpty() ? KisCubicCurve() : KisCubicCurve(points));
    const bool on = m_effector.isEnabled(input);
    m_curve->setEnabled(on);

    m_lengthRow->setVisible(CspEffector::hasLength(input));
    if (CspEffector::hasLength(input)) {
        m_length->setSuffix(lengthSuffix(input));
        m_length->setValue(m_effector.length(input));
        m_periodic->setChecked(m_effector.isPeriodic(input));
        m_lengthRow->setEnabled(on);
    }
    m_updating = false;
}

void CspEffectorEditor::commit()
{
    Q_EMIT effectorChanged(m_effector.toString());
}

CspEffectorButton::CspEffectorButton(const QString &title, QWidget *parent)
    : QToolButton(parent)
    , m_title(title)
{
    setAutoRaise(false);
    setCheckable(false);
    setText(QStringLiteral("…"));
    setMinimumWidth(32);
    connect(this, &QToolButton::clicked, this, &CspEffectorButton::openEditor);
}

CspEffectorButton::~CspEffectorButton()
{
    // on Android the panel belongs to the window, which outlives the button
    delete m_window;
}

void CspEffectorButton::setEffector(const QString &text)
{
    m_text = text;
    const CspEffector e = CspEffector::fromString(text);
    QStringList details;
    for (int i = 0; i < CspEffector::InputCount; i++) {
        if (!e.isEnabled(CspEffector::Input(i))) continue;
        details << i18n("%1 (minimum %2%)", CspEffectorEditor::inputName(i),
                        qRound(e.minimum(CspEffector::Input(i)) * 100));
    }
    // always "…": the inputs in use are listed in the tooltip
    setToolTip(i18n("Effect source settings of %1", m_title) + "\n"
               + (details.isEmpty() ? i18n("No dynamics") : details.join('\n')));
    if (m_editor) m_editor->setEffector(text);
}

void CspEffectorButton::setSignedRandom(bool value)
{
    m_signedRandom = value;
    if (m_editor) m_editor->setSignedRandom(value);
}

CspEffectorEditor *CspEffectorButton::editor()
{
    if (!m_window) {
#ifdef Q_OS_ANDROID
        // a separate window has no frame on Android to close it by, can take
        // the touch input from the main window, and crashes Krita when Back
        // quits: the panel floats inside the button's window, hides with it,
        // and has its own Close button
        QFrame *frame = new QFrame(window());
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setAutoFillBackground(true);
        m_window = frame;
#else
        m_window = new QWidget(this, Qt::Tool);
#endif
        m_window->setWindowTitle(i18n("Dynamics: %1", m_title));
        QVBoxLayout *layout = new QVBoxLayout(m_window);
#ifdef Q_OS_ANDROID
        // no title bar on Android: the panel draws its own, with a close button
        QHBoxLayout *header = new QHBoxLayout();
        QLabel *title = new QLabel(m_window->windowTitle(), m_window);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        header->addWidget(title, 1);
        QToolButton *close = new QToolButton(m_window);
        // the close button of Krita's canvas tabs
        close->setIcon(KisIconUtils::loadIcon("close-tab"));
        close->setAutoRaise(true);
        close->setMinimumSize(40, 40);
        close->setToolTip(i18n("Close"));
        connect(close, &QToolButton::clicked, m_window.data(), &QWidget::hide);
        header->addWidget(close, 0, Qt::AlignRight | Qt::AlignTop);
        layout->addLayout(header);
#endif
        m_editor = new CspEffectorEditor(m_window);
        m_editor->setSignedRandom(m_signedRandom);
        layout->addWidget(m_editor);
        connect(m_editor, &CspEffectorEditor::effectorChanged, this, [this](const QString &text) {
            m_text = text;
            Q_EMIT effectorChanged(text);
        });
        m_window->resize(340, 600);
    }
    m_editor->setEffector(m_text);
    return m_editor;
}

void CspEffectorButton::openEditor()
{
    editor();
#ifdef Q_OS_ANDROID
    // inside the button's window (which may have changed)
    QWidget *host = window();
    if (m_window->parentWidget() != host) {
        m_window->setParent(host);
    }
    const QSize size = m_window->sizeHint().expandedTo(QSize(340, 600)).boundedTo(host->size());
    // at the left edge of the settings page, level with the button: the
    // scratchpad to its right stays free for trying strokes
    QWidget *page = parentWidget();
    while (page && page != host && !page->inherits("KisPaintOpConfigWidget")) {
        page = page->parentWidget();
    }
    QPoint pos = mapTo(host, QPoint(0, 0));
    if (page && page != host) pos.setX(page->mapTo(host, QPoint(0, 0)).x());
    pos.setX(qBound(0, pos.x(), host->width() - size.width()));
    pos.setY(qBound(0, pos.y(), host->height() - size.height()));
    m_window->setGeometry(QRect(pos, size));
    m_window->show();
    m_window->raise();
#else
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
#endif
}
