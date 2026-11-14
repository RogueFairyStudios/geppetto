/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreDriver.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>

#include <KUser>
#include <klocalizedstring.h>

#include <KisVersioningFileRules.h>

#include "LoreFolderListing.h"
#include "LoreOperation.h"
#include "LoreSyncOperation.h"

using namespace KisVersioning;

namespace {

/// What a locked file's owner is on a server that does not know who calls.
const char unknownOwner[] = "<unknown>";

/// True when this computer has work the server does not have: a change on disk or prepared for a
/// version, a version not sent, or a merge not finished.
bool hasLocalWork(const LoreStatus &status, const QString &backupSuffix)
{
    if (status.localAhead) {
        return true;
    }
    return std::any_of(status.files.begin(), status.files.end(), [&backupSuffix](const LoreStatusFile &file) {
        return file.isFile && !isNeverSynced(file.path, backupSuffix) && (file.dirty || file.staged || file.conflict);
    });
}

/// A rule of a view file that leaves out one top-level folder. Characters the rules treat
/// specially are escaped.
QString viewRuleFor(const QString &folder)
{
    QString escaped;
    for (const QChar c : loreRelativePath(folder)) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('[')) {
            escaped += QLatin1Char('\\');
        }
        escaped += c;
    }
    return QStringLiteral("/%1/").arg(escaped);
}

/**
 * The top-level folders of the server that a choice leaves out: those not in @p chosen. Folders
 * whose name starts with a dot (such as .geppetto, which holds what everyone must see) are never
 * left out. The view rules ignore case, and so does this.
 */
QStringList foldersLeftOut(const QStringList &serverFolders, const QStringList &chosen)
{
    QStringList kept;
    for (const QString &folder : chosen) {
        kept << loreRelativePath(folder).toLower();
    }
    QStringList leftOut;
    for (const QString &folder : serverFolders) {
        const QString name = loreRelativePath(folder);
        if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.contains(QLatin1Char('/'))
            || kept.contains(name.toLower()) || leftOut.contains(name)) {
            continue;
        }
        leftOut << name;
    }
    return leftOut;
}

/**
 * Writes the view file that leaves out @p leftOut into @p directory and returns its path, or an
 * empty string. The library copies the file into the project, so it only has to exist during the
 * call. Only those folders are left out: root files and any folder that exists only on this
 * computer stay in the view, so that whatever the artist creates is sent. An empty list keeps the
 * whole project.
 */
