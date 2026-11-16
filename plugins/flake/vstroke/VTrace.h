/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VTRACE_H
#define VTRACE_H

#include <QElapsedTimer>
#include <QString>

#include "kritavstroke_export.h"

/**
 * A timing log for finding lag, off unless Krita starts with the
 * environment variable KRITA_VSTROKE_TRACE set: to a file path, or to 1 for
 * %TEMP%/krita-vstroke-trace.txt.
 *
 * It writes, once a second, a summary per key (count, total and longest
 * time), each single event of 8 ms or more, stalls of the GUI thread
 * (a 5 ms heartbeat that came 25 ms or more late) and memory. On Windows
 * it also samples the GUI thread's stack during each stall of 100 ms or
 * more (STALL and SAMPLE lines, module+offset). Off, a
 * Scope costs one test of a static flag.
 *
 * Built only with the CMake option VSTROKE_TRACE; without it everything
 * here is an empty inline stand-in and the timing points compile away.
 */
#ifdef VSTROKE_TRACE
namespace VTrace
{
/// whether the log is on (read once)
KRITAVSTROKE_EXPORT bool enabled();
/// starts the heartbeat and the once-a-second summary (GUI thread, once)
KRITAVSTROKE_EXPORT void start();
/// \p key took \p us microseconds, in the current thread
KRITAVSTROKE_EXPORT void record(const char *key, qint64 us, const QString &detail = QString());
/// one line as it is
KRITAVSTROKE_EXPORT void note(const QString &line);

/// times its own life under \p key
class Scope
{
public:
    explicit Scope(const char *key)
        : m_key(enabled() ? key : nullptr)
    {
        if (m_key) m_timer.start();
    }
    ~Scope()
    {
        if (m_key) record(m_key, m_timer.nsecsElapsed() / 1000, m_detail);
    }
    void setDetail(const QString &detail) { if (m_key) m_detail = detail; }
    bool active() const { return m_key; }

private:
    const char *m_key;
    QElapsedTimer m_timer;
    QString m_detail;
};
}

#else // VSTROKE_TRACE

namespace VTrace
{
inline bool enabled() { return false; }
inline void start() {}
inline void record(const char *, qint64, const QString & = QString()) {}
inline void note(const QString &) {}

class Scope
{
public:
    explicit Scope(const char *) {}
    void setDetail(const QString &) {}
    bool active() const { return false; }
};
}

#endif // VSTROKE_TRACE

#endif // VTRACE_H
