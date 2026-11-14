/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreCall.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QtGlobal>

#include <atomic>

/// Shared by a LoreCall and the library until both let go of it: the call may be destroyed before
/// the library delivers END, and the library may deliver END before the call is destroyed.
struct LoreCallContext {
    QMutex mutex;
    LoreCall *target {nullptr};   ///< guarded by mutex; null once the LoreCall is gone
    std::atomic<int> references {2};
    int status {0};               ///< from COMPLETE; only the library's callback touches these
    QString message;

    void release()
    {
        if (--references == 0) {
            delete this;
        }
    }
};

namespace {

/// How long the application waits at exit for running calls before skipping lore_shutdown().
const unsigned long shutdownWaitMs = 10000;

QMutex inFlightMutex;
QWaitCondition inFlightChanged;
int inFlight = 0;
bool shutdownRegistered = false;
bool shutDown = false;

void callStarted()
{
    QMutexLocker locker(&inFlightMutex);
    ++inFlight;
}

void callEnded()
{
    QMutexLocker locker(&inFlightMutex);
    --inFlight;
    inFlightChanged.wakeAll();
}

/// Runs when the application object is destroyed.
void shutdownLore()
{
    QMutexLocker locker(&inFlightMutex);
    QDeadlineTimer deadline(shutdownWaitMs);
    while (inFlight > 0) {
        if (!inFlightChanged.wait(&inFlightMutex, deadline)) {
            break;
        }
    }
    if (inFlight > 0) {
        // lore_shutdown() must wait for the calls to deliver END; a call without network can take
        // minutes. Exiting without it is safer than hanging the exit.
        qWarning("Lore: %d call(s) still running at exit; lore_shutdown() skipped", inFlight);
        return;
    }
    shutDown = true;
    const int32_t status = lore_shutdown();
    if (status != 0) {
        qWarning("Lore: lore_shutdown() returned %d", int(status));
    }
}

void registerShutdown()
{
    QMutexLocker locker(&inFlightMutex);
    if (!shutdownRegistered && QCoreApplication::instance()) {
        shutdownRegistered = true;
        qAddPostRoutine(shutdownLore);
    }
}

QString hex(const uint8_t *data, int size)
{
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(data), size).toHex());
}

QString hex(const lore_hash_t &hash)
{
    return hex(hash.data, int(sizeof hash.data));
}

QString hex(const lore_partition_t &partition)
{
    return hex(partition.data, int(sizeof partition.data));
}

QVariant number(quint64 value)
{
    return QVariant(qulonglong(value));
}

