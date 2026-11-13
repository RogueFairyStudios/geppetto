/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreOperation.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

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
    globals.offline = m_settings.offline;
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
            recordReachability(m_projectPath, status->remoteAvailable);
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

bool LoreOperation::recentlyUnreachable(const QString &key) const
{
    const auto known = m_settings.memory->reachability.constFind(key);
    return known != m_settings.memory->reachability.constEnd() && !known->reachable && known->age.isValid()
        && !known->age.hasExpired(loreReachabilityReuseMs);
}

void LoreOperation::recordReachability(const QString &key, bool reachable)
{
    LoreMemory::Reachability &entry = m_settings.memory->reachability[key];
    entry.reachable = reachable;
    entry.age.start();
}

bool LoreOperation::stopIfRecentlyUnreachable(const QString &key, const QString &failText)
{
    const auto known = m_settings.memory->reachability.constFind(key);
    const bool stillAsking = known != m_settings.memory->reachability.constEnd() && known->asking
        && !known->askingSince.hasExpired(loreQuestionRetryMs);
    if (stillAsking || recentlyUnreachable(key)) {
        fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
        return true;
    }
    return false;
}

void LoreOperation::requireProjectServer(const std::function<void()> &then, const QString &failText)
{
    askServer(m_projectPath, globals(),
        [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_status_args_t args;
            std::memset(&args, 0, sizeof args);
            args.revision_only = 1;
            lore_repository_status_async(globals, &args, callback);
        },
        true, then, failText, EventHandler());
}

void LoreOperation::requireServer(const QString &serverAddress, const std::function<void()> &then, const QString &failText,
                                  const EventHandler &onEvent)
{
    const QString key = loreServerKey(serverAddress);
    LoreGlobals g = globals();
    g.repositoryPath.clear();
    g.workingDirectory.clear();
    const QByteArray url = key.toUtf8();
    askServer(key, g,
        [url](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_repository_list_args_t args;
            std::memset(&args, 0, sizeof args);
            args.url = toLore(url);
            lore_repository_list_async(globals, &args, callback);
        },
        false, then, failText, onEvent);
}

void LoreOperation::askServer(const QString &key, const LoreGlobals &globals, const LoreCall::Starter &starter,
                              bool readsStatus, const std::function<void()> &then, const QString &failText,
                              const EventHandler &onEvent)
{
    if (stopIfRecentlyUnreachable(key, failText)) {
        return;
    }
    if (m_settings.offline && !readsStatus) {
        // With the library's offline option a listing ends well without asking anyone, so it
        // cannot tell; that option means no network.
        recordReachability(key, false);
        fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
        return;
    }
    const std::shared_ptr<LoreMemory> memory = m_settings.memory;
    LoreMemory::Reachability &asking = memory->reachability[key];
    asking.asking = true;
    asking.askingSince.start();

    // The question is not a child of this operation: when the server does not answer, the job
    // ends without it, and the question must still clear the "asking" mark when it ends.
    auto *question = new LoreCall();
    auto available = std::make_shared<bool>(false);
    auto settled = std::make_shared<bool>(false);
    const QPointer<LoreOperation> self(this);
    connect(question, &LoreCall::event, question, [available, settled, onEvent](const LoreEvent &event) {
        if (event.tag == LORE_EVENT_REPOSITORY_STATUS_REVISION) {
            *available = event.fields.value(QStringLiteral("remoteAvailable")).toBool();
        }
        if (onEvent && !*settled) {
            onEvent(event);
        }
    });
    connect(question, &LoreCall::completed, question,
            [question, memory, key, available, readsStatus, settled, self, then, failText](int status, const QString &rawText) {
                question->deleteLater();
                const bool reachable = status == 0 && (!readsStatus || *available);
                LoreMemory::Reachability &entry = memory->reachability[key];
                entry.asking = false;
                entry.reachable = reachable;
                entry.age.start();
                if (*settled || !self) {
                    return;
                }
                *settled = true;
                if (status != 0 && classifyLoreError(status, rawText) != LoreProblem::Offline) {
                    self->failFromLore("asking the server", status, rawText, failText);
                } else if (!reachable) {
                    if (status != 0) {
                        qWarning("Lore: the server did not answer (status %d): %s", status, qUtf8Printable(rawText));
                    }
                    self->fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
                } else {
                    then();
                }
            });
    QTimer::singleShot(loreReachabilityWaitMs, this, [this, settled, failText]() {
        if (*settled) {
            return;
        }
        *settled = true;
        qWarning("Lore: the server did not answer within %d ms", loreReachabilityWaitMs);
        fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
    });
    question->start(globals.args(), starter);
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
