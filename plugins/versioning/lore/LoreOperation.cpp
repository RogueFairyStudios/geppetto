/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreOperation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

#include <klocalizedstring.h>

lore_global_args_t LoreGlobals::args() const
{
    lore_global_args_t globals;
    std::memset(&globals, 0, sizeof globals);
    globals.repository_path = toLore(repositoryPath);
    globals.working_directory = toLore(workingDirectory);
    globals.identity = toLore(identity);
    globals.offline = offline ? 1 : 0;
    globals.cache = cache ? 1 : 0;
    return globals;
}

LoreProblem classifyLoreError(int status, const QString &rawText)
{
    switch (status) {
    case 16: // NotAuthenticated
    case 18: // TokenNotFound
        return LoreProblem::NotSignedIn;
    case 17: // NotAuthorized
    case 19: // WriteRequired
        return LoreProblem::NoAccess;
    case 28:  // Disconnected
    case 29:  // NotConnected
    case 30:  // Maintenance
    case 111: // NoRemote: what a call asking for the network says in offline mode
        return LoreProblem::Offline;
    case 31: // SlowDown
        return LoreProblem::Busy;
    case 42: // Divergent: the registry's code, not used by this version for a refused upload
        return LoreProblem::Diverged;
    case 45: // LockNotOwned: likewise
        return LoreProblem::LockedByOther;
    case 87: // BranchNotFound
    case 89: // RepositoryNotFound
        return LoreProblem::NotFound;
    default:
        break;
    }
    if (rawText.contains(QLatin1String("Branch has diverged"))) {
        return LoreProblem::Diverged;
    }
    if (rawText.contains(QLatin1String("already locked")) || rawText.contains(QLatin1String("locked by somebody else"))) {
        return LoreProblem::LockedByOther;
    }
    return LoreProblem::Other;
}

QByteArray lorePath(const QString &path)
{
    return QDir::fromNativeSeparators(path).toUtf8();
}

QString loreRelativePath(const QString &path)
{
    QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    while (clean.startsWith(QLatin1String("./"))) {
        clean = clean.mid(2);
    }
    while (clean.startsWith(QLatin1Char('/'))) {
        clean = clean.mid(1);
    }
    return clean;
}

QString loreServerKey(const QString &serverAddress)
{
    QString server = serverAddress.trimmed();
    while (server.endsWith(QLatin1Char('/'))) {
        server.chop(1);
    }
    return server;
}

QString loreRepositoryUrl(const KisVersioning::ProjectInfo &project)
{
    return loreServerKey(project.serverAddress) + QLatin1Char('/') + project.name;
}

bool isLoreProject(const QString &localPath)
{
    return QFileInfo(QDir(localPath).filePath(QStringLiteral(".lore"))).isDir();
}

namespace {

QString unsentRecordPath(const QString &projectPath)
{
    return QDir(projectPath).filePath(QStringLiteral(".lore/geppetto-unsent.json"));
}

} // namespace

