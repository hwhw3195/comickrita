/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "VTrace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <thread>

#include <tiles3/kis_tile_data_store.h>

#include "VStrokeRenderCache.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {

const qint64 SingleEventUs = 8000;
const int HeartbeatMs = 5;
const qint64 StallMs = 25;

struct Stat {
    int count = 0;
    qint64 totalUs = 0;
    qint64 maxUs = 0;
};

struct Log {
    QMutex mutex;
    QFile file;
    QTextStream out;
    QElapsedTimer clock;
    QHash<QByteArray, Stat> stats;
};

void summarizeAtExit();
std::atomic<qint64> lastBeatMs {0};
std::atomic<bool> stopping {false};

bool readEnabled()
{
    return !qEnvironmentVariableIsEmpty("KRITA_VSTROKE_TRACE");
}

Log *theLog()
{
    static Log *log = [] () -> Log* {
        if (!readEnabled()) return nullptr;
        Log *l = new Log();
        QString path = QString::fromLocal8Bit(qgetenv("KRITA_VSTROKE_TRACE"));
        if (path == QLatin1String("1")) path = QDir::temp().filePath(QStringLiteral("krita-vstroke-trace.txt"));
        l->file.setFileName(path);
        if (!l->file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            delete l;
            return nullptr;
        }
        l->out.setDevice(&l->file);
        l->clock.start();
        l->out << "# vstroke trace. Columns: ms since start, thread, key, ms, detail\n";
        l->out << "# SUMMARY lines: per second, key count total_ms max_ms\n";
        // the last second's summary when the application ends
        qAddPostRoutine(&summarizeAtExit);
        return l;
    }();
    return log;
}

QString threadName()
{
    if (QCoreApplication *app = QCoreApplication::instance()) {
        if (QThread::currentThread() == app->thread()) return QStringLiteral("gui");
    }
    return QStringLiteral("w%1").arg(quintptr(QThread::currentThreadId()) % 100000);
}

qint64 workingSetMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return qint64(counters.WorkingSetSize) / (1024 * 1024);
    }
#endif
    return -1;
}

void summarize()
{
    Log *l = theLog();
    if (!l) return;
    QMutexLocker locker(&l->mutex);
    const qint64 now = l->clock.elapsed();
    for (auto it = l->stats.constBegin(); it != l->stats.constEnd(); ++it) {
        l->out << now << "\tSUMMARY\t" << it.key() << '\t' << it->count << '\t'
               << QString::number(it->totalUs / 1000.0, 'f', 1) << '\t'
               << QString::number(it->maxUs / 1000.0, 'f', 1) << '\n';
    }
    l->stats.clear();
    l->out << now << "\tMEMORY\tworkingSetMB " << workingSetMb()
           << "\ttilesInMemory " << KisTileDataStore::instance()->numTilesInMemory()
           << "\ttiles " << KisTileDataStore::instance()->numTiles()
           << "\trenderCaches " << VStrokeRenderCache::liveCaches()
           << "\trenderImagesMB " << VStrokeRenderCache::liveImageBytes() / (1024 * 1024) << '\n';
    l->out.flush();
}

void summarizeAtExit()
{
    stopping = true;
    summarize();
}

#ifdef Q_OS_WIN
/*
 * The stall sampler: a thread that, while the GUI thread's heartbeat is late,
 * suspends the GUI thread about every 10 ms and copies its return addresses.
 * When the heartbeat comes back it writes each sample as module+offset,
 * which llvm-symbolizer --relative-address turns into functions. Nothing
 * is allocated while the GUI thread is suspended.
 */
const qint64 SampleAfterMs = 60;
const qint64 WriteStallsFromMs = 100;
const int MaxFrames = 64;
const int MaxSamples = 300;

struct Sample {
    int count = 0;
    DWORD64 pc[MaxFrames];
};

int walkStack(HANDLE thread, DWORD64 *out)
{
    if (SuspendThread(thread) == DWORD(-1)) return 0;
    int n = 0;
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        while (n < MaxFrames && context.Rip) {
            out[n++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function) {
                // a leaf function: the return address is on top of the stack
                if (!context.Rsp) break;
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function,
                                 &context, &handlerData, &establisherFrame, nullptr);
            }
        }
    }
    ResumeThread(thread);
    return n;
}

