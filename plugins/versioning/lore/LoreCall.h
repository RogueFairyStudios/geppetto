/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LORECALL_H
#define LORECALL_H

#include <functional>

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "LoreStrings.h"

/**
 * One event of a Lore call, copied out of the library's memory. @c tag is a LORE_EVENT_* value;
 * @c fields holds what the driver reads of that event, by name (strings as QString, hashes and
 * identifiers as lowercase hexadecimal QString, numbers as qint64/quint64, flags as bool).
 */
struct LoreEvent {
    int tag = 0;
    QVariantMap fields;
};

Q_DECLARE_METATYPE(LoreEvent)

struct LoreCallContext;

/**
 * One asynchronous call of the Lore library.
 *
 * The library runs the callback on its own threads. The callback copies what the driver needs of
 * each event into a LoreEvent and returns at once; the signals are emitted on this object's
 * thread through queued calls, in the order the events came. The object may be destroyed while
 * the call still runs: its events are then dropped, and the library's END still releases the
 * call.
 *
 * The first call registers a routine that runs when the application object is destroyed: it
 * waits a bounded time for every call to deliver END and then calls lore_shutdown(), or skips it
 * (and logs) when calls are still running.
 */
class LoreCall : public QObject
{
    Q_OBJECT
public:
    using Starter = std::function<void(const lore_global_args_t *, lore_event_callback_config_t)>;

    explicit LoreCall(QObject *parent = nullptr);
    ~LoreCall() override;

    /// Calls @p starter at once, with @p globals and the callback to give the library's _async
    /// function. Arguments may live on the caller's stack: the library copies them before the
    /// function returns. Call it once per object.
    void start(const lore_global_args_t &globals, const Starter &starter);

    /// Lore calls started in this process that have not delivered END yet.
    static int callsInFlight();

// The signal's name hides QObject::event(QEvent *), which stays virtual and keeps working.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#endif
Q_SIGNALS:
    void event(const LoreEvent &event);                      // on the object's thread
    void completed(int status, const QString &errorText);    // after LORE_EVENT_END
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

private:
    LoreCallContext *m_context {nullptr};
};

#endif // LORECALL_H
