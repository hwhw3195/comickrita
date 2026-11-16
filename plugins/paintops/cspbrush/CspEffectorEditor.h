/*
 *  SPDX-FileCopyrightText: 2026 Krita CSP Brush contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CSP_EFFECTOR_EDITOR_H
#define CSP_EFFECTOR_EDITOR_H

#include <QPointer>
#include <QToolButton>
#include <QWidget>

#include "CspEffector.h"

class QCheckBox;
class QTabBar;
class QWidget;
class KisSliderSpinBox;
class KisCurveWidget;

/**
 * CSP's "effect source settings" popup for one setting: which inputs
 * (pressure, tilt, velocity, random, fade, distance, time) change it, the
 * minimum each can bring it down to, and the input curves, edited in
 * Krita's curve widget.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspEffectorEditor : public QWidget
{
    Q_OBJECT
public:
    explicit CspEffectorEditor(QWidget *parent = nullptr);

    /// Random's minimum may go down to -100% (color jitter of hue, saturation, value: both ways);
    /// otherwise 0..100% as in CSP
    void setSignedRandom(bool value);

    /// Sets the edited effector (serialized form), emits nothing
    void setEffector(const QString &text);
    QString effector() const { return m_effector.toString(); }

    static QString inputName(int input);

Q_SIGNALS:
    void effectorChanged(const QString &text);

private:
    void updateWidgets();
    void showCurve();
    CspEffector::Input currentInput() const;
    void commit();

    CspEffector m_effector;
    QCheckBox *m_enabled[CspEffector::InputCount];
    KisSliderSpinBox *m_minimum[CspEffector::InputCount];
    QTabBar *m_curveTabs {nullptr};
    KisCurveWidget *m_curve {nullptr};
    QWidget *m_lengthRow {nullptr};
    KisSliderSpinBox *m_length {nullptr};
    QCheckBox *m_periodic {nullptr};
    bool m_updating {false};
};

/**
 * The small button next to a setting that opens its CspEffectorEditor
 * (in a tool window, so strokes can be tried while it is open; on Android
 * in a panel inside the button's window). Its tooltip lists the inputs in use.
 */
class KRITACSPBRUSHPAINTOP_EXPORT CspEffectorButton : public QToolButton
{
    Q_OBJECT
public:
    CspEffectorButton(const QString &title, QWidget *parent = nullptr);
    ~CspEffectorButton() override;

    /// See CspEffectorEditor::setSignedRandom
    void setSignedRandom(bool value);

    void setEffector(const QString &text);
    CspEffectorEditor *editor();

Q_SIGNALS:
    void effectorChanged(const QString &text);

private:
    void openEditor();

    QString m_title;
    QString m_text;
    QPointer<QWidget> m_window; ///< on Android a child of the button's window, not of the button
    CspEffectorEditor *m_editor {nullptr};
    bool m_signedRandom {false};
};

#endif // CSP_EFFECTOR_EDITOR_H