/// Copies what the driver reads of @p e. Runs on the library's thread, inside the callback.
LoreEvent convert(const lore_event_t *e)
{
    LoreEvent event;
    event.tag = int(e->tag);
    QVariantMap &f = event.fields;
    switch (e->tag) {
    case LORE_EVENT_METADATA:
        f.insert(QStringLiteral("key"), fromLore(e->metadata.key));
        if (e->metadata.value.tag == LORE_METADATA_TYPE_STRING) {
            f.insert(QStringLiteral("text"), fromLore(e->metadata.value.string));
        } else if (e->metadata.value.tag == LORE_METADATA_TYPE_NUMERIC) {
            f.insert(QStringLiteral("number"), number(e->metadata.value.numeric));
        }
        break;
    case LORE_EVENT_AUTH_URL:
        f.insert(QStringLiteral("url"), fromLore(e->auth_url.url));
        f.insert(QStringLiteral("userCode"), fromLore(e->auth_url.user_code));
        break;
    case LORE_EVENT_AUTH_USER_INFO:
        f.insert(QStringLiteral("id"), fromLore(e->auth_user_info.id));
        f.insert(QStringLiteral("name"), fromLore(e->auth_user_info.name));
        break;
    case LORE_EVENT_AUTH_USER_TOKEN:
        f.insert(QStringLiteral("id"), fromLore(e->auth_user_token.id));
        f.insert(QStringLiteral("sessionValid"), e->auth_user_token.session_valid != 0);
        break;
    case LORE_EVENT_REPOSITORY_CREATE:
        f.insert(QStringLiteral("id"), hex(e->repository_create.id));
        f.insert(QStringLiteral("name"), fromLore(e->repository_create.name));
        break;
    case LORE_EVENT_REPOSITORY_LIST_ENTRY:
        f.insert(QStringLiteral("id"), hex(e->repository_list_entry.id));
        f.insert(QStringLiteral("name"), fromLore(e->repository_list_entry.name));
        break;
    case LORE_EVENT_REPOSITORY_CLONE_BEGIN:
        f.insert(QStringLiteral("repository"), hex(e->repository_clone_begin.repository));
        f.insert(QStringLiteral("revision"), hex(e->repository_clone_begin.revision));
        break;
    case LORE_EVENT_REPOSITORY_CLONE_PROGRESS:
        f.insert(QStringLiteral("bytesDone"), number(e->repository_clone_progress.count.bytes_transferred));
        f.insert(QStringLiteral("bytesTotal"), number(e->repository_clone_progress.count.bytes_total));
        break;
    case LORE_EVENT_REPOSITORY_CLONE_END:
        f.insert(QStringLiteral("revision"), hex(e->repository_clone_end.revision));
        break;
    case LORE_EVENT_REVISION_COMMIT_PROGRESS:
        f.insert(QStringLiteral("bytesDone"), number(e->revision_commit_progress.count.bytes_transferred));
        f.insert(QStringLiteral("bytesTotal"), number(e->revision_commit_progress.count.bytes_total));
        break;
    case LORE_EVENT_REVISION_COMMIT_REVISION:
        f.insert(QStringLiteral("revision"), hex(e->revision_commit_revision.revision));
        f.insert(QStringLiteral("number"), number(e->revision_commit_revision.revision_number));
        break;
    case LORE_EVENT_BRANCH_PUSH_FRAGMENT_PROGRESS:
        f.insert(QStringLiteral("bytesDone"), number(e->branch_push_fragment_progress.bytes_transferred));
        f.insert(QStringLiteral("bytesTotal"), number(e->branch_push_fragment_progress.bytes_total));
        break;
    case LORE_EVENT_REVISION_SYNC_FILE:
        f.insert(QStringLiteral("path"), fromLore(e->revision_sync_file.path));
        f.insert(QStringLiteral("action"), int(e->revision_sync_file.action));
        f.insert(QStringLiteral("isFile"), e->revision_sync_file.flag_file != 0);
        break;
    case LORE_EVENT_REVISION_SYNC_PROGRESS:
        f.insert(QStringLiteral("bytesDone"), number(e->revision_sync_progress.bytes_update));
        f.insert(QStringLiteral("bytesTotal"), number(e->revision_sync_progress.bytes_update_total));
        break;
    case LORE_EVENT_BRANCH_MERGE_START_END:
        f.insert(QStringLiteral("automerged"), number(e->branch_merge_start_end.stats.file_automerge));
        f.insert(QStringLiteral("conflicts"), number(e->branch_merge_start_end.stats.file_conflict));
        break;
    case LORE_EVENT_BRANCH_MERGE_CONFLICT_FILE:
        f.insert(QStringLiteral("path"), fromLore(e->branch_merge_conflict_file.path));
        break;
    case LORE_EVENT_REPOSITORY_STATUS_REVISION: {
        const lore_repository_status_revision_event_data_t &d = e->repository_status_revision;
        f.insert(QStringLiteral("repository"), hex(d.repository));
        f.insert(QStringLiteral("branchName"), fromLore(d.branch_name));
        f.insert(QStringLiteral("revision"), hex(d.revision));
        f.insert(QStringLiteral("number"), number(d.revision_number));
        f.insert(QStringLiteral("localNumber"), number(d.revision_local_number));
        f.insert(QStringLiteral("remoteNumber"), number(d.revision_remote_number));
        f.insert(QStringLiteral("remoteRevision"), hex(d.revision_remote));
        f.insert(QStringLiteral("localAhead"), d.is_local_ahead != 0);
        f.insert(QStringLiteral("remoteAhead"), d.is_remote_ahead != 0);
        f.insert(QStringLiteral("remoteAvailable"), d.remote_available != 0);
        break;
    }
    case LORE_EVENT_REPOSITORY_STATUS_FILE: {
        const lore_repository_status_file_event_data_t &d = e->repository_status_file;
        f.insert(QStringLiteral("path"), fromLore(d.path));
        f.insert(QStringLiteral("action"), int(d.action));
        f.insert(QStringLiteral("isFile"), d.type == LORE_NODE_TYPE_FILE);
        f.insert(QStringLiteral("staged"), d.flag_staged != 0);
        f.insert(QStringLiteral("merged"), d.flag_merged != 0);
        f.insert(QStringLiteral("conflict"), d.flag_conflict != 0);
        f.insert(QStringLiteral("unresolved"), d.flag_conflict_unresolved != 0);
        f.insert(QStringLiteral("mine"), d.flag_conflict_mine != 0);
        f.insert(QStringLiteral("theirs"), d.flag_conflict_theirs != 0);
        f.insert(QStringLiteral("dirty"), d.flag_dirty != 0);
        break;
    }
    case LORE_EVENT_REPOSITORY_CONFIG_GET:
        f.insert(QStringLiteral("key"), fromLore(e->repository_config_get.key));
        f.insert(QStringLiteral("value"), fromLore(e->repository_config_get.value));
        break;
    case LORE_EVENT_FILE_HISTORY:
        f.insert(QStringLiteral("revision"), hex(e->file_history.revision));
        f.insert(QStringLiteral("number"), number(e->file_history.revision_number));
        break;
    case LORE_EVENT_LOCK_FILE_QUERY:
        f.insert(QStringLiteral("path"), fromLore(e->lock_file_query.path));
        f.insert(QStringLiteral("owner"), fromLore(e->lock_file_query.owner));
        break;
    case LORE_EVENT_STORAGE_OPENED:
        f.insert(QStringLiteral("handle"), number(e->storage_opened.handle_id));
        break;
    case LORE_EVENT_REVISION_TREE_LOADED:
        f.insert(QStringLiteral("handle"), number(e->revision_tree_loaded.handle_id));
        break;
    case LORE_EVENT_REVISION_TREE_RESOLVE_PATH_COMPLETE:
        f.insert(QStringLiteral("node"), number(e->revision_tree_resolve_path_complete.node_id));
        f.insert(QStringLiteral("errorCode"), int(e->revision_tree_resolve_path_complete.error_code));
        break;
    case LORE_EVENT_REVISION_TREE_CHILD:
        f.insert(QStringLiteral("name"), fromLore(e->revision_tree_child.name));
        f.insert(QStringLiteral("isDirectory"), e->revision_tree_child.kind == LORE_NODE_TYPE_DIRECTORY);
        f.insert(QStringLiteral("errorCode"), int(e->revision_tree_child.error_code));
        break;
    default:
        break;
    }
    return event;
}