QString writeViewFile(const QString &directory, const QStringList &leftOut)
{
    QStringList lines;
    lines << QStringLiteral("# The folders of the project left out of this computer.");
    for (const QString &folder : leftOut) {
        lines << viewRuleFor(folder);
    }
    const QString path = QDir(directory).filePath(QStringLiteral("view"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    const QByteArray bytes = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written && file.error() == QFileDevice::NoError ? path : QString();
}

/// Ends @p op after the folder listing it needed failed: the listing's own words when they are
/// about the server or the session, @p failText otherwise.
void failAfterListing(LoreOperation *op, KisVersioningJob *listing, const QString &failText)
{
    const KisVersioningJob::Error error = listing->error();
    if (error == KisVersioningJob::Offline || error == KisVersioningJob::NotSignedIn
        || error == KisVersioningJob::Cancelled) {
        op->fail(error, listing->errorText());
    } else {
        op->fail(KisVersioningJob::Failed, failText);
    }
}

/// Builds a history entry from the events of lore_file_history: each FILE_HISTORY is followed by
/// the METADATA of its revision.
struct HistoryReader {
    QVector<VersionInfo> newestFirst;

    void read(const LoreEvent &event)
    {
        if (event.tag == LORE_EVENT_FILE_HISTORY) {
            VersionInfo info;
            info.id = event.fields.value(QStringLiteral("revision")).toString();
            info.number = int(event.fields.value(QStringLiteral("number")).toULongLong());
            newestFirst.append(info);
        } else if (event.tag == LORE_EVENT_METADATA && !newestFirst.isEmpty()) {
            VersionInfo &info = newestFirst.last();
            const QString key = event.fields.value(QStringLiteral("key")).toString();
            if (key == QLatin1String("created-by")) {
                info.author = event.fields.value(QStringLiteral("text")).toString();
            } else if (key == QLatin1String("committed-by") && info.author.isEmpty()) {
                info.author = event.fields.value(QStringLiteral("text")).toString();
            } else if (key == QLatin1String("timestamp")) {
                info.date =
                    QDateTime::fromMSecsSinceEpoch(qint64(event.fields.value(QStringLiteral("number")).toULongLong())).toUTC();
            } else if (key == QLatin1String("message")) {
                info.notes = event.fields.value(QStringLiteral("text")).toString();
            } else if (key == QLatin1String("merged-by")) {
                info.isMerge = true;
            }
        }
    }
};

} // namespace

LoreDriver::LoreDriver(QObject *parent)
    : KisVersioningDriver(parent)
    , m_memory(std::make_shared<LoreMemory>())
{
}

LoreDriver::~LoreDriver() = default;

QString LoreDriver::id() const
{
    return QStringLiteral("lore");
}

Capabilities LoreDriver::capabilities() const
{
    return SignIn | RemoteProjects | PartialDownload | Presence | Notes;
}

QString LoreDriver::currentUser() const
{
    return m_testIdentity.isEmpty() ? m_user : m_testIdentity;
}

void LoreDriver::setOfflineForTesting(bool offline)
{
    m_offline = offline;
    // What the server answered before belongs to the other mode.
    m_memory->reachability = LoreReachability();
    m_memory->checks.clear();
}

void LoreDriver::setIdentityForTesting(const QString &identity)
{
    m_testIdentity = identity;
}

LoreSettings LoreDriver::settings() const
{
    LoreSettings settings;
    settings.identity = m_testIdentity.isEmpty() ? m_identity : m_testIdentity;
    settings.offline = m_offline;
    settings.memory = m_memory;
    settings.backupSuffix = configuredBackupSuffix();
    return settings;
}

KisVersioningJob *LoreDriver::signIn(const QString &serverAddress)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), QString());
    const QString address = serverAddress.trimmed();
    const QString failText = i18n("Could not sign in to %1.", address);
    if (address.isEmpty()) {
        op->fail(KisVersioningJob::Failed, failText);
        return job;
    }

    LoreGlobals g = op->globals();
    g.repositoryPath.clear();
    g.workingDirectory.clear();
    g.identity.clear();
    const QByteArray url = address.toUtf8();
    auto name = std::make_shared<QString>();
    QPointer<LoreDriver> self(this);
    op->run(g,
        [url](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_auth_login_interactive_args_t args;
            std::memset(&args, 0, sizeof args);
            args.remote_url = toLore(url);
            // The library would open the browser itself and fail when it cannot; Qt opens it here.
            args.no_browser = 1;
            args.reuse_session = 1;
            lore_auth_login_interactive_async(globals, &args, callback);
        },
        [op, name](const LoreEvent &event) {
            if (event.tag == LORE_EVENT_AUTH_URL) {
                // The address comes from the server: only a web page is opened, never a program or
                // a file, and plain http only on this computer.
                const QUrl url(event.fields.value(QStringLiteral("url")).toString());
                if (!loreSignInAddressIsSafe(url)) {
                    qWarning("Lore: signing in asked to open an address that is not safe: %s",
                             qUtf8Printable(url.toString()));
                    op->fail(KisVersioningJob::Failed,
                             i18n("The server asked to open an address that is not safe, so signing in was stopped."));
                    return;
                }
                const QString code = event.fields.value(QStringLiteral("userCode")).toString();
                op->reportProgress(0, 0, code.isEmpty()
                                             ? i18n("Finish signing in in the browser.")
                                             : i18n("Finish signing in in the browser. The code to confirm is %1.", code));
                QDesktopServices::openUrl(url);
            } else if (event.tag == LORE_EVENT_AUTH_USER_INFO) {
                const QString user = event.fields.value(QStringLiteral("name")).toString();
                *name = user.isEmpty() ? event.fields.value(QStringLiteral("id")).toString() : user;
            }
        },
        [op, self, name, failText](int status, const QString &rawText) {
            if (status == 0) {
                if (self) {
                    // The session is the library's; only the name is kept, for this session.
                    self->m_user = *name;
                    self->m_identity.clear();
                }
                op->finish();
                return;
            }
            if (status == 9 && rawText.contains(QLatin1String("No authentication configured"))) {
                // A development or test server: it does not know who calls, so the operating
                // system user name is who this is.
                if (self) {
                    self->m_user = KUser().loginName();
                    self->m_identity = self->m_user;
                }
                op->finish();
                return;
            }
            op->failFromLore("sign in", status, rawText, failText);
        });
    return job;
}

