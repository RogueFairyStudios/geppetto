/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LoreSyncOperation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <memory>

#include <klocalizedstring.h>

#include <KisVersioningFileRules.h>
#include <KisVersioningJob.h>

#include "LoreOperation.h"

using namespace KisVersioning;

namespace {

/// The files the library writes beside a file both sides changed. The merge commit removes them.
const char *const mineSuffix = "~mine";
const char *const theirsSuffix = "~theirs";
const char *const baseSuffix = "~base";

QString syncFailText()
{
    return i18n("Could not synchronize the project.");
}

bool hasSidecars(const QDir &root, const QString &path)
{
    const QString file = root.filePath(path);
    return QFileInfo::exists(file + QLatin1String(mineSuffix)) || QFileInfo::exists(file + QLatin1String(theirsSuffix))
        || QFileInfo::exists(file + QLatin1String(baseSuffix));
}

/// Replaces @p target with the bytes of @p source in one step: on failure @p target is as it was.
bool replaceFile(const QString &target, const QString &source)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 20);
        if ((chunk.isEmpty() && in.error() != QFileDevice::NoError) || out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            return false;
        }
    }
    return out.commit();
}

/// True when @p path holds the markers the library writes when it merges text with a conflict.
bool hasConflictMarkers(const QString &path)
{
    QFile file(path);
    if (file.size() > (64 << 20) || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    bool open = false;
    bool middle = false;
    bool close = false;
    for (const QByteArray &line : file.readAll().split('\n')) {
        open = open || line.startsWith("<<<<<<< ");
        middle = middle || line == "=======" || line == "=======\r";
        close = close || line.startsWith(">>>>>>> ");
    }
    return open && middle && close;
}

/// What a file the library reports in conflict is, from what the merge left on disk.
DivergenceKind kindOf(const QDir &root, const QString &path)
{
    const QString file = root.filePath(path);
    if (!QFileInfo::exists(file)) {
        return DivergenceKind::DeletedHereModifiedThere;
    }
    if (QFileInfo::exists(file + QLatin1String(theirsSuffix))) {
        return DivergenceKind::BothModified;
    }
    if (QFileInfo::exists(file + QLatin1String(baseSuffix))) {
        return DivergenceKind::ModifiedHereDeletedThere; // the other side is a deletion
    }
    return DivergenceKind::BothModified;
}

/// What a worker that puts the artist's versions back found: the path it could not put back, or
/// none, and the divergences it read from what is on disk.
struct GivenBack {
    bool ok = true;
    QString failedPath;
    QVector<Divergence> divergences;
};

/// Runs on a worker. For each path, when the library left the artist's own bytes in "~mine" (a
/// text merge), they go back into the working file; @p onlyWithMarkers does it only when the
/// working file holds the library's conflict markers. Then reads what each path is.
GivenBack giveBackMine(const QString &projectPath, const QStringList &paths, bool onlyWithMarkers)
{
    GivenBack outcome;
    const QDir root(projectPath);
    for (const QString &path : paths) {
        const QString file = root.filePath(path);
        const QString mine = file + QLatin1String(mineSuffix);
        if (QFileInfo::exists(mine) && (!onlyWithMarkers || hasConflictMarkers(file)) && !replaceFile(file, mine)) {
            outcome.ok = false;
            outcome.failedPath = path;
            return outcome;
        }
        outcome.divergences.append({path, kindOf(root, path), QString()});
    }
    return outcome;
}

QString keepMineFailText(const QString &path)
{
    return path.isEmpty()
        ? i18n("Could not keep your version of the files that both of you changed, so the new versions were not taken. Synchronize again.")
        : i18n("Could not keep your version of %1, so the new versions were not taken. Synchronize again.", path);
}

void appendUnique(QStringList *list, const QStringList &items)
{
    for (const QString &item : items) {
        if (!list->contains(item)) {
            list->append(item);
        }
    }
}

/// Runs a call whose arguments hold one list of paths.
using PathsStarter = std::function<void(const lore_global_args_t *, lore_event_callback_config_t, lore_string_array_t)>;

LoreCall::Starter withPaths(const QStringList &paths, const PathsStarter &starter)
{
    QList<QByteArray> utf8;
    for (const QString &path : paths) {
        utf8 << path.toUtf8();
    }
    return [utf8, starter](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
        QVector<lore_string_t> strings;
        for (const QByteArray &path : utf8) {
            strings << toLore(path);
        }
        lore_string_array_t array;
        std::memset(&array, 0, sizeof array);
        array.ptr = strings.constData();
        array.count = uintptr_t(strings.size());
        starter(globals, callback, array);
    };
}

/**
 * The order of the calls is the one task 1 recorded: stage, commit, push, and on a refused push
 * a merge with the server, settled by the artist through resolve() and sent by the next call.
 */
class SyncOperation : public LoreOperation
{
public:
    SyncOperation(KisVersioningJob *job, const LoreSettings &settings, const QString &localPath, const QString &notes)
        : LoreOperation(job, settings, localPath)
        , m_notes(notes)
    {
        setFlushBeforeEnd(true);
    }

    void start()
    {
        if (!requireProject()) {
            return;
        }
        reportProgress(0, 0, i18n("Looking for changes"));
        // Without an answer from the server in time, everything runs offline: the version is
        // created on this computer (downloaded with the local cache) and waits to be sent.
        checkProjectServer([this](bool reachable, int checkStatus, const QString &checkText) {
            if (!reachable) {
                if (checkStatus != 0 && classifyLoreError(checkStatus, checkText) != LoreProblem::Offline) {
                    endWithoutServer(checkStatus, checkText, syncFailText());
                    return;
                }
                setWorkOffline(true);
            }
            runStatus(true, [this](const LoreStatus &status) { onStatus(status); }, syncFailText());
        });
    }

private:
    /// A version was created here: it waits to be sent, with the files in it.
    void recordUnsentVersion(const QStringList &paths)
    {
        LoreUnsent unsent = readLoreUnsent(projectPath());
        ++unsent.versions;
        appendUnique(&unsent.paths, paths);
        if (!writeLoreUnsent(projectPath(), unsent)) {
            // This run still reports the work as waiting; a state read with no network may not.
            qWarning("Lore: could not record the versions not sent yet in %s", qUtf8Printable(projectPath()));
        }
    }

    QSet<QString> &keptAfterMerge()
    {
        return settings().memory->keptAfterMerge[projectPath()];
    }

    QString notesFor(int changedFiles) const
    {
        return m_notes.trimmed().isEmpty() ? defaultNotes(std::max(1, changedFiles)) : m_notes;
    }

    void onStatus(const LoreStatus &status)
    {
        m_status = status;
        const QDir root(projectPath());
        QStringList unresolved;
        QStringList mergedByLore;
        int mergedConflicts = 0;
        for (const LoreStatusFile &file : status.files) {
            if (!file.isFile) {
                continue;
            }
            if (file.merged && file.staged) {
                m_mergeInProgress = true;
                ++m_mergedFiles;
                mergedConflicts += file.conflict ? 1 : 0;
            }
            if (file.conflict && file.unresolved) {
                unresolved << file.path;
            } else if (file.conflict && !file.mine && !file.theirs && !hasSidecars(root, file.path)
                       && !keptAfterMerge().contains(file.path)) {
                // Merged by the library on its own and not settled by the artist yet: an earlier
                // synchronisation reported it and did not get to the end.
                mergedByLore << file.path;
            } else if (file.dirty && !file.conflict && !isNeverSynced(file.path, settings().backupSuffix)) {
                m_toStage << file.path;
            } else if (file.staged && !file.merged && !file.conflict && !isNeverSynced(file.path, settings().backupSuffix)) {
                // Prepared by an earlier synchronisation that did not create its version (cancelled,
                // failed, or the application stopped in between). The library can report such a file
                // as not dirty once the file on disk matches what was staged; it is still work to send.
                m_alreadyStaged << file.path;
            }
        }
        m_changedFiles = m_toStage.size() + m_alreadyStaged.size() + mergedConflicts;

        if (!unresolved.isEmpty() || !mergedByLore.isEmpty()) {
            reportOpenDivergences(unresolved, mergedByLore);
            return;
        }
        if (m_toStage.isEmpty()) {
            commit();
        } else {
            stage();
        }
    }

    /// Divergences of a merge still waiting for the artist: reported again, nothing is sent.
    void reportOpenDivergences(const QStringList &unresolved, const QStringList &mergedByLore)
    {
        // The library writes the text merge, markers included, into the working file. If an
        // earlier synchronisation stopped before putting the artist's version back, it goes back
        // now, on a worker: the files can be large.
        const QString project = projectPath();
        onWorker<GivenBack>([project, unresolved]() { return giveBackMine(project, unresolved, true); },
            [this, mergedByLore](const GivenBack &given) {
                if (!given.ok) {
                    fail(KisVersioningJob::Failed, keepMineFailText(given.failedPath));
                    return;
                }
                m_result.divergences += given.divergences;
                for (const QString &path : mergedByLore) {
                    m_result.divergences.append({path, DivergenceKind::BothModified, QString()});
                }
                m_result.pendingUpload = true;
                finish(QVariant::fromValue(m_result));
            });
    }

    void stage()
    {
        if (stopIfCancelled()) {
            return;
        }
        reportProgress(0, 0, i18n("Preparing your files"));
        run(globals(),
            withPaths(m_toStage, [](const lore_global_args_t *globals, lore_event_callback_config_t callback,
                                    lore_string_array_t paths) {
                lore_file_stage_args_t args;
                std::memset(&args, 0, sizeof args);
                args.paths = paths;
                lore_file_stage_async(globals, &args, callback);
            }),
            nullptr,
            [this](int status, const QString &rawText) {
                if (status != 0) {
                    failFromLore("preparing files", status, rawText, syncFailText());
                    return;
                }
                commit();
            });
    }

    /// Creates the version of what was prepared, and of a settled merge. Without network it is
    /// created on this computer only (the folder was downloaded with the local cache).
    void commit()
    {
        if (m_toStage.isEmpty() && m_alreadyStaged.isEmpty() && !m_mergeInProgress) {
            afterCommit();
            return;
        }
        if (stopIfCancelled()) {
            return;
        }
        LoreGlobals g = globals();
        // A default commit without network waits minutes before it gives up; offline it is instant.
        g.offline = g.offline || !m_status.remoteAvailable;
        const QByteArray message = notesFor(m_changedFiles ? m_changedFiles : m_mergedFiles).toUtf8();
        const QString label = i18n("Creating a version");
        reportProgress(0, 0, label);
        run(g,
            [message](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_revision_commit_args_t args;
                std::memset(&args, 0, sizeof args);
                args.message = toLore(message);
                lore_revision_commit_async(globals, &args, callback);
            },
            [this, label](const LoreEvent &event) { reportBytes(event, LORE_EVENT_REVISION_COMMIT_PROGRESS, label); },
            [this](int status, const QString &rawText) {
                if (status == 40) { // nothing staged
                    afterCommit();
                    return;
                }
                if (status != 0) {
                    failFromLore("creating a version", status, rawText, syncFailText());
                    return;
                }
                m_committed = true;
                recordUnsentVersion(m_toStage + m_alreadyStaged);
                if (m_mergeInProgress) {
                    keptAfterMerge().clear();
                }
                afterCommit();
            });
    }

    void afterCommit()
    {
        if (!m_status.remoteAvailable) {
            // Versions made here before, in this run of the application or an earlier one, wait too.
            m_result.pendingUpload = m_committed || !readLoreUnsent(projectPath()).isEmpty();
            finish(QVariant::fromValue(m_result));
            return;
        }
        if (m_committed || m_status.localAhead) {
            push();
        } else if (m_status.remoteAhead) {
            receive();
        } else {
            finish(QVariant::fromValue(m_result));
        }
    }

    void push()
    {
        if (stopIfCancelled()) {
            return;
        }
        const QString label = i18n("Sending your work");
        reportProgress(0, 0, label);
        run(globals(),
            [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_branch_push_args_t args;
                std::memset(&args, 0, sizeof args);
                lore_branch_push_async(globals, &args, callback);
            },
            [this, label](const LoreEvent &event) { reportBytes(event, LORE_EVENT_BRANCH_PUSH_FRAGMENT_PROGRESS, label); },
            [this](int status, const QString &rawText) {
                if (status == 0) {
                    m_result.sent = readLoreUnsent(projectPath()).paths;
                    m_result.sent.sort();
                    if (!writeLoreUnsent(projectPath(), LoreUnsent())) {
                        qWarning("Lore: could not clear the record of versions not sent in %s",
                                 qUtf8Printable(projectPath()));
                    }
                    m_result.pendingUpload = false;
                    finish(QVariant::fromValue(m_result));
                    return;
                }
                if (classifyLoreError(status, rawText) == LoreProblem::Diverged) {
                    if (m_merged) {
                        // The server changed again while this computer merged: the session asks again.
                        m_result.serverChanged = true;
                        m_result.pendingUpload = true;
                        finish(QVariant::fromValue(m_result));
                        return;
                    }
                    mergeWithServer();
                    return;
                }
                failFromLore("sending", status, rawText, syncFailText());
            });
    }

    /// Nothing of this computer to send: the latest versions come in, with no merge.
    void receive()
    {
        if (stopIfCancelled()) {
            return;
        }
        const QString label = i18n("Receiving the latest versions");
        reportProgress(0, 0, label);
        run(globals(),
            [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_revision_sync_args_t args;
                std::memset(&args, 0, sizeof args);
                lore_revision_sync_async(globals, &args, callback);
            },
            [this, label](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REVISION_SYNC_FILE) {
                    noteReceived(event);
                } else {
                    reportBytes(event, LORE_EVENT_REVISION_SYNC_PROGRESS, label);
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0) {
                    failFromLore("receiving", status, rawText, syncFailText());
                    return;
                }
                finish(QVariant::fromValue(m_result));
            });
    }

    /// The server holds versions this computer does not have. The library's own merge would
    /// commit by itself, after merging text files line by line with nobody told; it is started
    /// here without its commit, so that every file both sides changed can be seen and given back
    /// to the artist before anything is created.
    void mergeWithServer()
    {
        if (stopIfCancelled()) {
            return;
        }
        runStatus(false, [this](const LoreStatus &status) { startMerge(status); }, syncFailText());
    }

    void startMerge(const LoreStatus &status)
    {
        m_localRevision = status.revision;
        if (status.branchName.isEmpty() || m_localRevision.isEmpty()) {
            qWarning("Lore: no current branch or version to merge into");
            fail(KisVersioningJob::Failed, syncFailText());
            return;
        }
        keptAfterMerge().clear();
        const QByteArray branch = status.branchName.toUtf8();
        const QString label = i18n("Receiving the latest versions");
        reportProgress(0, 0, label);
        run(globals(),
            [branch](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_branch_merge_start_args_t args;
                std::memset(&args, 0, sizeof args);
                args.branch = toLore(branch);
                args.no_commit = 1;
                lore_branch_merge_start_async(globals, &args, callback);
            },
            [this, label](const LoreEvent &event) {
                if (event.tag == LORE_EVENT_REVISION_SYNC_FILE) {
                    noteReceived(event);
                } else if (event.tag == LORE_EVENT_BRANCH_MERGE_START_END) {
                    m_mergedByLoreCount = event.fields.value(QStringLiteral("automerged")).toULongLong();
                } else {
                    reportBytes(event, LORE_EVENT_REVISION_SYNC_PROGRESS, label);
                }
            },
            [this](int status, const QString &rawText) {
                if (status != 0) {
                    failFromLore("merging", status, rawText, syncFailText());
                    return;
                }
                runStatus(false, [this](const LoreStatus &merged) { afterMerge(merged); }, syncFailText());
            });
    }

    void afterMerge(const LoreStatus &status)
    {
        int mergedFiles = 0;
        for (const LoreStatusFile &file : status.files) {
            if (!file.isFile) {
                continue;
            }
            mergedFiles += file.merged ? 1 : 0;
            if (file.conflict && file.unresolved) {
                m_conflicts << file.path;
            } else if (file.conflict) {
                // Both sides changed it and the library merged the two by itself.
                m_mergedByLore << file.path;
            }
        }
        for (const QString &path : m_conflicts + m_mergedByLore) {
            m_result.received.removeAll(path);
        }

        if (quint64(m_mergedByLore.size()) < m_mergedByLoreCount) {
            // The library merged files that the state does not show: they cannot be given back.
            qWarning("Lore: %llu files merged by the library, %d found", m_mergedByLoreCount, int(m_mergedByLore.size()));
            abortMerge(QString());
            return;
        }

        if (m_conflicts.isEmpty() && m_mergedByLore.isEmpty()) {
            // Nothing both sides changed: the merge becomes a version and goes out.
            m_merged = true;
            m_mergeInProgress = true;
            m_toStage.clear();
            m_alreadyStaged.clear();
            m_changedFiles = 0;
            m_mergedFiles = mergedFiles;
            commit();
            return;
        }

        // Text both sides changed in the same lines: the library wrote its merge, with markers,
        // into the working file. The artist's own bytes are in "~mine" until the merge is
        // committed; they go back into the working file now, on a worker.
        const QString project = projectPath();
        const QStringList conflicts = m_conflicts;
        onWorker<GivenBack>([project, conflicts]() { return giveBackMine(project, conflicts, false); },
            [this](const GivenBack &given) {
                if (!given.ok) {
                    abortMerge(given.failedPath);
                    return;
                }
                m_restoreQueue = m_mergedByLore;
                restoreNext();
            });
    }

    /// A file the library merged by itself gets the artist's own bytes back, from the version
    /// this computer created before the merge.
    void restoreNext()
    {
        if (m_restoreQueue.isEmpty()) {
            reportDivergences();
            return;
        }
        const QString path = m_restoreQueue.takeFirst();
        if (!m_restoreFolder) {
            m_restoreFolder.reset(new QTemporaryDir());
        }
        if (!m_restoreFolder->isValid()) {
            abortMerge(path);
            return;
        }
        const QString output = m_restoreFolder->filePath(QStringLiteral("mine"));
        QFile::remove(output);
        const QByteArray file = path.toUtf8();
        const QByteArray revision = m_localRevision.toUtf8();
        const QByteArray outputPath = lorePath(output);
        run(globals(),
            [file, revision, outputPath](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_file_write_args_t args;
                std::memset(&args, 0, sizeof args);
                args.path = toLore(file);
                args.revision = toLore(revision);
                args.output = toLore(outputPath);
                lore_file_write_async(globals, &args, callback);
            },
            nullptr,
            [this, path, output](int status, const QString &rawText) {
                const QString target = QDir(projectPath()).filePath(path);
                const bool written = status == 0;
                onWorker<bool>([target, output, written]() {
                        const bool restored = written && replaceFile(target, output);
                        QFile::remove(output);
                        return restored;
                    },
                    [this, path, status, rawText](const bool &restored) {
                        if (!restored) {
                            qWarning("Lore: could not give back %s (status %d): %s", qUtf8Printable(path), status,
                                     qUtf8Printable(rawText));
                            abortMerge(path);
                            return;
                        }
                        restoreNext();
                    });
            });
    }

    /// The merge cannot leave the artist's own version in the working file: everything goes back
    /// to this computer's last version, and nothing of the server is taken.
    void abortMerge(const QString &path)
    {
        run(globals(),
            [](const lore_global_args_t *globals, lore_event_callback_config_t callback) {
                lore_branch_merge_abort_args_t args;
                std::memset(&args, 0, sizeof args);
                lore_branch_merge_abort_async(globals, &args, callback);
            },
            nullptr,
            [this, path](int status, const QString &rawText) {
                if (status != 0) {
                    qWarning("Lore: the merge could not be put back (status %d): %s", status, qUtf8Printable(rawText));
                }
                fail(KisVersioningJob::Failed, keepMineFailText(path));
            });
    }

    void reportDivergences()
    {
        const QDir root(projectPath());
        for (const QString &path : std::as_const(m_conflicts)) {
            m_result.divergences.append({path, kindOf(root, path), QString()});
        }
        for (const QString &path : std::as_const(m_mergedByLore)) {
            m_result.divergences.append({path, DivergenceKind::BothModified, QString()});
        }
        m_result.pendingUpload = true;
        finish(QVariant::fromValue(m_result));
    }

    void noteReceived(const LoreEvent &event)
    {
        const QString path = event.fields.value(QStringLiteral("path")).toString();
        if (event.fields.value(QStringLiteral("isFile")).toBool()
            && event.fields.value(QStringLiteral("action")).toInt() != LORE_FILE_ACTION_DELETE
            && !m_result.received.contains(path)) {
            m_result.received << path;
        }
    }

    void reportBytes(const LoreEvent &event, int tag, const QString &label)
    {
        if (event.tag == tag) {
            reportProgress(event.fields.value(QStringLiteral("bytesDone")).toLongLong(),
                           event.fields.value(QStringLiteral("bytesTotal")).toLongLong(), label);
        }
    }

    QString m_notes;
    LoreStatus m_status;
    QStringList m_toStage;
    QStringList m_alreadyStaged;   ///< staged by an earlier call, not yet in a version
    int m_changedFiles {0};
    int m_mergedFiles {0};
    bool m_mergeInProgress {false};
    bool m_committed {false};
    bool m_merged {false};
    QString m_localRevision;
    quint64 m_mergedByLoreCount {0};
    QStringList m_conflicts;
    QStringList m_mergedByLore;
    QStringList m_restoreQueue;
    std::unique_ptr<QTemporaryDir> m_restoreFolder;
    SyncResult m_result;
};