/// The library's callback. Copies, posts to the call's thread, returns.
void onLoreEvent(const lore_event_t *e, uint64_t userContext)
{
    auto *context = reinterpret_cast<LoreCallContext *>(uintptr_t(userContext));

    if (e->tag == LORE_EVENT_COMPLETE) {
        // Events of one call arrive one at a time, so nothing else touches these meanwhile.
        context->status = int(e->complete.status);
        context->message = fromLore(e->complete.error.message);
        return;
    }

    if (e->tag == LORE_EVENT_END) {
        const int status = context->status;
        const QString message = context->message;
        {
            QMutexLocker locker(&context->mutex);
            if (LoreCall *target = context->target) {
                QMetaObject::invokeMethod(target, [target, status, message]() {
                    Q_EMIT target->completed(status, message);
                }, Qt::QueuedConnection);
            }
        }
        // Nothing of the library touches the context after END.
        context->release();
        callEnded();
        return;
    }

    const LoreEvent event = convert(e);
    QMutexLocker locker(&context->mutex);
    if (LoreCall *target = context->target) {
        // A call posted to an object that is then destroyed is dropped by Qt.
        QMetaObject::invokeMethod(target, [target, event]() {
            Q_EMIT target->event(event);
        }, Qt::QueuedConnection);
    }
}

} // namespace

LoreCall::LoreCall(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<LoreEvent>("LoreEvent");
}

LoreCall::~LoreCall()
{
    if (m_context) {
        {
            QMutexLocker locker(&m_context->mutex);
            m_context->target = nullptr;
        }
        m_context->release();
    }
}

void LoreCall::start(const lore_global_args_t &globals, const Starter &starter)
{
    Q_ASSERT(!m_context);
    {
        QMutexLocker locker(&inFlightMutex);
        if (shutDown) {
            // The application is being destroyed; nothing may call the library any more.
            locker.unlock();
            QMetaObject::invokeMethod(this, [this]() {
                Q_EMIT completed(LORE_ERROR_CODE_INTERNAL, QStringLiteral("Lore has been shut down"));
            }, Qt::QueuedConnection);
            return;
        }
    }
    registerShutdown();

    m_context = new LoreCallContext;
    m_context->target = this;
    callStarted();

    lore_event_callback_config_t callback;
    std::memset(&callback, 0, sizeof callback);
    callback.user_context = uint64_t(uintptr_t(m_context));
    callback.func = onLoreEvent;
    starter(&globals, callback);
}

int LoreCall::callsInFlight()
{
    QMutexLocker locker(&inFlightMutex);
    return inFlight;
}