LoreUnsent readLoreUnsent(const QString &projectPath)
{
    LoreUnsent unsent;
    QFile file(unsentRecordPath(projectPath));
    if (!file.open(QIODevice::ReadOnly)) {
        return unsent;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    unsent.versions = std::max(0, object.value(QLatin1String("versions")).toInt());
    const QJsonArray paths = object.value(QLatin1String("paths")).toArray();
    for (const QJsonValue &path : paths) {
        if (path.isString() && !unsent.paths.contains(path.toString())) {
            unsent.paths << path.toString();
        }
    }
    return unsent;
}

bool writeLoreUnsent(const QString &projectPath, const LoreUnsent &unsent)
{
    const QString path = unsentRecordPath(projectPath);
    if (unsent.isEmpty()) {
        return !QFileInfo::exists(path) || QFile::remove(path);
    }
    QJsonObject object;
    object.insert(QLatin1String("versions"), std::max(1, unsent.versions));
    object.insert(QLatin1String("paths"), QJsonArray::fromStringList(unsent.paths));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

LoreOperation::LoreOperation(KisVersioningJob *job, const LoreSettings &settings, const QString &projectPath)
    : QObject(job)
    , m_job(job)
    , m_settings(settings)
    , m_projectPath(QDir::cleanPath(QDir::fromNativeSeparators(projectPath)))
{
}

LoreOperation::~LoreOperation() = default;

KisVersioningJob *LoreOperation::job() const
{
    return m_job;
}

const LoreSettings &LoreOperation::settings() const
{
    return m_settings;
}

QString LoreOperation::projectPath() const
{
    return m_projectPath;
}

LoreGlobals LoreOperation::globals() const
{
    LoreGlobals globals;
    globals.repositoryPath = lorePath(m_projectPath);
    globals.workingDirectory = globals.repositoryPath;
    globals.identity = m_settings.identity.toUtf8();
    globals.offline = m_settings.offline || m_workOffline;
    return globals;
}

void LoreOperation::run(const LoreGlobals &globals, const LoreCall::Starter &starter, const EventHandler &onEvent,
                        const DoneHandler &onDone)
{
    auto *call = new LoreCall(this);
    if (onEvent) {
        connect(call, &LoreCall::event, this, [onEvent](const LoreEvent &event) { onEvent(event); });
    }
    connect(call, &LoreCall::completed, this, [call, onDone](int status, const QString &rawText) {
        call->deleteLater();
        onDone(status, rawText);
    });
    call->start(globals.args(), starter);
}

void LoreOperation::runStatus(bool scan, const std::function<void(const LoreStatus &)> &onStatus, const QString &failText)
{
    auto status = std::make_shared<LoreStatus>();
    run(globals(),
        [scan](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_status_args_t args;
            std::memset(&args, 0, sizeof args);
            args.staged = 1;
            args.scan = scan ? 1 : 0;
            lore_repository_status_async(globals, &args, callback);
        },
        [status](const LoreEvent &event) {
            const QVariantMap &f = event.fields;
            if (event.tag == LORE_EVENT_REPOSITORY_STATUS_REVISION) {
                status->branchName = f.value(QStringLiteral("branchName")).toString();
                status->revision = f.value(QStringLiteral("revision")).toString();
                status->number = f.value(QStringLiteral("number")).toULongLong();
                status->localNumber = f.value(QStringLiteral("localNumber")).toULongLong();
                status->remoteNumber = f.value(QStringLiteral("remoteNumber")).toULongLong();
                status->localAhead = f.value(QStringLiteral("localAhead")).toBool();
                status->remoteAhead = f.value(QStringLiteral("remoteAhead")).toBool();
                status->remoteAvailable = f.value(QStringLiteral("remoteAvailable")).toBool();
            } else if (event.tag == LORE_EVENT_REPOSITORY_STATUS_FILE) {
                LoreStatusFile file;
                file.path = f.value(QStringLiteral("path")).toString();
                file.action = f.value(QStringLiteral("action")).toInt();
                file.isFile = f.value(QStringLiteral("isFile")).toBool();
                file.staged = f.value(QStringLiteral("staged")).toBool();
                file.merged = f.value(QStringLiteral("merged")).toBool();
                file.conflict = f.value(QStringLiteral("conflict")).toBool();
                file.unresolved = f.value(QStringLiteral("unresolved")).toBool();
                file.mine = f.value(QStringLiteral("mine")).toBool();
                file.theirs = f.value(QStringLiteral("theirs")).toBool();
                file.dirty = f.value(QStringLiteral("dirty")).toBool();
                status->files.append(file);
            }
        },
        [this, status, onStatus, failText](int code, const QString &rawText) {
            if (code != 0) {
                failFromLore("status", code, rawText, failText);
                return;
            }
            onStatus(*status);
        });
}

bool LoreOperation::requireProject()
{
    if (!QFileInfo(m_projectPath).isDir()) {
        fail(KisVersioningJob::Failed, LoreText::folderMissing(m_projectPath));
        return false;
    }
    if (!isLoreProject(m_projectPath)) {
        fail(KisVersioningJob::Failed, LoreText::notAProject(m_projectPath));
        return false;
    }
    return true;
}

LoreReachability::Decision LoreReachability::decide(const QString &key) const
{
    const auto entry = m_entries.constFind(key);
    if (entry == m_entries.constEnd()) {
        return Decision::Ask;
    }
    if (entry->checking) {
        return Decision::Wait;
    }
    const qint64 reuse = entry->lingering > 0 ? loreReachabilityReuseLingeringMs : loreReachabilityReuseMs;
    if (entry->known && !entry->reachable && entry->age.isValid() && !entry->age.hasExpired(reuse)) {
        return Decision::Unreachable;
    }
    return Decision::Ask;
}

quint64 LoreReachability::begin(const QString &key)
{
    Entry &entry = m_entries[key];
    entry.checking = true;
    return ++entry.latest;
}

void LoreReachability::settle(const QString &key, quint64 check, bool reachable, bool timedOut)
{
    Entry &entry = m_entries[key];
    if (timedOut) {
        ++entry.lingering;
    }
    if (check != entry.latest) {
        return; // a newer check has begun: its answer is the one that counts
    }
    entry.checking = false;
    entry.known = true;
    entry.reachable = reachable && !timedOut;
    entry.age.start();
}

void LoreReachability::lateEnd(const QString &key, quint64 check, bool reachable)
{
    Entry &entry = m_entries[key];
    entry.lingering = std::max(0, entry.lingering - 1);
    if (check != entry.latest || entry.checking) {
        return;
    }
    entry.known = true;
    entry.reachable = reachable;
    entry.age.start();
}

int LoreReachability::lingering(const QString &key) const
{
    return m_entries.value(key).lingering;
}

namespace {

/**
 * One server check: a call, bounded by loreReachabilityWaitMs. It is no child of the operation
 * that began it: when the limit passes, the operations end without it, and its call goes on in the
 * library until it ends, which only then lets the check go.
 */
class LoreServerCheck : public QObject
{
public:
    LoreServerCheck(const std::shared_ptr<LoreMemory> &memory, const QString &key, bool readsStatus,
                    const LoreOperation::EventHandler &onEvent)
        : m_memory(memory)
        , m_key(key)
        , m_readsStatus(readsStatus)
        , m_onEvent(onEvent)
    {
    }

    void start(const LoreGlobals &globals, const LoreCall::Starter &starter)
    {
        m_number = m_memory->reachability.begin(m_key);
        m_memory->checks.insert(m_key, this);
        auto *call = new LoreCall(this);
        connect(call, &LoreCall::event, this, [this](const LoreEvent &event) {
            if (event.tag == LORE_EVENT_REPOSITORY_STATUS_REVISION) {
                m_available = event.fields.value(QStringLiteral("remoteAvailable")).toBool();
            }
            if (m_onEvent && !m_answered) {
                m_onEvent(event);
            }
        });
        connect(call, &LoreCall::completed, this, [this](int status, const QString &rawText) {
            const bool reachable = status == 0 && (!m_readsStatus || m_available);
            if (m_answered) {
                m_memory->reachability.lateEnd(m_key, m_number, reachable);
            } else {
                m_memory->reachability.settle(m_key, m_number, reachable, false);
                answer(reachable, status, rawText);
            }
            deleteLater();
        });
        QTimer::singleShot(loreReachabilityWaitMs, this, [this]() {
            if (m_answered) {
                return;
            }
            qWarning("Lore: the server did not answer within %d ms", loreReachabilityWaitMs);
            m_memory->reachability.settle(m_key, m_number, false, true);
            answer(false, 0, QString());
        });
        call->start(globals.args(), starter);
    }

    void addWaiter(LoreOperation *operation, const LoreOperation::Answer &onAnswer)
    {
        m_waiters.append({QPointer<LoreOperation>(operation), onAnswer});
    }

private:
    void answer(bool reachable, int status, const QString &rawText)
    {
        m_answered = true;
        if (m_memory->checks.value(m_key) == this) {
            m_memory->checks.remove(m_key);
        }
        const QVector<Waiter> waiters = m_waiters;
        m_waiters.clear();
        for (const Waiter &waiter : waiters) {
            if (waiter.operation) {
                waiter.onAnswer(reachable, status, rawText);
            }
        }
    }

    struct Waiter {
        QPointer<LoreOperation> operation;
        LoreOperation::Answer onAnswer;
    };

    std::shared_ptr<LoreMemory> m_memory;
    QString m_key;
    bool m_readsStatus;
    LoreOperation::EventHandler m_onEvent;
    quint64 m_number {0};
    bool m_available {false};
    bool m_answered {false};
    QVector<Waiter> m_waiters;
};

} // namespace

void LoreOperation::check(const QString &key, const LoreGlobals &globals, const LoreCall::Starter &starter,
                          bool readsStatus, bool own, const EventHandler &onEvent, const Answer &onAnswer)
{
    const std::shared_ptr<LoreMemory> memory = m_settings.memory;
    const LoreReachability::Decision decision = memory->reachability.decide(key);
    if (decision == LoreReachability::Decision::Unreachable) {
        onAnswer(false, 0, QString());
        return;
    }
    if (decision == LoreReachability::Decision::Wait && !own) {
        if (auto *underWay = static_cast<LoreServerCheck *>(memory->checks.value(key).data())) {
            underWay->addWaiter(this, onAnswer);
            return;
        }
    }
    auto *check = new LoreServerCheck(memory, key, readsStatus, onEvent);
    check->addWaiter(this, onAnswer);
    check->start(globals, starter);
}

void LoreOperation::checkProjectServer(const Answer &onAnswer)
{
    // Asked without the offline option of this operation: it is the question whether to use it.
    LoreGlobals g = globals();
    g.offline = m_settings.offline;
    check(m_projectPath, g,
        [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_status_args_t args;
            std::memset(&args, 0, sizeof args);
            args.revision_only = 1;
            lore_repository_status_async(globals, &args, callback);
        },
        true, false, EventHandler(), onAnswer);
}

void LoreOperation::checkServer(const QString &serverAddress, const Answer &onAnswer, bool ownListing,
                                const EventHandler &onEvent)
{
    if (m_settings.offline) {
        // With the library's offline option a listing ends well without asking anyone, so it
        // cannot tell; that option means no network.
        onAnswer(false, 0, QString());
        return;
    }
    const QString key = loreServerKey(serverAddress);
    LoreGlobals g = globals();
    g.repositoryPath.clear();
    g.workingDirectory.clear();
    const QByteArray url = key.toUtf8();
    check(key, g,
        [url](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_list_args_t args;
            std::memset(&args, 0, sizeof args);
            args.url = toLore(url);
            lore_repository_list_async(globals, &args, callback);
        },
        false, ownListing, onEvent, onAnswer);
}

void LoreOperation::endWithoutServer(int status, const QString &rawText, const QString &failText)
{
    if (status != 0 && classifyLoreError(status, rawText) != LoreProblem::Offline) {
        failFromLore("checking the server", status, rawText, failText);
        return;
    }
    if (status != 0) {
        qWarning("Lore: the server did not answer (status %d): %s", status, qUtf8Printable(rawText));
    }
    fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
}

void LoreOperation::requireProjectServer(const std::function<void()> &then, const QString &failText)
{
    checkProjectServer([this, then, failText](bool reachable, int status, const QString &rawText) {
        if (reachable) {
            then();
        } else {
            endWithoutServer(status, rawText, failText);
        }
    });
}

void LoreOperation::requireServer(const QString &serverAddress, const std::function<void()> &then, const QString &failText)
{
    checkServer(serverAddress, [this, then, failText](bool reachable, int status, const QString &rawText) {
        if (reachable) {
            then();
        } else {
            endWithoutServer(status, rawText, failText);
        }
    });
}

void LoreOperation::setWorkOffline(bool offline)
{
    m_workOffline = offline;
}

bool LoreOperation::stopIfCancelled()
{
    if (m_job && m_job->cancelRequested()) {
        fail(KisVersioningJob::Cancelled, LoreText::cancelled());
        return true;
    }
    return false;
}

void LoreOperation::reportProgress(qint64 done, qint64 total, const QString &label)
{
    if (m_job) {
        m_job->reportProgress(done, total, label);
    }
}

void LoreOperation::setFlushBeforeEnd(bool flush)
{
    m_flushBeforeEnd = flush;
}

void LoreOperation::endAfterFlush(const std::function<void()> &end)
{
    if (!m_flushBeforeEnd || m_ending || !isLoreProject(m_projectPath)) {
        end();
        return;
    }
    m_ending = true;
    run(globals(),
        [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_flush_args_t args;
            std::memset(&args, 0, sizeof args);
            lore_repository_flush_async(globals, &args, callback);
        },
        nullptr,
        [end](int status, const QString &rawText) {
            if (status != 0) {
                // What the operation did is done; only the wait failed.
                qWarning("Lore: waiting for the project's store failed (status %d): %s", status, qUtf8Printable(rawText));
            }
            end();
        });
}

void LoreOperation::finish(const QVariant &result)
{
    endAfterFlush([this, result]() {
        if (m_job) {
            m_job->finish(result);
        }
    });
}

void LoreOperation::fail(KisVersioningJob::Error error, const QString &text)
{
    endAfterFlush([this, error, text]() {
        if (m_job) {
            m_job->fail(error, text);
        }
    });
}

void LoreOperation::failFromLore(const char *what, int status, const QString &rawText, const QString &text)
{
    // The library's own words go to the log only.
    qWarning("Lore: %s failed (status %d): %s", what, status, qUtf8Printable(rawText));
    const LoreProblem problem = classifyLoreError(status, rawText);
    KisVersioningJob::Error error = KisVersioningJob::Failed;
    if (problem == LoreProblem::Offline) {
        error = KisVersioningJob::Offline;
    } else if (problem == LoreProblem::NotSignedIn) {
        error = KisVersioningJob::NotSignedIn;
    }
    fail(error, LoreText::forProblem(problem, text));
}

namespace LoreText {

QString cancelled()
{
    return i18n("The operation was cancelled.");
}

QString folderMissing(const QString &path)
{
    return i18n("The folder %1 does not exist.", QDir::toNativeSeparators(path));
}

QString notAProject(const QString &path)
{
    return i18n("The folder %1 is not a downloaded project.", QDir::toNativeSeparators(path));
}

QString forProblem(LoreProblem problem, const QString &fallback)
{
    switch (problem) {
    case LoreProblem::Offline:
        return i18n("The server could not be reached. Check the connection and try again.");
    case LoreProblem::NotSignedIn:
        return i18n("Sign in to the server to continue.");
    case LoreProblem::NoAccess:
        return i18n("Your account does not have access to this project.");
    case LoreProblem::NotFound:
        return i18n("The project was not found on the server.");
    case LoreProblem::Busy:
        return i18n("The server is busy. Try again in a moment.");
    case LoreProblem::Diverged:
    case LoreProblem::LockedByOther:
    case LoreProblem::Other:
        break;
    }
    return fallback;
}

} // namespace LoreText