class ResolveOperation : public LoreOperation
{
public:
    ResolveOperation(KisVersioningJob *job, const LoreSettings &settings, const QString &localPath, const QString &path,
                     Resolution resolution, const QString &mergedFile)
        : LoreOperation(job, settings, localPath)
        , m_path(loreRelativePath(path))
        , m_resolution(resolution)
        , m_mergedFile(mergedFile)
    {
        setFlushBeforeEnd(true);
    }

    void start()
    {
        if (!requireProject()) {
            return;
        }
        runStatus(false, [this](const LoreStatus &status) { onStatus(status); }, failText());
    }

private:
    QString failText() const
    {
        return i18n("Could not settle the differences in %1.", m_path);
    }

    void onStatus(const LoreStatus &status)
    {
        const LoreStatusFile *entry = nullptr;
        for (const LoreStatusFile &file : status.files) {
            if (file.path == m_path) {
                entry = &file;
                break;
            }
        }
        if (!entry || !entry->conflict) {
            qWarning("Lore: %s is not in a merge", qUtf8Printable(m_path));
            fail(KisVersioningJob::Failed, failText());
            return;
        }
        if (stopIfCancelled()) {
            return;
        }
        if (m_resolution == Resolution::MergedFile) {
            // A merged file takes the place of the working file, which is then kept as it is. The
            // copy runs on a worker.
            if (m_mergedFile.isEmpty()) {
                fail(KisVersioningJob::Failed, failText());
                return;
            }
            const QString target = QDir(projectPath()).filePath(m_path);
            const QString merged = m_mergedFile;
            const bool unresolved = entry->unresolved;
            onWorker<bool>([target, merged]() { return replaceFile(target, merged); },
                [this, unresolved](const bool &replaced) {
                    if (!replaced) {
                        fail(KisVersioningJob::Failed, failText());
                        return;
                    }
                    settleKept(unresolved);
                });
            return;
        }
        if (m_resolution == Resolution::Theirs) {
            // In a merge with the server the library's "mine" is the server's side (the merge's
            // first parent): it writes the server's version into the working file and stages it,
            // a deletion included. Its "theirs" would keep this computer's version.
            settle(withPaths({m_path}, [](const lore_global_args_t *globals, lore_event_callback_config_t callback,
                                         lore_string_array_t paths) {
                lore_branch_merge_resolve_mine_args_t args;
                std::memset(&args, 0, sizeof args);
                args.paths = paths;
                lore_branch_merge_resolve_mine_async(globals, &args, callback);
            }));
        } else {
            settleKept(entry->unresolved);
        }
    }