KisVersioningJob *LoreDriver::resumeSession(const QString &serverAddress)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), QString());
    const QString address = loreServerKey(serverAddress);
    const QString failText = i18n("Could not find out who is signed in.");
    if (address.isEmpty()) {
        op->fail(KisVersioningJob::NotSignedIn, LoreText::forProblem(LoreProblem::NotSignedIn, failText));
        return job;
    }
    LoreGlobals g = op->globals();
    g.repositoryPath.clear();
    g.workingDirectory.clear();
    g.identity.clear();
    const QByteArray endpoint = address.toUtf8();
    auto valid = std::make_shared<QSet<QString>>();
    auto found = std::make_shared<QString>();
    QPointer<LoreDriver> self(this);
    // First the session the library keeps on this computer: read here, with no browser.
    op->run(g,
        [endpoint](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_auth_local_user_info_args_t args;
            std::memset(&args, 0, sizeof args);
            args.auth_endpoint = toLore(endpoint);
            lore_auth_local_user_info_async(globals, &args, callback);
        },
        [valid, found](const LoreEvent &event) {
            // A stored identity comes with its token first: only one whose session is still valid counts.
            const QString id = event.fields.value(QStringLiteral("id")).toString();
            if (event.tag == LORE_EVENT_AUTH_USER_TOKEN && event.fields.value(QStringLiteral("sessionValid")).toBool()) {
                valid->insert(id);
            } else if (event.tag == LORE_EVENT_AUTH_USER_INFO && found->isEmpty() && valid->contains(id)) {
                const QString user = event.fields.value(QStringLiteral("name")).toString();
                *found = user.isEmpty() ? id : user;
            }
        },
        [op, self, found, address, failText](int status, const QString &rawText) {
            if (status != 0) {
                qWarning("Lore: reading the stored session failed (status %d): %s", status, qUtf8Printable(rawText));
            }
            if (!found->isEmpty()) {
                if (self) {
                    self->m_user = *found;
                    self->m_identity.clear();
                }
                op->finish(QVariant(*found));
                return;
            }
            // None kept. A server that lists its projects to nobody in particular has no
            // authentication: the system user is who this is, as signIn() finds.
            op->checkServer(address,
                [op, self, failText](bool reachable, int checkStatus, const QString &checkText) {
                    if (!reachable) {
                        op->endWithoutServer(checkStatus, checkText, failText);
                        return;
                    }
                    const QString user = KUser().loginName();
                    if (self) {
                        self->m_user = user;
                        self->m_identity = user;
                    }
                    op->finish(QVariant(user));
                },
                true);
        });
    return job;
}

KisVersioningJob *LoreDriver::signOut()
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), QString());
    if (!m_identity.isEmpty() || m_user.isEmpty()) {
        // No session with the library: a server without authentication, or nobody signed in.
        m_user.clear();
        m_identity.clear();
        op->finish();
        return job;
    }
    LoreGlobals g = op->globals();
    g.repositoryPath.clear();
    g.workingDirectory.clear();
    QPointer<LoreDriver> self(this);
    op->run(g,
        [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_auth_logout_args_t args;
            std::memset(&args, 0, sizeof args);
            lore_auth_logout_async(globals, &args, callback);
        },
        nullptr,
        [op, self](int status, const QString &rawText) {
            if (status != 0) {
                op->failFromLore("sign out", status, rawText, i18n("Could not sign out."));
                return;
            }
            if (self) {
                self->m_user.clear();
            }
            op->finish();
        });
    return job;
}

KisVersioningJob *LoreDriver::listProjects(const QString &serverAddress)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), QString());
    const QString address = loreServerKey(serverAddress);
    const QString failText = i18n("Could not list the projects on the server.");
    if (address.isEmpty()) {
        op->fail(KisVersioningJob::Failed, failText);
        return job;
    }
    // The listing is itself the server check, with its time limit; it is never shared, since this
    // operation needs the listing.
    auto projects = std::make_shared<QVector<ProjectInfo>>();
    op->checkServer(address,
        [op, projects, failText](bool reachable, int status, const QString &rawText) {
            if (!reachable) {
                op->endWithoutServer(status, rawText, failText);
                return;
            }
            std::sort(projects->begin(), projects->end(), [](const ProjectInfo &a, const ProjectInfo &b) {
                return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
            });
            op->finish(QVariant::fromValue(*projects));
        },
        true,
        [projects, address](const LoreEvent &event) {
            if (event.tag == LORE_EVENT_REPOSITORY_LIST_ENTRY) {
                ProjectInfo info;
                info.id = event.fields.value(QStringLiteral("id")).toString();
                info.name = event.fields.value(QStringLiteral("name")).toString();
                info.serverAddress = address;
                projects->append(info);
            }
        });
    return job;
}

