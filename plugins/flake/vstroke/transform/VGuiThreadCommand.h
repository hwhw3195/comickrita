/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VGUI_THREAD_COMMAND_H
#define VGUI_THREAD_COMMAND_H

#include <functional>

#include <QApplication>
#include <QThread>

#include <KisSafeBlockingQueueConnectionProxy.h>
#include <kundo2command.h>

#include "VTrace.h"

/**
 * A command whose work runs in the GUI thread, as shapes change there only
 * (as KisShapeLayer::transform() does). Strokes run their commands in
 * worker threads: those block until the GUI thread has done the work.
 */
class VGuiThreadCommand : public KUndo2Command
{
public:
    explicit VGuiThreadCommand(const KUndo2MagicString &text = KUndo2MagicString())
        : KUndo2Command(text),
          m_connection([this] (bool redo) { apply(redo); })
    {
    }

    void redo() override { run(true); }
    void undo() override { run(false); }

protected:
    /// in the GUI thread
    virtual void apply(bool redo) = 0;

private:
    void run(bool redo)
    {
        if (QThread::currentThread() == qApp->thread()) {
            VTrace::Scope trace("gui.command.direct");
            apply(redo);
        } else {
            VTrace::Scope trace("gui.command.wait");
            m_connection.start(redo);
        }
    }

    KisSafeBlockingQueueConnectionProxy<bool> m_connection;
};

/**
 * Runs a call in the GUI thread and waits for it, from any thread. Make one
 * in the GUI thread (a plugin's installHooks()) and keep it for good.
 */
template <typename T>
class VGuiThreadCall
{
public:
    explicit VGuiThreadCall(std::function<void(T)> call)
        : m_call(call),
          m_proxy(call)
    {
    }

    void operator()(T value)
    {
        if (QThread::currentThread() == qApp->thread()) {
            VTrace::Scope trace("gui.call.direct");
            m_call(value);
        } else {
            VTrace::Scope trace("gui.call.wait");
            m_proxy.start(value);
        }
    }

private:
    std::function<void(T)> m_call;
    KisSafeBlockingQueueConnectionProxy<T> m_proxy;
};

#endif // VGUI_THREAD_COMMAND_H