    /// Settles the path with the working file as it is.
    void settleKept(bool unresolved)
    {
        if (unresolved) {
            // Marks it settled with the working file as it is.
            settle(withPaths({m_path}, [](const lore_global_args_t *globals, lore_event_callback_config_t callback,
                                         lore_string_array_t paths) {
                lore_branch_merge_resolve_args_t args;
                std::memset(&args, 0, sizeof args);
                args.paths = paths;
                lore_branch_merge_resolve_async(globals, &args, callback);
            }));
        } else {
            // Merged by the library by itself, so not open to resolve: the working file is staged.
            settle(withPaths({m_path}, [](const lore_global_args_t *globals, lore_event_callback_config_t callback,
                                         lore_string_array_t paths) {
                lore_file_stage_args_t args;
                std::memset(&args, 0, sizeof args);
                args.paths = paths;
                lore_file_stage_async(globals, &args, callback);
            }));
        }
    }

    void settle(const LoreCall::Starter &starter)
    {
        reportProgress(0, 0, i18n("Settling the differences in %1", m_path));
        run(globals(), starter, nullptr, [this](int status, const QString &rawText) {
            if (status != 0) {
                failFromLore("settling differences", status, rawText, failText());
                return;
            }
            QSet<QString> &kept = settings().memory->keptAfterMerge[projectPath()];
            if (m_resolution == Resolution::Theirs) {
                kept.remove(m_path);
            } else {
                kept.insert(m_path);
            }
            finish();
        });
    }

    QString m_path;
    Resolution m_resolution;
    QString m_mergedFile;
};

} // namespace

KisVersioningJob *startLoreSynchronise(const LoreSettings &settings, const QString &localPath, const QString &notes)
{
    auto *job = new KisVersioningJob();
    auto *operation = new SyncOperation(job, settings, localPath, notes);
    operation->start();
    return job;
}

KisVersioningJob *startLoreResolve(const LoreSettings &settings, const QString &localPath, const QString &path,
                                   Resolution resolution, const QString &mergedFile)
{
    auto *job = new KisVersioningJob();
    auto *operation = new ResolveOperation(job, settings, localPath, path, resolution, mergedFile);
    operation->start();
    return job;
}