KisVersioningJob *LoreDriver::listFolders(const ProjectInfo &project)
{
    return startLoreFolderListing(settings(), project);
}

KisVersioningJob *LoreDriver::downloadProject(const ProjectInfo &project, const QString &localPath,
                                              const QStringList &folders)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    const QString target = op->projectPath();
    const QString failText = i18n("Could not download %1.", project.name);
    if (target.isEmpty()) {
        op->fail(KisVersioningJob::Failed, failText);
        return job;
    }
    auto clone = [op, project, target, failText](const QStringList &leftOut) {
        auto viewFolder = std::make_shared<QTemporaryDir>();
        QString view;
        if (!leftOut.isEmpty()) {
            view = viewFolder->isValid() ? writeViewFile(viewFolder->path(), leftOut) : QString();
            if (view.isEmpty()) {
                op->fail(KisVersioningJob::Failed, failText);
                return;
            }
        }
        op->requireServer(project.serverAddress, [op, project, target, view, viewFolder, leftOut, failText]() {
            // The folder is created here, so that nothing is left behind if the download fails.
            const bool existed = QFileInfo::exists(target);
            if (!QDir().mkpath(target)) {
                op->fail(KisVersioningJob::Failed, failText);
                return;
            }
            LoreGlobals g = op->globals();
            // Every version file is kept in the project's own store, so that a version can be
            // created with no network.
            g.cache = true;
            const QByteArray url = loreRepositoryUrl(project).toUtf8();
            const QByteArray viewPath = lorePath(view);
            op->reportProgress(0, 0, i18n("Downloading %1", project.name));
            op->run(g,
                [url, viewPath](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                    lore_repository_clone_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.repository_url = toLore(url);
                    args.view = toLore(viewPath);
                    lore_repository_clone_async(globals, &args, callback);
                },
                [op, project](const LoreEvent &event) {
                    if (event.tag == LORE_EVENT_REPOSITORY_CLONE_PROGRESS) {
                        op->reportProgress(event.fields.value(QStringLiteral("bytesDone")).toLongLong(),
                                           event.fields.value(QStringLiteral("bytesTotal")).toLongLong(),
                                           i18n("Downloading %1", project.name));
                    }
                },
                [op, viewFolder, target, existed, leftOut, failText](int status, const QString &rawText) {
                    if (status != 0) {
                        if (existed) {
                            op->failFromLore("download", status, rawText, failText);
                            return;
                        }
                        // What the failed download left goes, on a worker, before the job ends.
                        op->onWorker<bool>([target]() { return QDir(target).removeRecursively(); },
                            [op, status, rawText, failText](const bool &) {
                                op->failFromLore("download", status, rawText, failText);
                            });
                        return;
                    }
                    op->setFlushBeforeEnd(true);
                    op->finish(QVariant::fromValue(leftOut));
                });
        }, failText);
    };

    if (folders.isEmpty()) {
        clone(QStringList());
        return job;
    }
    // The folders of the server that were not chosen are left out; the others, and any folder made
    // on this computer later, are kept. The server's folders are read first, without downloading.
    op->reportProgress(0, 0, i18n("Downloading %1", project.name));
    KisVersioningJob *listing = listFolders(project);
    QObject::connect(listing, &KisVersioningJob::finished, op, [op, folders, clone, failText](KisVersioningJob *listed) {
        if (listed->error() != KisVersioningJob::NoError) {
            failAfterListing(op, listed, failText);
            return;
        }
        clone(foldersLeftOut(listed->result<QStringList>(), folders));
    });
    return job;
}