QString moduleOffset(DWORD64 pc, QHash<quintptr, QString> &names)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &module) || !module) {
        return QStringLiteral("?+0x%1").arg(pc, 0, 16);
    }
    auto it = names.find(quintptr(module));
    if (it == names.end()) {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        it = names.insert(quintptr(module), QFileInfo(QString::fromWCharArray(path, int(length))).fileName());
    }
    return QStringLiteral("%1+0x%2").arg(*it).arg(pc - DWORD64(module), 0, 16);
}

void writeStall(qint64 startMs, qint64 lengthMs, const Sample *samples, int count, QHash<quintptr, QString> &names)
{
    static int stallId = 0;
    stallId++;
    VTrace::note(QStringLiteral("STALL	%1	from %2 ms	%3 ms	%4 samples")
                 .arg(stallId).arg(startMs).arg(lengthMs).arg(count));
    for (int i = 0; i < count; i++) {
        QStringList frames;
        for (int f = 0; f < samples[i].count; f++) frames << moduleOffset(samples[i].pc[f], names);
        VTrace::note(QStringLiteral("SAMPLE	%1	%2").arg(stallId).arg(frames.join(QLatin1Char(' '))));
    }
}

void startSampler()
{
    HANDLE gui = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gui,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return;
    }
    std::thread([gui] () {
        Log *l = theLog();
        Sample *samples = new Sample[MaxSamples];
        QHash<quintptr, QString> names;
        int count = 0;
        qint64 stallStart = -1;
        while (!stopping) {
            Sleep(10);
            const qint64 now = l->clock.elapsed();
            const qint64 beat = lastBeatMs;
            if (now - beat >= SampleAfterMs) {
                if (stallStart < 0) {
                    stallStart = beat;
                    count = 0;
                }
                if (count < MaxSamples) {
                    samples[count].count = walkStack(gui, samples[count].pc);
                    if (samples[count].count) count++;
                }
            } else if (stallStart >= 0) {
                const qint64 length = beat - stallStart;
                if (length >= WriteStallsFromMs && count) writeStall(stallStart, length, samples, count, names);
                stallStart = -1;
            }
        }
    }).detach();
}
#endif

}

bool VTrace::enabled()
{
    static const bool on = theLog() != nullptr;
    return on;
}

void VTrace::record(const char *key, qint64 us, const QString &detail)
{
    Log *l = theLog();
    if (!l) return;
    QMutexLocker locker(&l->mutex);
    Stat &s = l->stats[QByteArray(key)];
    s.count++;
    s.totalUs += us;
    s.maxUs = qMax(s.maxUs, us);
    if (us >= SingleEventUs) {
        l->out << l->clock.elapsed() << '\t' << threadName() << '\t' << key << '\t'
               << QString::number(us / 1000.0, 'f', 1) << '\t' << detail << '\n';
    }
}

void VTrace::note(const QString &line)
{
    Log *l = theLog();
    if (!l) return;
    QMutexLocker locker(&l->mutex);
    l->out << l->clock.elapsed() << '\t' << threadName() << '\t' << line << '\n';
}

void VTrace::start()
{
    static bool started = false;
    if (started || !enabled() || !QCoreApplication::instance()) return;
    started = true;

    QObject *app = QCoreApplication::instance();
    note(QStringLiteral("trace started"));

    QTimer *summary = new QTimer(app);
    QObject::connect(summary, &QTimer::timeout, &summarize);
    summary->start(1000);

    // the GUI thread's heartbeat: a tick that comes late means the thread was busy
    QTimer *heartbeat = new QTimer(app);
    heartbeat->setTimerType(Qt::PreciseTimer);
    QElapsedTimer *last = new QElapsedTimer();
    last->start();
    QObject::connect(heartbeat, &QTimer::timeout, [last] () {
        if (Log *l = theLog()) lastBeatMs = l->clock.elapsed();
        const qint64 gap = last->elapsed();
        last->restart();
        if (gap >= StallMs) record("gui.stall", gap * 1000);
    });
    heartbeat->start(HeartbeatMs);
    lastBeatMs = theLog()->clock.elapsed();

#ifdef Q_OS_WIN
    startSampler();
#endif
}