namespace {

/// Puts into effect the view that leaves out @p leftOut, and ends the job with that list.
void applyView(LoreOperation *op, const QStringList &leftOut, const QString &failText)
{
    auto viewFolder = std::make_shared<QTemporaryDir>();
    const QString view = viewFolder->isValid() ? writeViewFile(viewFolder->path(), leftOut) : QString();
    if (view.isEmpty()) {
        op->fail(KisVersioningJob::Failed, failText);
        return;
    }
    if (op->stopIfCancelled()) {
        return;
    }
    const QByteArray viewPath = lorePath(view);
    op->reportProgress(0, 0, i18n("Changing the folders kept on this computer"));
    op->run(op->globals(),
        [viewPath](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_revision_sync_args_t args;
            std::memset(&args, 0, sizeof args);
            args.view = toLore(viewPath);
            lore_revision_sync_async(globals, &args, callback);
        },
        nullptr,
        [op, viewFolder, leftOut, failText](int code, const QString &rawText) {
            if (code != 0) {
                op->failFromLore("changing folders", code, rawText, failText);
                return;
            }
            op->finish(QVariant::fromValue(leftOut));
        });
}

} // namespace

KisVersioningJob *LoreDriver::setFolders(const QString &localPath, const QStringList &folders)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    op->setFlushBeforeEnd(true);
    const QString failText = i18n("Could not change the folders kept on this computer.");

    // Changing the folders receives the latest versions too, through the library's plain sync,
    // which would merge text by itself and create the merged version with nobody told. It only
    // runs on a project with nothing of this computer waiting: no change on disk, no version not
    // sent, no merge open.
    op->requireProjectServer([op, folders, failText]() {
        op->runStatus(true, [op, folders, failText](const LoreStatus &status) {
            if (op->stopIfCancelled()) {
                return;
            }
            if (hasLocalWork(status, op->settings().backupSuffix)) {
                op->fail(KisVersioningJob::Failed, i18n("Synchronize the project before changing its folders."));
                return;
            }
            if (!status.remoteAvailable) {
                op->fail(KisVersioningJob::Offline, LoreText::forProblem(LoreProblem::Offline, failText));
                return;
            }
            if (folders.isEmpty()) {
                applyView(op, QStringList(), failText);
                return;
            }
            // Only the folders of the server that were not chosen are left out. The project's
            // address on the server is its server and its identifier: the folder kept here does
            // not record its name.
            auto server = std::make_shared<QString>();
            op->run(op->globals(),
                [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                    static const QByteArray key("remote_url");
                    lore_repository_config_get_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.key = toLore(key);
                    lore_repository_config_get_async(globals, &args, callback);
                },
                [server](const LoreEvent &event) {
                    if (event.tag == LORE_EVENT_REPOSITORY_CONFIG_GET
                        && event.fields.value(QStringLiteral("key")).toString() == QLatin1String("remote_url")) {
                        *server = event.fields.value(QStringLiteral("value")).toString();
                    }
                },
                [op, status, folders, server, failText](int code, const QString &rawText) {
                    if (code != 0 || server->isEmpty() || status.repository.isEmpty()) {
                        op->failFromLore("reading the server address", code ? code : -1, rawText, failText);
                        return;
                    }
                    ProjectInfo project;
                    project.id = status.repository;
                    project.name = status.repository;
                    project.serverAddress = *server;
                    KisVersioningJob *listing = startLoreFolderListing(op->settings(), project);
                    QObject::connect(listing, &KisVersioningJob::finished, op,
                                     [op, folders, failText](KisVersioningJob *listed) {
                                         if (listed->error() != KisVersioningJob::NoError) {
                                             failAfterListing(op, listed, failText);
                                             return;
                                         }
                                         applyView(op, foldersLeftOut(listed->result<QStringList>(), folders),
                                                   failText);
                                     });
                });
        }, failText);
    }, failText);
    return job;
}

KisVersioningJob *LoreDriver::status(const QString &localPath)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    // Reading the state compares the files on disk and records what it found.
    op->setFlushBeforeEnd(true);
    const QString failText = i18n("Could not read the state of the project.");
    // The state also says what the server has; without an answer from it in time, the state is
    // read from this computer alone, with nothing counted on the server's side.
    op->checkProjectServer([op, failText](bool reachable, int checkStatus, const QString &checkText) {
        if (!reachable) {
            if (checkStatus != 0 && classifyLoreError(checkStatus, checkText) != LoreProblem::Offline) {
                op->endWithoutServer(checkStatus, checkText, failText);
                return;
            }
            op->setWorkOffline(true);
        }
        op->runStatus(true, [op](const LoreStatus &status) {
            const QString project = op->projectPath();
            ProjectStatus result;
            for (const LoreStatusFile &file : status.files) {
                // Files only staged by a merge came from the server; the rest are this computer's.
                const bool local = file.dirty || (file.staged && !file.merged) || file.unresolved;
                if (!file.isFile || !local || isNeverSynced(file.path, op->settings().backupSuffix)) {
                    continue;
                }
                FileStatus entry;
                entry.path = file.path;
                entry.state = file.action == LORE_FILE_ACTION_ADD      ? FileState::Added
                            : file.action == LORE_FILE_ACTION_DELETE ? FileState::Deleted
                                                                      : FileState::Modified;
                result.files.append(entry);
            }
            std::sort(result.files.begin(), result.files.end(), [](const FileStatus &a, const FileStatus &b) {
                return a.path < b.path;
            });
            if (status.remoteAvailable) {
                const bool known = std::any_of(status.remoteRevision.begin(), status.remoteRevision.end(),
                                               [](QChar c) { return c != QLatin1Char('0'); });
                if (known) {
                    result.serverVersion = status.remoteRevision;
                }
                if (status.remoteAhead) {
                    result.incoming = int(std::max<qint64>(1, qint64(status.remoteNumber) - qint64(status.number)));
                }
                if (status.localAhead) {
                    result.pendingUpload = int(std::max<qint64>(1, qint64(status.localNumber) - qint64(status.remoteNumber)));
                } else if (!readLoreUnsent(project).isEmpty() && !writeLoreUnsent(project, LoreUnsent())) {
                    // The server has everything: the record of what waited is out of date.
                    qWarning("Lore: could not clear the record of versions not sent in %s", qUtf8Printable(project));
                }
            } else {
                // Without network the library knows nothing of the server's side: the versions this
                // computer made and did not send come from the driver's own record.
                const LoreUnsent unsent = readLoreUnsent(project);
                result.pendingUpload = unsent.isEmpty() ? 0 : std::max(1, unsent.versions);
            }
            op->finish(QVariant::fromValue(result));
        }, failText);
    });
    return job;
}

KisVersioningJob *LoreDriver::synchronise(const QString &localPath, const QString &notes)
{
    return startLoreSynchronise(settings(), localPath, notes);
}

KisVersioningJob *LoreDriver::resolve(const QString &localPath, const QString &path, Resolution resolution,
                                      const QString &mergedFile)
{
    return startLoreResolve(settings(), localPath, path, resolution, mergedFile);
}

KisVersioningJob *LoreDriver::fileHistory(const QString &localPath, const QString &path, int offset, int limit)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    const QByteArray file = loreRelativePath(path).toUtf8();
    const QString failText = i18n("Could not read the versions of %1.", loreRelativePath(path));
    auto reader = std::make_shared<HistoryReader>();
    op->run(op->globals(),
        [file](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_file_history_args_t args;
            std::memset(&args, 0, sizeof args);
            args.path = toLore(file);
            args.length = 0; // every entry
            lore_file_history_async(globals, &args, callback);
        },
        [reader](const LoreEvent &event) { reader->read(event); },
        [op, reader, offset, limit, failText](int status, const QString &rawText) {
            if (status != 0) {
                op->failFromLore("history", status, rawText, failText);
                return;
            }
            // The library lists the newest first.
            QVector<VersionInfo> history;
            const int first = std::max(0, offset);
            for (int i = reader->newestFirst.size() - 1 - first; i >= 0; --i) {
                if (limit > 0 && history.size() >= limit) {
                    break;
                }
                history.append(reader->newestFirst.at(i));
            }
            op->finish(QVariant::fromValue(history));
        });
    return job;
}

KisVersioningJob *LoreDriver::fetchFileAtVersion(const QString &localPath, const QString &path,
                                                 const QString &versionId, const QString &outputPath)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    const QString file = loreRelativePath(path);
    const QString failText = i18n("Could not get %1 as it was in that version.", file);
    const QFileInfo output(QDir::cleanPath(QDir::fromNativeSeparators(outputPath)));
    if (versionId.trimmed().isEmpty() || !QDir().mkpath(output.absolutePath())) {
        op->fail(KisVersioningJob::Failed, failText);
        return job;
    }
    // Written beside the output and moved over it once complete, so that an existing output is
    // never left half written.
    const QString partial = QDir(output.absolutePath())
                                .filePath(QStringLiteral(".%1.%2.part")
                                              .arg(output.fileName())
                                              .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
    const QByteArray filePath = file.toUtf8();
    const QByteArray revision = versionId.trimmed().toUtf8();
    const QByteArray partialPath = lorePath(partial);
    const QString target = output.absoluteFilePath();
    op->run(op->globals(),
        [filePath, revision, partialPath](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
            lore_file_write_args_t args;
            std::memset(&args, 0, sizeof args);
            args.path = toLore(filePath);
            args.revision = toLore(revision);
            args.output = toLore(partialPath);
            lore_file_write_async(globals, &args, callback);
        },
        nullptr,
        [op, partial, target, failText](int status, const QString &rawText) {
            if (status != 0) {
                QFile::remove(partial);
                op->failFromLore("getting a version", status, rawText, failText);
                return;
            }
            QFile::remove(target);
            if (!QFile::rename(partial, target)) {
                QFile::remove(partial);
                op->fail(KisVersioningJob::Failed, failText);
                return;
            }
            op->finish();
        });
    return job;
}

KisVersioningJob *LoreDriver::announceEditing(const QString &localPath, const QString &path, bool editing)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    const QString file = loreRelativePath(path);
    const QByteArray filePath = file.toUtf8();
    const QString failText = editing ? i18n("Could not tell the others that you are editing %1.", file)
                                     : i18n("Could not tell the others that you stopped editing %1.", file);
    op->requireProjectServer([op, file, filePath, editing, failText]() {
        op->run(op->globals(),
            [filePath, editing](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                const lore_string_t paths[] = {toLore(filePath)};
                // An empty branch is the project's current one; an empty owner is the caller.
                if (editing) {
                    lore_lock_file_acquire_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.paths.ptr = paths;
                    args.paths.count = 1;
                    lore_lock_file_acquire_async(globals, &args, callback);
                } else {
                    lore_lock_file_release_args_t args;
                    std::memset(&args, 0, sizeof args);
                    args.paths.ptr = paths;
                    args.paths.count = 1;
                    lore_lock_file_release_async(globals, &args, callback);
                }
            },
            nullptr,
            [op, file, failText](int status, const QString &rawText) {
                if (status != 0) {
                    if (classifyLoreError(status, rawText) == LoreProblem::LockedByOther) {
                        // Someone else's notice is there first; the panel says nothing.
                        qWarning("Lore: editing notice refused (status %d): %s", status, qUtf8Printable(rawText));
                        op->fail(KisVersioningJob::Failed, i18n("Someone else is already editing %1.", file));
                        return;
                    }
                    op->failFromLore("editing notice", status, rawText, failText);
                    return;
                }
                op->finish();
            });
    }, failText);
    return job;
}

KisVersioningJob *LoreDriver::whoIsEditing(const QString &localPath, const QStringList &paths)
{
    auto *job = new KisVersioningJob();
    auto *op = new LoreOperation(job, settings(), localPath);
    if (!op->requireProject()) {
        return job;
    }
    QSet<QString> wanted;
    for (const QString &path : paths) {
        wanted.insert(loreRelativePath(path));
    }
    auto editing = std::make_shared<QVector<EditingInfo>>();
    const QString failText = i18n("Could not find out who is editing.");
    op->requireProjectServer([op, editing, wanted, failText]() {
        op->run(op->globals(),
            [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_lock_file_query_args_t args;
                std::memset(&args, 0, sizeof args);
                lore_lock_file_query_async(globals, &args, callback);
            },
            [editing, wanted](const LoreEvent &event) {
                if (event.tag != LORE_EVENT_LOCK_FILE_QUERY) {
                    return;
                }
                const QString path = loreRelativePath(event.fields.value(QStringLiteral("path")).toString());
                const QString owner = event.fields.value(QStringLiteral("owner")).toString();
                // A server that does not know who calls names every owner "<unknown>": never report
                // a nameless editor.
                // No path asked for: every file of the project.
                if ((wanted.isEmpty() || wanted.contains(path)) && !owner.isEmpty()
                    && owner != QLatin1String(unknownOwner)) {
                    EditingInfo info;
                    info.path = path;
                    info.user = owner;
                    editing->append(info);
                }
            },
            [op, editing, failText](int status, const QString &rawText) {
                if (status != 0) {
                    op->failFromLore("asking who is editing", status, rawText, failText);
                    return;
                }
                op->finish(QVariant::fromValue(*editing));
            });
    }, failText);
    return job;
}
